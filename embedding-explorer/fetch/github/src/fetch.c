#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "call.h"
#include "flush.h"
#include "parse_http.h"
#include "sync.h"

// Refer to GH REST API: https://docs.github.com/en/rest/
#define GH_SEARCH                                                              \
  "https://api.github.com/search/"                                             \
  "repositories?q=stars:%s&sort=stars&order=desc&per_page=100&page=%d"
#define GH_REPOS "https://api.github.com/repos/%s/readme"

// search API answers at most 1000 results per query, 100 per page
#define GH_SEARCH_CAP 1000
#define GH_SEARCH_MAX_PAGES 10

/*
 * Wait out the rate limit window, reading the previous response's headers.
 */
void wait_ratelimit(Response resph) {
  if (parse_ratelimit_remaining(resph) != 0) {
    return;
  }
  long reset = parse_ratelimit_reset(resph);
  if (reset <= 0) {
    return;
  }
  long wait = reset - (long)time(NULL) + 1; /* +1s of slack for clock skew */
  if (wait > 0) {
    printf("Rate limit reached, waiting %ld seconds for the window to "
           "reset...\n",
           wait);
    sleep((unsigned int)wait);
  }
}

/*
 * Fetch repos from a range of stars, where star counts fall in [min_stars,
 * max_stars] and sync them into the database.
 *
 *  - max_stars below 0 leaves the range open at the top
 */
int fetch_repos_star_range(int min_stars, int max_stars) {

  // Initialize variables
  Repos *head = NULL; // Head to linked list of repos
  char link[256];     // links
  char stars[32];     // stars range qualifier

  Response resph;  // response header buffer
  Response resp;   // response body buffer
  long status = 0; // HTTP status of the last request
  long total = 0;  // size of the whole result set, not of this page
  int result = 0;  // final result

  // star_range(stars, sizeof(stars), min_stars, max_stars);
  if (max_stars < 0) {
    snprintf(stars, sizeof(stars), "%d..*", min_stars);
  } else {
    snprintf(stars, sizeof(stars), "%d..%d", min_stars, max_stars);
  }

  // Initial request, carries the total
  snprintf(link, sizeof(link), GH_SEARCH, stars, 1);
  if (call_gh_url(link, "", &resph, &resp, &status)) {
    return -1;
  }
  if (status < 200 || status >= 300) {
    fprintf(stderr, "fetch/src/fetch.c (window): stars:%s returned HTTP %ld\n",
            stars, status);
    types_free_response(&resph);
    types_free_response(&resp);
    return -1;
  }
  if (flush_response_repos(&resp, &head, &total)) {
    types_free_response(&resph);
    types_free_response(&resp);
    types_free_repos(head);
    return -1;
  }

  // An open ended window has no ceiling to halve, so the top result supplies
  // one. Results come back sorted by stars descending, so that is the head.
  int top = max_stars;
  if (max_stars < 0) {
    top = head != NULL ? (int)head->stars : min_stars;
  }

  /* DEPRECATED split the window if too many total responses
  if (total > GH_SEARCH_CAP && top > min_stars) {
    types_free_response(&resph);
    types_free_response(&resp);
    types_free_repos(head);

    int mid = min_stars + (top - min_stars) / 2;
    printf("stars:%s holds %ld repos, splitting into %d..%d and %d..%d\n",
           stars, total, mid + 1, top, min_stars, mid);

    int upper = fetch_repos_star_range(mid + 1, top);
    int lower = fetch_repos_star_range(min_stars, mid);
    return upper || lower;
  }
  if (total > GH_SEARCH_CAP) {
    fprintf(stderr,
            "fetch/src/fetch.c (window): stars:%s holds %ld repos and cannot "
            "be split, keeping the first %d\n",
            stars, total, GH_SEARCH_CAP);
  }
  */

  // Page through the rest of the window. Page 1 is already in head, and
  // anything past the cap is refused by the API however many pages it claims.
  int n_pages = parse_page_count(resph);
  if (n_pages > GH_SEARCH_MAX_PAGES) {
    n_pages = GH_SEARCH_MAX_PAGES;
  }

  printf("Fetching %d page(s) of repos with stars:%s (%ld total)...\n", n_pages,
         stars, total);
  for (int page = 2; page <= n_pages; page++) {

    wait_ratelimit(resph);

    // call_gh_url hands back fresh buffers, so the previous pair goes first
    types_free_response(&resph);
    types_free_response(&resp);

    snprintf(link, sizeof(link), GH_SEARCH, stars, page);
    if (call_gh_url(link, "", &resph, &resp, &status)) {
      result = 1;
      break;
    }
    if (status < 200 || status >= 300) {
      fprintf(stderr,
              "fetch/src/fetch.c (window): stars:%s page %d returned HTTP "
              "%ld\n",
              stars, page, status);
      result = 1;
      break;
    }

    if (flush_response_repos(&resp, &head, NULL)) {
      result = 1;
      break;
    }
  }

  /* Clean up */
  types_free_response(&resph);
  types_free_response(&resp);

  if (!result) result = sync_repos(head);
  types_free_repos(head);

  return (result ? -1 : total);
}

int fetch_readme(const char *repo_name, char **content) {
  // Initialize variables
  Response resph;
  Response resp;
  int result = 0;
  char link[256];
  long status = 0;

  // Perform request
  snprintf(link, sizeof(link), GH_REPOS, repo_name);
  if (call_gh_url(link, ".raw", &resph, &resp, &status)) {
    return 1;
  }
  // A repo with no README answers 404 with a JSON error body, which would
  // otherwise be flushed into content and embedded as if it were the readme.
  if (status < 200 || status >= 300) {
    fprintf(stderr,
            "fetch/src/fetch.c (fetch_readme): readme for %s returned HTTP "
            "%ld\n",
            repo_name, status);
    types_free_response(&resph);
    types_free_response(&resp);
    return 1;
  }

  // Flush response into content
  result = flush_response_content(&resp, content);

  /* Clean up */
  types_free_response(&resph);

  types_free_response(&resp);
  return result;
}

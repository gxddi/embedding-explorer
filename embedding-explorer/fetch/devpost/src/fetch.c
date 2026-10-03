#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "call.h"
#include "flush.h"
#include "sync.h"

// Devpost publishes no REST API. The listing its hackathon browser is built on
// answers plain JSON, and every event's gallery is server rendered HTML, so
// the projects are reached through the events rather than searched for.
//
// The endpoints that would search them directly, /software/search and
// /software/built-with, sit behind a WAF that answers an unattended request
// with 202 and an empty body.
#define DP_LISTING "https://devpost.com/api/hackathons?page=%d&per_page=%d"
#define DP_SEARCH DP_LISTING "&search=%s"
#define DP_GALLERY "https://%s.devpost.com/project-gallery?page=%d"
#define DP_PROJECT "https://devpost.com/software/%s"

// The listing clamps per_page to 40 and quietly serves a smaller page rather
// than erroring, so what came back is read out of the response, not assumed.
#define DP_PER_PAGE 40

// A gallery serves 24 cards a page and answers past its end with an empty one,
// which is what stops the walk. The cap is only here so a gallery that keeps
// answering cannot run forever.
#define DP_GALLERY_MAX_PAGES 200

// Neither endpoint sends rate limit headers, so there is nothing to wait on
// and nothing to be told when the walk is going too fast.
#define DP_PAGE_DELAY_SECONDS 1

/*
 * Walk one hackathon's gallery and sync the projects in it.
 *
 * NOTE: The gallery is the unit of work, the way a star window is on the
 * github side: it is collected whole and written in one go. An event that has
 * not been submitted to yet serves an empty gallery, which syncs nothing.
 *
 *  - hackathon and year tag every project synced, NULL and 0 leave them unset
 */
static int gallery(const char *host, const char *hackathon, int year) {

  Projects *head = NULL; // Head to linked list of projects
  char link[256];        // links

  Response resp;   // response body buffer
  long status = 0; // HTTP status of the last request
  int found = 0;   // cards on the page just read

  int result = 0; // final result

  for (int page = 1; page <= DP_GALLERY_MAX_PAGES; page++) {

    // Every request, not just the ones past the first page. Most galleries are
    // one page, so pacing only within a gallery would leave the walk across
    // thousands of them unpaced.
    sleep(DP_PAGE_DELAY_SECONDS);

    snprintf(link, sizeof(link), DP_GALLERY, host, page);
    if (dp_call_url(link, &resp, &status)) {
      result = 1;
      break;
    }
    // A private or retired event answers 404 with a normal error page, which
    // holds no cards and would otherwise read as an empty gallery
    if (status < 200 || status >= 300) {
      fprintf(stderr,
              "fetch/devpost/src/fetch.c (gallery): %s page %d returned HTTP "
              "%ld\n",
              host, page, status);
      dp_types_free_response(&resp);
      result = 1;
      break;
    }

    if (dp_flush_gallery_projects(&resp, &head, &found)) {
      dp_types_free_response(&resp);
      result = 1;
      break;
    }
    dp_types_free_response(&resp);

    // Past the end of the gallery the page still answers, it just has nothing
    // on it. That is the only end marker there is: no count is published.
    if (found == 0) {
      break;
    }
  }

  for (Projects *cur = head; cur != NULL; cur = cur->next) {
    cur->hackathon = hackathon;
    cur->year = year;
  }

  if (head != NULL) {
    result |= dp_sync_projects(head);
    dp_types_free_projects(head);
  }

  return result;
}

/*
 * Fetch the projects submitted to devpost's hackathons and sync them into the
 * database.
 *
 *  - max_hackathons below 0 walks every hackathon devpost lists
 *
 * NOTE: The listing is read one page at a time and each event on it is walked
 * before the next page is asked for, so what is held at once is one page of
 * subdomains and one gallery of projects. Collecting all of them first would
 * mean thousands of requests before the first row is written.
 */
int fetch_projects(int max_hackathons) {

  char link[256];    // links
  Response resp;     // response body buffer
  long status = 0;   // HTTP status of the last request
  long total = 0;    // size of the whole listing, not of this page
  long per_page = 0; // page size the listing actually served

  int done = 0;   // hackathons walked so far
  int result = 0; // final result

  for (int page = 1; max_hackathons < 0 || done < max_hackathons; page++) {

    Hosts *hosts = NULL;

    snprintf(link, sizeof(link), DP_LISTING, page, DP_PER_PAGE);
    if (dp_call_url(link, &resp, &status)) {
      return 1;
    }
    if (status < 200 || status >= 300) {
      fprintf(stderr,
              "fetch/devpost/src/fetch.c (fetch_projects): listing page %d "
              "returned HTTP %ld\n",
              page, status);
      dp_types_free_response(&resp);
      return 1;
    }
    if (dp_flush_response_hosts(&resp, &hosts, &total, &per_page)) {
      dp_types_free_response(&resp);
      return 1;
    }
    dp_types_free_response(&resp);

    if (page == 1) {
      printf("Walking the galleries of %ld hackathon(s)...\n",
             max_hackathons < 0 || max_hackathons > total ? total
                                                          : max_hackathons);
    }

    for (Hosts *cur = hosts; cur != NULL; cur = cur->next) {
      if (max_hackathons >= 0 && done >= max_hackathons) {
        break;
      }
      result |= gallery(cur->name, NULL, 0);
      done++;
    }
    dp_types_free_hosts(hosts);

    // The listing ran out. per_page is what came back rather than what was
    // asked for, so an empty page is the honest end either way.
    if (per_page <= 0 || (long)page * per_page >= total) {
      break;
    }
  }

  printf("Walked %d hackathon gallery(s).\n", done);
  return result;
}

/*
 * Whether a listing title belongs to the named hackathon: it starts with the
 * name, case aside. A search for "TreeHacks" also answers "Hortonworks @
 * TreeHacks", which is a different event.
 */
static int titled(const char *title, const char *name) {
  return strncasecmp(title, name, strlen(name)) == 0;
}

/*
 * Fetch the projects submitted to one hackathon, every year of it or one, and
 * sync them into the database tagged with id and the year.
 *
 * NOTE: The listing's search narrows thousands of events to a few pages, so
 * this walks the galleries of only the events titled name rather than the
 * whole listing.
 */
int fetch_hackathon_projects(const char *id, const char *name, int year) {

  char link[512];    // links
  char search[128];  // name with its spaces as the query string wants them
  Response resp;     // response body buffer
  long status = 0;   // HTTP status of the last request
  long total = 0;    // size of the whole search, not of this page
  long per_page = 0; // page size the listing actually served

  int done = 0;   // galleries walked so far
  int result = 0; // final result

  size_t n = 0;
  for (const char *c = name; *c != '\0' && n + 1 < sizeof(search); c++) {
    search[n++] = *c == ' ' ? '+' : (char)tolower((unsigned char)*c);
  }
  search[n] = '\0';

  for (int page = 1;; page++) {

    Hosts *hosts = NULL;

    snprintf(link, sizeof(link), DP_SEARCH, page, DP_PER_PAGE, search);
    if (dp_call_url(link, &resp, &status)) {
      return 1;
    }
    if (status < 200 || status >= 300) {
      fprintf(stderr,
              "fetch/devpost/src/fetch.c (fetch_hackathon_projects): %s page "
              "%d returned HTTP %ld\n",
              name, page, status);
      dp_types_free_response(&resp);
      return 1;
    }
    if (dp_flush_response_hosts(&resp, &hosts, &total, &per_page)) {
      dp_types_free_response(&resp);
      return 1;
    }
    dp_types_free_response(&resp);

    for (Hosts *cur = hosts; cur != NULL; cur = cur->next) {
      if (!titled(cur->title, name) || (year > 0 && cur->year != year)) {
        continue;
      }
      result |= gallery(cur->name, id, cur->year);
      done++;
    }
    dp_types_free_hosts(hosts);

    if (per_page <= 0 || (long)page * per_page >= total) {
      break;
    }
  }

  printf("Walked %d %s gallery(s).\n", done, name);
  return result;
}

/*
 * Fetch one project's write-up, given the slug the gallery handed back
 */
int fetch_project_description(const char *slug, char **content, char **prizes) {
  // Initialize variables
  Response resp;
  int result = 0;
  char link[256];
  long status = 0;

  // Perform request
  snprintf(link, sizeof(link), DP_PROJECT, slug);
  if (dp_call_url(link, &resp, &status)) {
    return 1;
  }
  // A project that has been taken down answers 404 with an error page, which
  // would otherwise be flushed into content and embedded as if it were the
  // write-up.
  if (status < 200 || status >= 300) {
    fprintf(stderr,
            "fetch/devpost/src/fetch.c (fetch_project_description): %s "
            "returned HTTP %ld\n",
            slug, status);
    dp_types_free_response(&resp);
    return 1;
  }

  // Prizes first, the description flush empties the page
  if (prizes != NULL && dp_flush_response_prizes(&resp, prizes)) {
    dp_types_free_response(&resp);
    return 1;
  }

  // Flush the write-up out of the page into content
  result = dp_flush_response_description(&resp, content);

  /* Clean up */
  dp_types_free_response(&resp);

  return result;
}

/*
 * Fetch the prizes one project won, given the slug the gallery handed back
 */
int fetch_project_prizes(const char *slug, char **prizes) {
  Response resp;
  char link[256];
  long status = 0;

  snprintf(link, sizeof(link), DP_PROJECT, slug);
  if (dp_call_url(link, &resp, &status)) {
    return 1;
  }
  if (status < 200 || status >= 300) {
    fprintf(stderr,
            "fetch/devpost/src/fetch.c (fetch_project_prizes): %s returned "
            "HTTP %ld\n",
            slug, status);
    dp_types_free_response(&resp);
    return 1;
  }

  int result = dp_flush_response_prizes(&resp, prizes);
  dp_types_free_response(&resp);
  return result;
}

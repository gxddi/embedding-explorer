#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "flush.h"
#include "parse_html.h"

// One gallery card, and the fields read out of it
#define DP_CARD "data-software-id=\""
#define DP_SLUG "href=\"https://devpost.com/software/"
#define DP_MEMBER "data-url=\"https://devpost.com/"
#define DP_LIKES "data-count=\"like\">"
#define DP_WINNER_BADGE "class=\"winner\""

// Each prize won, as the project page's "Submitted to" column lists it
#define DP_SUBMISSIONS "id=\"submissions\""
#define DP_PRIZE "class=\"winner label"

// The write-up on a project page, which runs from the top of the left column
// down to the tag list under it
#define DP_DETAILS "id=\"app-details-left\">"
#define DP_BUILT_WITH "id=\"built-with\""
#define DP_DETAILS_END "id=\"app-details-right\""

/*
 * Whether needle occurs inside [from, limit)
 */
static int contains(const char *from, const char *limit, const char *needle) {
  size_t n = strlen(needle);
  for (const char *at = from; at + n <= limit; at++) {
    if (memcmp(at, needle, n) == 0) {
      return 1;
    }
  }
  return 0;
}

/*
 * Reset resp to an empty buffer, so the next page writes into a fresh one.
 */
static int reset(Response *resp) {
  free(resp->data);
  resp->data = (char *)malloc(1);
  if (resp->data == NULL) {
    fprintf(stderr, "fetch/devpost/src/flush.c (reset): out of memory\n");
    resp->size = 0;
    return 1;
  }
  resp->data[0] = '\0';
  resp->size = 0;
  return 0;
}

/*
 * Travel down a list until the empty slot past its tail.
 *
 * NOTE: Stopping on the tail itself rather than on its next pointer would have
 * the first append overwrite it, dropping and leaking one entry per page.
 */
#define TAIL_OF(type, head, cur)                                              \
  type **cur = (head);                                                        \
  while (*cur != NULL) {                                                      \
    cur = &(*cur)->next;                                                      \
  }

/*
 * Cut the subdomain out of a hackathon's url, which is where its gallery
 * lives: "https://xprize.devpost.com/" -> "xprize".
 *
 * -> Returns NULL for an event parked on its own domain, which has no gallery
 *    to walk in the shape the rest of this expects.
 */
static char *host_of(const char *url) {
  return dp_parse_between(url, url + strlen(url), "https://", ".devpost.com",
                          NULL);
}

/*
 * Empty response and process and load(flush) the hackathon listing in resp
 * into the hosts list
 */
int dp_flush_response_hosts(Response *resp, Hosts **hosts, long *total,
                            long *per_page) {
  TAIL_OF(Hosts, hosts, cur)

  // Get raw (root object)
  cJSON *raw = cJSON_Parse(resp->data);
  if (raw == NULL) {
    fprintf(stderr, "dp_flush_response_hosts: response has nothing\n");
    reset(resp);
    return 1;
  }

  // Size of the whole listing and the page size that produced it. Asking for a
  // bigger page than the listing allows is answered with a smaller one, so the
  // page count has to be worked out from what came back.
  cJSON *meta = cJSON_GetObjectItem(raw, "meta");
  if (total != NULL) {
    cJSON *count = cJSON_GetObjectItem(meta, "total_count");
    *total = cJSON_IsNumber(count) ? (long)count->valuedouble : 0;
  }
  if (per_page != NULL) {
    cJSON *size = cJSON_GetObjectItem(meta, "per_page");
    *per_page = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
  }

  // Get hackathons (array)
  cJSON *items = cJSON_GetObjectItem(raw, "hackathons");
  if (items == NULL) {
    fprintf(stderr,
            "dp_flush_response_hosts: response has no \"hackathons\" array\n");
    cJSON_Delete(raw);
    reset(resp);
    return 1;
  }

  // The subdomain is where the gallery is, the title and year are what a
  // caller looking for one hackathon picks it out by.
  cJSON *item = NULL;
  cJSON_ArrayForEach(item, items) {
    cJSON *url = cJSON_GetObjectItem(item, "url");
    if (!cJSON_IsString(url)) {
      continue;
    }
    char *host = host_of(url->valuestring);
    if (host == NULL) {
      continue;
    }

    // "Jan 17 - 18, 2026": the year is the last four digits of the dates
    cJSON *title = cJSON_GetObjectItem(item, "title");
    cJSON *dates = cJSON_GetObjectItem(item, "submission_period_dates");
    int year = 0;
    if (cJSON_IsString(dates) && strlen(dates->valuestring) >= 4) {
      year = atoi(dates->valuestring + strlen(dates->valuestring) - 4);
    }

    *cur = (Hosts *)malloc(sizeof(Hosts));
    (*cur)->name = host;
    (*cur)->title = strdup(cJSON_IsString(title) ? title->valuestring : "");
    (*cur)->year = year;
    (*cur)->next = NULL;
    cur = &(*cur)->next;
  }

  /* Clean up */
  cJSON_Delete(raw);

  return reset(resp);
}

/*
 * Empty response and process and load(flush) the gallery page in resp into the
 * projects list
 */
int dp_flush_gallery_projects(Response *resp, Projects **projects, int *found) {
  TAIL_OF(Projects, projects, cur)

  const char *page = resp->data;
  const char *end = page + resp->size;
  int cards = 0;

  // Walk card to card. Every field is read inside the card it belongs to,
  // since an unbounded search would happily pick up the next card's instead.
  for (const char *card = strstr(page, DP_CARD); card != NULL;) {

    const char *stop = strstr(card + strlen(DP_CARD), DP_CARD);
    if (stop == NULL) {
      stop = end;
    }

    char *slug = dp_parse_between(card, stop, DP_SLUG, "\"", NULL);
    if (slug == NULL) {
      card = stop != end ? stop : NULL; // a card with no link is not a project
      continue;
    }

    *cur = (Projects *)malloc(sizeof(Projects));
    (*cur)->id = dp_parse_number(card, stop, DP_CARD, "\"");
    (*cur)->slug = slug;
    (*cur)->name = dp_parse_between(card, stop, "<h5>", "</h5>", NULL);
    (*cur)->tagline =
        dp_parse_between(card, stop, "class=\"small tagline\">", "</p>", NULL);
    (*cur)->likes = dp_parse_number(card, stop, DP_LIKES, "</span>");
    (*cur)->hackathon = NULL;
    (*cur)->year = 0;
    (*cur)->winner = contains(card, stop, DP_WINNER_BADGE);

    // Both arrive wrapped in the markup that lays them out
    if ((*cur)->name != NULL) {
      dp_parse_text((*cur)->name);
    }
    if ((*cur)->tagline != NULL) {
      dp_parse_text((*cur)->tagline);
    }

    // Every member of the team that submitted it, in the order shown
    Members **member = &(*cur)->members;
    *member = NULL;
    const char *at = card;
    while (at != NULL && at < stop) {
      char *name = dp_parse_between(at, stop, DP_MEMBER, "\"", &at);
      if (name == NULL) {
        break;
      }
      *member = (Members *)malloc(sizeof(Members));
      (*member)->name = name;
      (*member)->next = NULL;
      member = &(*member)->next;
    }

    (*cur)->next = NULL;
    cur = &(*cur)->next;
    cards++;

    card = stop != end ? stop : NULL;
  }

  if (found != NULL) {
    *found = cards;
  }

  return reset(resp);
}

/*
 * Empty response and load(flush) the project's write-up out of resp into cont
 */
int dp_flush_response_description(Response *resp, char **cont) {
  const char *page = resp->data;
  const char *end = page + resp->size;

  // The write-up ends where the tag list starts. A project that lists nothing
  // it was built with has no such block, so the column's own end serves.
  *cont = dp_parse_between(page, end, DP_DETAILS, DP_BUILT_WITH, NULL);
  if (*cont == NULL) {
    *cont = dp_parse_between(page, end, DP_DETAILS, DP_DETAILS_END, NULL);
  }
  if (*cont == NULL) {
    fprintf(stderr,
            "fetch/devpost/src/flush.c (dp_flush_response_description): page "
            "carries no write-up\n");
    reset(resp);
    return 1;
  }

  // What comes out is a column of markup, and what the model wants is the
  // prose it renders as
  dp_parse_text(*cont);

  reset(resp);
  return 0;
}

/*
 * Load the prizes won out of resp into prizes, leaving resp as it is
 */
int dp_flush_response_prizes(Response *resp, char **prizes) {
  const char *end = resp->data + resp->size;
  const char *at = strstr(resp->data, DP_SUBMISSIONS);

  size_t size = 0;
  *prizes = (char *)calloc(1, 1);
  if (*prizes == NULL) {
    return 1;
  }

  // Each badge is followed by the prize's name, up to the end of its item
  while (at != NULL && (at = strstr(at, DP_PRIZE)) != NULL && at < end) {
    char *name = dp_parse_between(at, end, "</span>", "</li>", &at);
    if (name == NULL) {
      break;
    }
    dp_parse_text(name);
    char *first = name;
    while (*first == ' ' || *first == '\n') {
      first++;
    }
    size_t len = strlen(first);
    while (len > 0 && (first[len - 1] == ' ' || first[len - 1] == '\n')) {
      len--;
    }
    if (len > 0) {
      char *grown = (char *)realloc(*prizes, size + len + 2);
      if (grown == NULL) {
        free(name);
        return 1;
      }
      *prizes = grown;
      if (size > 0) {
        (*prizes)[size++] = '\n';
      }
      memcpy(*prizes + size, first, len);
      size += len;
      (*prizes)[size] = '\0';
    }
    free(name);
  }

  return 0;
}

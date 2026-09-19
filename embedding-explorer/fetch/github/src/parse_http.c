#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <strings.h>

#include "parse_http.h"

/*
 * Locate a header's value in a raw header block, or NULL if absent. Matched
 * case-insensitively and only at the start of a line, so a name appearing
 * inside another header's value does not match.
 */
static const char *find_header(const char *headers, const char *name) {
  size_t len = strlen(name);

  for (const char *p = headers; p != NULL; p = strchr(p, '\n')) {
    if (p != headers) {
      p++; // step off the newline onto the line it terminates
    }
    if (strncasecmp(p, name, len) == 0 && p[len] == ':') {
      return p + len + 1 + strspn(p + len + 1, " \t");
    }
  }

  return NULL;
}

/*
 * Return the number of pages that came with a response, by looking at it's
 * header.
 *
 * link looks like: <url>; rel="name", ..., <url>; rel="name". Ideally we'd find
 * where "name" is "last", but fallback to where "name" is "prev"
 */
int parse_page_count(Response resph) {

  const char *link = find_header(resph.data, "link"); // Link header value
  if (link == NULL) { // No Link header means the result was not paginated
    return 1;
  }
  const char *line_end = strchr(link, '\n'); // End of link header
  if (line_end == NULL) {                    // Response header is malformed
    return 1;
  }

  const char *prev = NULL;
  for (const char *p = link; p < (line_end - 10); p++) {
    if (strncmp(p, "rel=\"last\"", 10) == 0) { // finding "last"
      while (p > link) {
        if (strncmp(p, "page=", 5) == 0 && (p[-1] == '&' || p[-1] == '?'))
          return atoi(p + 5);
        p--;
      }
    } else if (strncmp(p, "rel=\"prev\"", 10) == 0) { // "prev" as fallback
      while (p > link) {
        if (strncmp(p, "page=", 5) == 0 && (p[-1] == '&' || p[-1] == '?'))
          prev = p + 5;
        p--;
      }
    }
  }

  if (prev != NULL) {
    return (atoi(prev) + 1);
  } else {
    return 1;
  }
}

/*
 * Return how many requests are left in the current rate limit window, or -1
 * if the header is absent
 */
int parse_ratelimit_remaining(Response resph) {

  const char *value = find_header(resph.data, "x-ratelimit-remaining");
  if (value == NULL) {
    return -1;
  }
  return atoi(value);
}

/*
 * Return the Unix time at which the current rate limit window resets, or -1 if
 * the header is absent
 */
long parse_ratelimit_reset(Response resph) {
  const char *value = find_header(resph.data, "x-ratelimit-reset");
  if (value == NULL) {
    return -1;
  }
  return atol(value);
}

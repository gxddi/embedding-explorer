#ifndef PARSE_HTTP_H
#define PARSE_HTTP_H

#include "types.h"

/*
 * Locate a header's value in a raw header block, or NULL if absent. Matched
 * case-insensitively and only at the start of a line, so a name appearing
 * inside another header's value does not match.
 */
static const char *find_header(const char *headers, const char *name);

/* Read a header whose whole value is a number. Returns -1 when the header is
 * absent, which keeps it distinct from a legitimate value of 0. */
static long header_long(Response resph, const char *name);

/*
 * Read the "page" query parameter out of a URL, or -1 if absent.
 *
 * Matches only on a '?' or '&' delimiter so that "per_page=100" - which
 * contains "page=" as a substring - is not mistaken for it.
 */
static int page_param(const char *url, const char *end);

/*
 * Return the number of pages that came with a response, by looking at it's
 * header
 */
int parse_page_count(Response resph);

/*
 * Return how many requests are left in the current rate limit window, or -1
 * if the header is absent
 */
int parse_ratelimit_remaining(Response resph);

/*
 * Return the Unix time at which the current rate limit window resets, or -1 if
 * the header is absent
 */
long parse_ratelimit_reset(Response resph);

#endif

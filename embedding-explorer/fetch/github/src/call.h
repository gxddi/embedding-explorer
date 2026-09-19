#ifndef CALL_H
#define CALL_H

#include "types.h"

/*
 * Fetch content from gh endpoint via their REST api and load the response
 * header into resph and response into resp
 *
 *  - status receives the HTTP status code, or 0 when the exchange never
 *    completed
 *
 * -> Returns the curl error code. A 0 return only means the exchange happened,
 *    status still has to be checked: GitHub answers 404/403/422 with a JSON
 *    error body that would otherwise be flushed through as if it were data.
 *    Either way the caller may safely types_free_response both buffers
 *    afterwards.
 */
int call_gh_url(const char *url, const char *media, Response *resph,
                Response *resp, long *status);

#endif

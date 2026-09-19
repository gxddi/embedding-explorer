#ifndef DP_CALL_H
#define DP_CALL_H

#include "types.h"

/*
 * Fetch content from a devpost url and load the body into resp.
 *
 *  - status receives the HTTP status code, or 0 when the exchange never
 *    completed
 *
 * NOTE: No header buffer, unlike the github side. Devpost sends no Link header
 * and no rate limit headers, so there is nothing in them worth parsing: the
 * page count comes out of the body's meta object instead.
 *
 * -> Returns the curl error code. A 0 return only means the exchange happened,
 *    status still has to be checked: devpost answers a blocked request with
 *    202 and an empty body, which would otherwise be flushed through as data.
 *    Either way the caller may safely dp_types_free_response afterwards.
 */
int dp_call_url(const char *url, Response *resp, long *status);

#endif

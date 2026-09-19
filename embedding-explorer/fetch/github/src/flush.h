#ifndef FLUSH_H
#define FLUSH_H

#include "types.h"

/*
 * Empty response and process and load(flush) resp into cur
 * Takes a pointer to a NULL repos head or tail
 *
 *  - total receives the search's total_count, the size of the whole result set
 *    rather than of this page. NULL when the caller does not care.
 */
int flush_response_repos(Response *resp, Repos **repos, long *total);

/*
 * Empty response and load(flush) resp's body into cont. With the .raw media
 * type the body is the file content itself, not JSON.
 */
int flush_response_content(Response *resp, char **cont);

#endif

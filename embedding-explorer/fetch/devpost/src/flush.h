#ifndef DP_FLUSH_H
#define DP_FLUSH_H

#include "types.h"

/*
 * Empty response and process and load(flush) the hackathon listing in resp
 * into the hosts list. Takes a pointer to a NULL hosts head or tail.
 *
 *  - total receives the listing's total_count, the size of the whole result
 *    set rather than of this page. NULL when the caller does not care.
 *  - per_page receives the page size the listing actually used, which is not
 *    always the one that was asked for. NULL when the caller does not care.
 */
int dp_flush_response_hosts(Response *resp, Hosts **hosts, long *total,
                            long *per_page);

/*
 * Empty response and process and load(flush) the gallery page in resp into the
 * projects list. Takes a pointer to a NULL projects head or tail.
 *
 *  - found receives the number of cards on the page, which is how the caller
 *    tells a short last page from a full one. NULL when the caller does not
 *    care.
 *
 * NOTE: The gallery is HTML, not JSON. Devpost serves it rendered, so the
 * fields are read straight off the cards rather than parsed out of a document.
 */
int dp_flush_gallery_projects(Response *resp, Projects **projects, int *found);

/*
 * Empty response and load(flush) the project's write-up out of resp into cont.
 * The body is the whole project page, so the write-up is cut out of it and
 * flattened to the text it renders as.
 */
int dp_flush_response_description(Response *resp, char **cont);

#endif

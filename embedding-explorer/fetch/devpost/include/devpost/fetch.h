#ifndef DEVPOST_FETCH_H
#define DEVPOST_FETCH_H

#ifdef __cplusplus
extern "C" {
#endif
/*
 * Fetch the projects submitted to devpost's hackathons and sync them into the
 * database.
 *
 *  - max_hackathons below 0 walks every hackathon devpost lists
 *
 * NOTE: Devpost has no project search worth calling. The endpoint the site's
 * own gallery uses sits behind a WAF that answers an unattended request with
 * 202 and an empty body, and the one page that does answer ignores its page
 * parameter. What is left is the per hackathon galleries, which are plain
 * server rendered HTML, so projects are reached one event at a time and the
 * hackathon listing is walked only to find them.
 */
int fetch_projects(int max_hackathons);

/*
 * Fetch one project's write-up, given the slug the gallery handed back. The
 * page it lives on is assembled from the slug, the same way a repo's readme
 * url is assembled from its name.
 *
 *  - content receives a fresh buffer the caller frees
 */
int fetch_project_description(const char *slug, char **content);
#ifdef __cplusplus
}
#endif

#endif

#ifndef FETCH_H
#define FETCH_H

#ifdef __cplusplus
extern "C" {
#endif
/*
 * Fetch repos from a range of stars, where star counts fall in [min_stars,
 * max_stars] and sync them into the database.
 *
 *  - max_stars below 0 leaves the range open at the top
 *
 * NOTE: The search API never returns more than 1000 results per query despite
 * pagination potentially hinting at more, so a window holding more than that
 * is halved until each piece fits. One extra request per split.
 */
int fetch_repos_star_range(int min_stars, int max_stars);

/*
 *
 */
int fetch_readme(const char *repo_name, char **content);

#ifdef __cplusplus
}
#endif

#endif

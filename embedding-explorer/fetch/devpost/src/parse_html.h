#ifndef DP_PARSE_HTML_H
#define DP_PARSE_HTML_H

/*
 * The github side parses response headers. Devpost's galleries and project
 * pages are the response body, so this is the same slot filled with the three
 * things that gets asked of markup: cut a span out of it, read a number out of
 * it, and turn a span of it into the text it renders as.
 */

/*
 * Copy out what sits between open and close, searching only inside
 * [from, limit).
 *
 *  - after, when not NULL, receives the position just past close, so a caller
 *    reading a repeated field can carry on from there
 *
 * -> Returns a fresh buffer the caller frees, or NULL when either marker is
 *    missing.
 */
char *dp_parse_between(const char *from, const char *limit, const char *open,
                       const char *close, const char **after);

/*
 * Read the digits out of what sits between open and close. Separators are
 * skipped, so "1,024" reads as 1024.
 *
 * -> Returns 0 when either marker is missing, which is also what a genuine
 *    zero reads as. Nothing here needs to tell those apart.
 */
long dp_parse_number(const char *from, const char *limit, const char *open,
                     const char *close);

/*
 * Turn a span of markup into the text it renders as, in place: tags dropped,
 * entities resolved, runs of whitespace collapsed to one space.
 *
 * NOTE: Every step only ever shortens the buffer, so this needs no second
 * allocation. Block level tags become a newline rather than nothing, so the
 * headings a devpost write-up is built out of do not run into the paragraph
 * under them.
 */
void dp_parse_text(char *html);

#endif

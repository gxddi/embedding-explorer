#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "parse_html.h"

// Tags that separate one block of prose from the next. A write-up is mostly
// headings and paragraphs, and without this they would be run together.
static const char *const BREAKS[] = {"p",  "div", "br", "li", "h1", "h2",
                                     "h3", "h4",  "h5", "h6", "tr", "table"};

// Entities devpost's markup actually contains. Anything else is left as it is
// rather than guessed at.
static const struct {
  const char *entity;
  const char *text;
} ENTITIES[] = {{"&lt;", "<"},   {"&gt;", ">"},    {"&quot;", "\""},
                {"&#39;", "'"},  {"&apos;", "'"},  {"&nbsp;", " "},
                {"&amp;", "&"}};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Find needle in [from, limit), rather than in the rest of the buffer. The
 * document is one string, so an unbounded search would happily read a field
 * out of the next card along.
 */
static const char *find_in(const char *from, const char *limit,
                           const char *needle) {
  size_t len = strlen(needle);
  if (from == NULL || limit == NULL || (size_t)(limit - from) < len) {
    return NULL;
  }

  for (const char *p = from; p <= limit - len; p++) {
    if (memcmp(p, needle, len) == 0) {
      return p;
    }
  }
  return NULL;
}

/*
 * Is name the tag that opens or closes at p, which points just past the '<'?
 */
static int tag_is(const char *p, const char *name) {
  if (*p == '/') {
    p++;
  }
  size_t len = strlen(name);
  if (strncasecmp(p, name, len) != 0) {
    return 0;
  }
  // Only a delimiter may follow, so <p> and <pre> do not read as the same tag
  char after = p[len];
  return after == '>' || after == ' ' || after == '\t' || after == '\n' ||
         after == '\r' || after == '/';
}

/*
 * Copy out what sits between open and close, searching only inside
 * [from, limit)
 */
char *dp_parse_between(const char *from, const char *limit, const char *open,
                       const char *close, const char **after) {
  const char *start = find_in(from, limit, open);
  if (start == NULL) {
    return NULL;
  }
  start += strlen(open);

  const char *end = find_in(start, limit, close);
  if (end == NULL) {
    return NULL;
  }

  if (after != NULL) {
    *after = end + strlen(close);
  }
  return strndup(start, (size_t)(end - start));
}

/*
 * Read the digits out of what sits between open and close
 */
long dp_parse_number(const char *from, const char *limit, const char *open,
                     const char *close) {
  char *text = dp_parse_between(from, limit, open, close, NULL);
  if (text == NULL) {
    return 0;
  }

  long out = 0;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p >= '0' && *p <= '9') {
      out = out * 10 + (*p - '0');
    }
  }

  free(text);
  return out;
}

/*
 * Turn a span of markup into the text it renders as, in place
 */
void dp_parse_text(char *html) {
  char *out = html;
  int space = 1; // leading whitespace has nothing to separate, so it is dropped

  for (const char *p = html; *p != '\0';) {

    // Tags: dropped, but the ones that end a block leave a newline behind
    if (*p == '<') {
      const char *end = strchr(p, '>');
      if (end == NULL) {
        break; // truncated markup, nothing past here is trustworthy
      }
      for (size_t i = 0; i < COUNT(BREAKS); i++) {
        if (tag_is(p + 1, BREAKS[i])) {
          if (!space) {
            *out++ = '\n';
            space = 1;
          } else if (out > html && out[-1] == ' ') {
            // "builder </p>" is a block ending, the space before the tag does
            // not make it any less of one, so it is promoted rather than kept
            out[-1] = '\n';
          }
          break;
        }
      }
      p = end + 1;
      continue;
    }

    // Entities: resolved where they are known, passed through where they
    // are not. The replacement is never longer than what it replaces.
    if (*p == '&') {
      size_t i = 0;
      for (; i < COUNT(ENTITIES); i++) {
        size_t len = strlen(ENTITIES[i].entity);
        if (strncmp(p, ENTITIES[i].entity, len) == 0) {
          for (const char *c = ENTITIES[i].text; *c != '\0'; c++) {
            *out++ = *c;
          }
          space = 0;
          p += len;
          break;
        }
      }
      if (i < COUNT(ENTITIES)) {
        continue;
      }
    }

    // Whitespace: collapsed, since the markup is indented and a write-up
    // pulled straight out of it is otherwise mostly blanks
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      if (!space) {
        *out++ = ' ';
        space = 1;
      }
      p++;
      continue;
    }

    *out++ = *p++;
    space = 0;
  }

  // Trailing separator has nothing after it to separate
  while (out > html && (out[-1] == ' ' || out[-1] == '\n')) {
    out--;
  }
  *out = '\0';
}

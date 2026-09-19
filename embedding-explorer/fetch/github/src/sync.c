#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sync.h"

#ifndef EXPLORER_DB_PATH
#define EXPLORER_DB_PATH "../data/main.db"
#endif

/*
 * Join a repo's topics into one space separated string, since sqlite has no
 * list type. Returns NULL when the repo has no topics or on allocation
 * failure, both of which store a NULL topics column.
 */
static char *join_topics(const Topics *topics) {
  size_t size = 0;
  for (const Topics *t = topics; t != NULL; t = t->next) {
    size += strlen(t->name) + 1; // + separator, the last one covers the NUL
  }
  if (size == 0) {
    return NULL;
  }

  char *out = (char *)malloc(size);
  if (out == NULL) {
    return NULL;
  }

  char *p = out;
  for (const Topics *t = topics; t != NULL; t = t->next) {
    p += sprintf(p, p == out ? "%s" : " %s", t->name);
  }
  return out;
}

// Sync linked list of repos with repos database
int sync_repos(Repos *repos) {

  sqlite3 *handle;

  // Open database
  if (sqlite3_open(EXPLORER_DB_PATH, &handle) != SQLITE_OK) {
    fprintf(stderr, "Initial open failed: %s\n", sqlite3_errmsg(handle));
    return 1;
  }

  // WAL so the server thread can read this table while this one writes it, and
  // a timeout so the loser of a write race retries instead of erroring out.
  // The journal mode lives in the file header, the timeout is per connection.
  sqlite3_exec(handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(handle, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

  // Check if table exists and create it if it doesn't
  const char *schema = "CREATE TABLE IF NOT EXISTS repos ("
                       "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  name TEXT NOT NULL UNIQUE,"
                       "  desc TEXT,"
                       "  stars INTEGER,"
                       "  topics TEXT," // space separated, sqlite has no list
                       "  vector BLOB"
                       ");";

  char *err = NULL;
  if (sqlite3_exec(handle, schema, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "create failed: %s\n", err);
    sqlite3_free(err); // must free the error message
    sqlite3_close(handle);
    return 1;
  }

  // Create a bindable sql statement where you check if a row of the repo name
  // exists and if it has the same description and star count, if not you update
  // the row with that name, creating it if the name does not exist.
  //
  // Both columns have to be listed in the SET and in the WHERE. Leaving stars
  // out of the SET would keep a stale count forever, and leaving it out of the
  // WHERE would skip the update for a repo that only gained stars.
  sqlite3_stmt *stmt;
  const char *upsert =
      "INSERT INTO repos (name, desc, stars, topics) VALUES (?1, ?2, ?3, ?4) "
      "ON CONFLICT(name) DO UPDATE SET desc = excluded.desc, "
      "stars = excluded.stars, topics = excluded.topics "
      "WHERE desc IS NOT excluded.desc OR stars IS NOT excluded.stars "
      "OR topics IS NOT excluded.topics;";

  if (sqlite3_prepare_v2(handle, upsert, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(handle));
    sqlite3_close(handle);
    return 1;
  }

  // Iterate over the linked list, reusing the prepared statement per row.
  for (Repos *cur = repos; cur != NULL; cur = cur->next) {
    sqlite3_bind_text(stmt, 1, cur->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cur->desc, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)cur->stars);
    // TRANSIENT so sqlite copies the join and it can be freed right away
    char *topics = join_topics(cur->topics);
    sqlite3_bind_text(stmt, 4, topics, -1, SQLITE_TRANSIENT);
    free(topics);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
      fprintf(stderr, "upsert failed: %s\n", sqlite3_errmsg(handle));
      sqlite3_finalize(stmt);
      sqlite3_close(handle);
      return 1;
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3_finalize(stmt);
  sqlite3_close(handle);
  return 0;
}

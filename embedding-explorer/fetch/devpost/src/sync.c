#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sync.h"

#ifndef EXPLORER_DB_PATH
#define EXPLORER_DB_PATH "../data/main.db"
#endif

/*
 * Join a project's members into one space separated string, since sqlite has
 * no list type. Returns NULL when the project has no members or on allocation
 * failure, both of which store a NULL members column.
 */
static char *join_members(const Members *members) {
  size_t size = 0;
  for (const Members *m = members; m != NULL; m = m->next) {
    size += strlen(m->name) + 1; // + separator, the last one covers the NUL
  }
  if (size == 0) {
    return NULL;
  }

  char *out = (char *)malloc(size);
  if (out == NULL) {
    return NULL;
  }

  char *p = out;
  for (const Members *m = members; m != NULL; m = m->next) {
    p += sprintf(p, p == out ? "%s" : " %s", m->name);
  }
  return out;
}

// Sync linked list of projects with the projects database
int dp_sync_projects(Projects *projects) {

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

  // Check if table exists and create it if it doesn't. The id is devpost's own
  // software id rather than an autoincrement: a project that is renamed gets a
  // new slug and keeps its id, so the id is what a row is the same row by.
  //
  // No url column. It is devpost.com/software/<slug> for every project there
  // is, so storing it would be storing the slug twice.
  const char *schema = "CREATE TABLE IF NOT EXISTS projects ("
                       "  id   INTEGER PRIMARY KEY,"
                       "  slug TEXT NOT NULL UNIQUE,"
                       "  name TEXT,"
                       "  tagline TEXT,"
                       "  likes INTEGER,"
                       "  members TEXT," // space separated, sqlite has no list
                       "  vector BLOB"
                       ");";

  char *err = NULL;
  if (sqlite3_exec(handle, schema, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "create failed: %s\n", err);
    sqlite3_free(err); // must free the error message
    sqlite3_close(handle);
    return 1;
  }

  // Update the row for this id, creating it if the id is new, and skip the
  // write entirely when nothing about the project has changed.
  //
  // Every column in the SET is also in the WHERE. A column left out of the SET
  // would keep a stale value forever, and one left out of the WHERE would skip
  // the update for a project that only changed that column, which is what
  // happens to a project that is only picking up likes.
  sqlite3_stmt *stmt;
  const char *upsert =
      "INSERT INTO projects (id, slug, name, tagline, likes, members) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
      "ON CONFLICT(id) DO UPDATE SET slug = excluded.slug, "
      "name = excluded.name, tagline = excluded.tagline, "
      "likes = excluded.likes, members = excluded.members "
      "WHERE slug IS NOT excluded.slug OR name IS NOT excluded.name "
      "OR tagline IS NOT excluded.tagline OR likes IS NOT excluded.likes "
      "OR members IS NOT excluded.members;";

  if (sqlite3_prepare_v2(handle, upsert, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(handle));
    sqlite3_close(handle);
    return 1;
  }

  // Iterate over the linked list, reusing the prepared statement per row.
  for (Projects *cur = projects; cur != NULL; cur = cur->next) {
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cur->id);
    sqlite3_bind_text(stmt, 2, cur->slug, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cur->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, cur->tagline, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)cur->likes);
    // TRANSIENT so sqlite copies the join and it can be freed right away
    char *members = join_members(cur->members);
    sqlite3_bind_text(stmt, 6, members, -1, SQLITE_TRANSIENT);
    free(members);

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

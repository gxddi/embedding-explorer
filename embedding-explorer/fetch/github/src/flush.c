#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "flush.h"

/*
 * Empty response and process and load(flush) resp into cur
 * Takes a pointer to a NULL repos head or tail
 */
int flush_response_repos(Response *resp, Repos **repos, long *total) {
  // Travel down resp until it's tail
  Repos **cur = repos;
  if (*cur != NULL) {
    while ((*cur)->next != NULL) {
      cur = &(*cur)->next;
    }
    cur = &(*cur)->next;
  }

  // Travel down the list of items(repos) in the response

  // Get raw (root object)
  cJSON *raw = cJSON_Parse(resp->data);
  if (raw == NULL) {
    fprintf(stderr, "flush_response_repos: response has nothing\n");
    cJSON_Delete(raw);
    free(resp->data);
    resp->data = NULL;
    resp->size = 0;
    return 1;
  }

  // Size of the whole result set, not of this page. Approximate for large
  // sets, so the caller treats it as a hint rather than a count.
  if (total != NULL) {
    cJSON *count = cJSON_GetObjectItem(raw, "total_count");
    *total = cJSON_IsNumber(count) ? (long)count->valuedouble : 0;
  }

  // Get items (array)
  cJSON *items = cJSON_GetObjectItem(raw, "items");
  if (!cJSON_IsArray(items)) {
    fprintf(stderr, "flush_response_repos: response has no \"items\" array\n");
    cJSON_Delete(raw);
    free(resp->data);
    resp->data = NULL;
    resp->size = 0;
    return 1;
  }

  // Iterate over items and get repo ...{name, star count, description, topics}
  for (int i = 0; i < (cJSON_GetArraySize(items)); i++) {
    cJSON *item = cJSON_GetArrayItem(items, i);
    *cur = (Repos *)malloc(sizeof(Repos));
    cJSON *name = cJSON_GetObjectItem(item, "full_name");
    cJSON *desc = cJSON_GetObjectItem(item, "description");
    cJSON *stars = cJSON_GetObjectItem(item, "stargazers_count");
    (*cur)->name = cJSON_IsString(name) ? strdup(name->valuestring) : NULL;
    (*cur)->desc = cJSON_IsString(desc) ? strdup(desc->valuestring)
                                        : NULL; // desc null for some repos
    (*cur)->stars = cJSON_IsNumber(stars) ? (stars->valueint) : 0;

    // Topics arrive as an array of strings, empty for repos that tag nothing
    Topics **topic = &(*cur)->topics;
    *topic = NULL;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(item, "topics")) {
      if (!cJSON_IsString(entry)) {
        continue;
      }
      *topic = (Topics *)malloc(sizeof(Topics));
      (*topic)->name = strdup(entry->valuestring);
      (*topic)->next = NULL;
      topic = &(*topic)->next;
    }

    (*cur)->next = NULL;
    cur = &(*cur)->next;
  }

  /* Clean up */
  // Empty response
  free(resp->data);
  resp->data = (char *)malloc(1);
  if (resp->data == NULL) {
    fprintf(stderr, "flush_response_repos: out of memory\n");
    return 1;
  }
  resp->data[0] = '\0';
  resp->size = 0;

  // Free cJSON
  cJSON_Delete(raw);

  return 0;
}

/*
 * Empty response and load(flush) resp's body into cont. With the .raw media
 * type the body is the file content itself, not JSON.
 */
int flush_response_content(Response *resp, char **cont) {
  *cont = strdup(resp->data);

  /* Clean up */
  // Empty response
  free(resp->data);
  resp->data = (char *)malloc(1);
  if (resp->data == NULL) {
    fprintf(stderr,
            "fetch/src/flush.c (flush_response_content): out of memory\n");
    return 1;
  }
  resp->data[0] = '\0';
  resp->size = 0;

  return *cont != NULL ? 0 : 1;
}

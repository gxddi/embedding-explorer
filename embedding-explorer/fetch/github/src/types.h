#ifndef TYPES_H
#define TYPES_H

#include <stdlib.h>

// HTTP response struct
typedef struct Response {
  char *data;
  size_t size;
} Response;

// Topics linked list, one list per repo
typedef struct Topics {
  char *name;
  struct Topics *next;
} Topics;

// Repositories linked list
typedef struct Repos {
  char *name;
  char *desc;
  size_t stars;
  Topics *topics;
  struct Repos *next;
} Repos;

// Free repos, along with each one's topics
void types_free_repos(Repos *head);

// Free response
void types_free_response(Response *resp);

// Empty resph response buffer
int types_reset_response(Response *resp);

#endif

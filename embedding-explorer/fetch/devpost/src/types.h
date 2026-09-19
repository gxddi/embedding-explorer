#ifndef DP_TYPES_H
#define DP_TYPES_H

#include <stdlib.h>

/*
 * Everything below carries a dp_ prefix. The github source has the same shapes
 * under the same names, and both static libraries end up in one binary, so the
 * two sets of symbols would collide at link time otherwise.
 */

// HTTP response struct
typedef struct Response {
  char *data;
  size_t size;
} Response;

// Hackathon subdomains, the galleries left to walk
typedef struct Hosts {
  char *name; // subdomain only, the rest of the url is assembled
  struct Hosts *next;
} Hosts;

// Members linked list, one list per project
typedef struct Members {
  char *name; // devpost username
  struct Members *next;
} Members;

// Projects linked list
typedef struct Projects {
  long id;      // devpost's own software id
  char *slug;   // devpost.com/software/<slug>, assembled where it is needed
  char *name;   // display title
  char *tagline;
  long likes;
  Members *members;
  struct Projects *next;
} Projects;

// Free hosts
void dp_types_free_hosts(Hosts *head);

// Free projects, along with each one's members
void dp_types_free_projects(Projects *head);

// Free response
void dp_types_free_response(Response *resp);

#endif

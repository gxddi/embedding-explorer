#include <stdlib.h>

#include "types.h"

void dp_types_free_hosts(Hosts *head) {
  while (head != NULL) {
    Hosts *next = head->next;
    free(head->name);
    free(head);
    head = next;
  }
}

void dp_types_free_projects(Projects *head) {
  while (head != NULL) {
    Projects *next = head->next;
    for (Members *m = head->members; m != NULL;) {
      Members *m_next = m->next;
      free(m->name);
      free(m);
      m = m_next;
    }
    free(head->slug);
    free(head->name);
    free(head->tagline);
    free(head);
    head = next;
  }
}

// Free response
void dp_types_free_response(Response *resp) {
  free(resp->data);
  resp->data = NULL;
  resp->size = 0;
}

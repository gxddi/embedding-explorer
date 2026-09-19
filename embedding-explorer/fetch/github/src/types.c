#include "types.h"
#include <stdio.h>
#include <stdlib.h>

void types_free_repos(Repos *head) {
  while (head != NULL) {
    Repos *next = head->next;
    for (Topics *t = head->topics; t != NULL;) {
      Topics *t_next = t->next;
      free(t->name);
      free(t);
      t = t_next;
    }
    free(head->name);
    free(head->desc);
    free(head);
    head = next;
  }
}

// Free response
void types_free_response(Response *resp) {
  free(resp->data);
  resp->size = 0;
}

// Empty resph response buffer
int types_reset_response(Response *resp) {
  free(resp->data);
  resp->data = (char *)malloc(1);
  if (resp->data == NULL) {
    fprintf(stderr,
            "fetch/src/types.c (types_reset_response): Out of memory\n");
    return 1;
  }
  resp->size = 0;
  resp->data[0] = '\0';
  return 0;
}

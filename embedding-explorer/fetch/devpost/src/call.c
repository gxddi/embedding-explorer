#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <curl/curl.h>

#include "call.h"

/*
 * Internal write function to write HTTP response to Response struct
 */
static size_t dp_write_to_mem(void *contents, size_t size_n, size_t n,
                              Response *resp) {
  if (size_n != 0 && n > SIZE_MAX / size_n) return 0;
  size_t chunk_size = (size_n * n);
  if (resp->size == SIZE_MAX || chunk_size > SIZE_MAX - resp->size - 1) return 0;

  char *grown = (char *)realloc(resp->data, resp->size + chunk_size + 1);
  if (grown == NULL) {
    return 0; // short write, curl aborts the transfer
  }
  resp->data = grown;

  memcpy(resp->data + resp->size, contents, chunk_size);
  resp->size += chunk_size;
  resp->data[resp->size] = '\0';

  return chunk_size;
}

/*
 * Fetch content from a devpost url and load the body into resp
 */
int dp_call_url(const char *url, Response *resp, long *status) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    *status = 0;
    *resp = (Response){NULL, 0};
    fprintf(stderr, "devpost fetch: could not initialize curl\n");
    return 1;
  }
  *status = 0;                              // no exchange yet
  *resp = (Response){(char *)malloc(1), 0}; // response buffer

  if (resp->data == NULL) {
    fprintf(stderr, "fetch/devpost/src/call.c (dp_call_url): out of memory\n");
    curl_easy_cleanup(curl);
    *resp = (Response){NULL, 0}; // caller's free is a no-op
    return 1;
  }
  resp->data[0] = '\0';

  char error[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Endpoint
  curl_easy_setopt(curl, CURLOPT_URL, url);

  // Hackathons live on per event subdomains, and the listing hands back the
  // canonical url, so a description fetch can be redirected once or twice.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  // No token: the listing is public and takes no auth. The User-Agent is not
  // optional though, devpost's edge answers 403 to a request without one.
  struct curl_slist *headers = curl_slist_append(NULL, "User-Agent: Hackright");
  headers = curl_slist_append(headers, "Accept: application/json, text/html");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Write function
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dp_write_to_mem);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

  // Perform request
  CURLcode result = curl_easy_perform(curl);

  // HTTP status, read while the handle is still alive
  if (result == CURLE_OK) {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
  }

  // Clean up. Only the easy handle and this request's header list: the globals
  // belong to the process, and tearing them down here would pull them out from
  // under any other thread mid-request.
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result) {
    fprintf(stderr,
            "fetch/devpost/src/call.c (dp_call_url): request failed with curl "
            "error %d\n",
            result);
    free(resp->data);
    *resp = (Response){NULL, 0}; // caller's free is a no-op
    fprintf(stderr, "%s: %s\n", url, error[0] ? error : curl_easy_strerror(result));
    return result;
  } else {
    return result;
  }
}

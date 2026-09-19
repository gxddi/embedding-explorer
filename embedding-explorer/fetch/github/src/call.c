#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "call.h"

/*
 * Internal write function to write HTTP response to Response struct
 */
size_t write_to_mem(void *contents, size_t size_n, size_t n, Response *resp) {
  // printf("Writing %zu characters of %zu bytes, into memory...\n", n, size_n);

  if (size_n != 0 && n > SIZE_MAX / size_n) return 0;
  size_t chunk_size = (size_n * n);
  if (resp->size == SIZE_MAX || chunk_size > SIZE_MAX - resp->size - 1) return 0;

  char *grown = (char *)realloc(resp->data, resp->size + chunk_size + 1);
  if (grown == NULL) return 0;
  resp->data = grown;
  // printf("    Data: %s\n", resp->data);
  memcpy(resp->data + resp->size, contents, chunk_size);

  resp->size += chunk_size;

  resp->data[resp->size] = '\0';

  return chunk_size;
}

/*
 * Fetch content from gh endpoint via their REST api and load the response
 * header into resph and response into resp
 */
int call_gh_url(const char *url, const char *media, Response *resph,
                Response *resp, long *status) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    *status = 0;
    *resp = (Response){NULL, 0};
    *resph = (Response){NULL, 0};
    fprintf(stderr, "github fetch: could not initialize curl\n");
    return 1;
  }
  *status = 0;                               // no exchange yet
  *resph = (Response){(char *)malloc(1), 0}; // header buffer
  *resp = (Response){(char *)malloc(1), 0};  // response buffer

  if (resph->data == NULL || resp->data == NULL) {
    fprintf(stderr, "fetch/call.c (call_gh_url): out of memory\n");
    curl_easy_cleanup(curl);
    free(resph->data);
    free(resp->data);
    *resph = (Response){NULL, 0}; // caller's types_free_response is a no-op
    *resp = (Response){NULL, 0};
    return 1;
  }
  resph->data[0] = '\0';
  resp->data[0] = '\0';

  char error[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Endpoint
  curl_easy_setopt(curl, CURLOPT_URL, url);

  // Media type (useful for getting raw html from readme)
  char media_type[100];
  snprintf(media_type, sizeof(media_type),
           "Accept: application/vnd.github%s+json", media);

  // Token
  const char *token = getenv("GITHUB_TOKEN");
  char auth[128];
  if (token && *token)
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);

  // Set headers
  struct curl_slist *headers = curl_slist_append(NULL, media_type);
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2026-03-10");
  headers = curl_slist_append(headers, "User-Agent: Hackright");
  if (token && *token) headers = curl_slist_append(headers, auth);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Write function
  /* (Writing to files)
   * FILE *repos = fopen("repos.txt", "w");
   * FILE *pages = fopen("pages.txt", "w");
   */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_mem);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);

  if (resph != NULL) {
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_to_mem);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resph);
  }

  // Perform request
  CURLcode result;
  for (int attempt = 0; ; attempt++) {
    result = curl_easy_perform(curl);
    if (result != CURLE_OK) break;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);

    long remaining = -1, reset = -1, retry_after = -1;
    for (const char *line = resph->data; line && *line; ) {
      if (strncasecmp(line, "x-ratelimit-remaining:", 22) == 0)
        remaining = strtol(line + 22, NULL, 10);
      if (strncasecmp(line, "x-ratelimit-reset:", 18) == 0)
        reset = strtol(line + 18, NULL, 10);
      if (strncasecmp(line, "retry-after:", 12) == 0)
        retry_after = strtol(line + 12, NULL, 10);
      line = strchr(line, '\n');
      if (line) line++;
    }
    int limited = *status == 429 || (*status == 403 &&
        (remaining == 0 || retry_after >= 0 ||
         strstr(resp->data, "rate limit") != NULL));
    if (!limited) break;
    if (attempt == 3) {
      fprintf(stderr, "GitHub rate limit persists after 3 retries: %s\n", url);
      break;
    }
    long delay = retry_after >= 0 ? retry_after : (60L << attempt);
    if (remaining == 0 && reset > 0) {
      long until_reset = reset - (long)time(NULL) + 1;
      if (retry_after < 0 || until_reset > delay) delay = until_reset;
    }
    if (delay < 1) delay = 1;
    if (delay > UINT_MAX) {
      fprintf(stderr, "GitHub rate limit wait is too large: %ld seconds\n", delay);
      break;
    }
    fprintf(stderr, "GitHub rate limited: waiting %ld seconds, retry %d/3: %s\n",
            delay, attempt + 1, url);
    unsigned int left = (unsigned int)delay;
    while (left) left = sleep(left);
    resph->size = resp->size = 0;
    resph->data[0] = resp->data[0] = '\0';
    *status = 0;
  }

  // Clean up. Only the easy handle: the globals belong to the process, and
  // tearing them down here would pull them out from under any other thread
  // mid-request.
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result) {
    fprintf(stderr,
            "fetch/call.c (call_gh_url): request failed with curl error %d\n",
            result);
    free(resph->data);
    free(resp->data);
    *resph = (Response){NULL, 0}; // caller's types_free_response is a no-op
    *resp = (Response){NULL, 0};
    fprintf(stderr, "%s: %s\n", url, error[0] ? error : curl_easy_strerror(result));
    return result;
  } else {
    return result;
  }
}

/*
 *
 *   zip_streamer.c - A FCGI reverse proxy to stream content from containers
 *   Copyright (C) 2015  Andy Irving
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <fcgiapp.h>
#include <magic.h>
#include <signal.h>
#include <log4c.h>
#include <pthread.h>

#define THREAD_COUNT 20

log4c_category_t *logcat;

struct response {
  char *data;
  size_t size;
  size_t read;
  CURLM *multi_handle;
  CURL *http_handle;
  const char *url;
};

typedef struct response response_t;

void *fcgi_worker(void *a);
int archive_open(struct archive *a, void *client_data);
int archive_close(struct archive *a, void *client_data);
ssize_t archive_read(struct archive *a, void *client_data, const void **block);

int read_archive(response_t *response, const char *filename, magic_t *magic,
                 FCGX_Request *request);
size_t curl_write_response(void *ptr, size_t size, size_t nmemb, void *stream);
void failure(int, FCGX_Request *request);

volatile sig_atomic_t terminate = 0;
pthread_t threads[THREAD_COUNT];

void sig_handler(__attribute__((unused)) int signum) {
  terminate = 1;
  for (unsigned int i = 0; i < THREAD_COUNT; i++) {
    pthread_kill(threads[i], SIGUSR1);
  }
}
void thread_sig_handler(__attribute__((unused)) int signum) {
  FCGX_ShutdownPending();
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Terminating thread");
}

int main(void) {
  log4c_init();
  logcat = log4c_category_get("zip_streamer");

  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = sig_handler;
  action.sa_flags = 0;
  sigaction(SIGTERM, &action, NULL);

  curl_global_init(CURL_GLOBAL_DEFAULT);

  FCGX_Init();

  for (unsigned i = 0; i < THREAD_COUNT; i++) {
    pthread_create(&threads[i], NULL, fcgi_worker, &i);
  }

  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Threads created");

  for (unsigned int i = 0; i < THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }

  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Threads finished");

  curl_global_cleanup();

  return 0;
}

void *fcgi_worker(void *a) {
  int id = *(int *)a;
  struct sigaction action;
  memset(&action, 0, sizeof(struct sigaction));
  action.sa_handler = thread_sig_handler;
  action.sa_flags = 0;
  sigaction(SIGUSR1, &action, NULL);
  magic_t magic = magic_open(MAGIC_MIME_TYPE);
  if (NULL == magic) {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "Thread %d: Unable to initialise Mime magic database",
                       id);
    return 0;
  }
  if (0 != magic_load(magic, NULL)) {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "Thread %d: Unable to load magic database: %s", id,
                       magic_error(magic));
    magic_close(magic);
    return 0;
  }
  CURLM *multi_handle = curl_multi_init();
  CURL *http_handle = curl_easy_init();
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG,
                     "Thread %d: curl handle created", id);

  FCGX_Request request;

  if (0 > FCGX_InitRequest(&request, 0, FCGI_FAIL_ACCEPT_ON_INTR)) {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "Thread %d: Unable to initialise request.", id);
    curl_easy_cleanup(http_handle);
    curl_multi_cleanup(multi_handle);
    magic_close(magic);
    return 0;
  }

  while (!terminate) {
    static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&accept_mutex);
    int rc = FCGX_Accept_r(&request);
    pthread_mutex_unlock(&accept_mutex);
    if (0 > rc || terminate) {
      break;
    }

    log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG,
                       "Thread %d: Accepted request", id);

    const char *request_uri = FCGX_GetParam("REQUEST_URI", request.envp);
    const char *host_uri = FCGX_GetParam("HOST_URI", request.envp);

    if (NULL == request_uri || (request_uri && 1 == strlen(request_uri))) {
      log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                         "Thread %d: Empty request URI: '%s'", id, request_uri);
      failure(404, &request);
      FCGX_Finish_r(&request);
      continue;
    }
    char *zip = malloc(strlen(request_uri) * sizeof(*request_uri));
    char *entry = malloc(strlen(request_uri) * sizeof(*request_uri));
    if (2 != sscanf(request_uri, "/%[^/]/%s", zip, entry)) {
      failure(500, &request);
      goto clean;
    }
    size_t ziplen = strlen(zip) * sizeof(*zip) + 1;
    size_t entlen = strlen(entry) * sizeof(*entry) + 1;
    zip = realloc(zip, ziplen);
    entry = realloc(entry, entlen);
    size_t len1 = strlen(host_uri) * sizeof(char);
    size_t len2 = strlen(zip) * sizeof(char);
    char *url = malloc(len1 + len2 + 1);
    memcpy(url, host_uri, len1);
    memcpy(url + len1, zip, len2 + 1);
    log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG,
                       "Thread %d: Target URI: %s", id, url);
    response_t response = {0, 0, 0, multi_handle, http_handle, url};
    int found = read_archive(&response, entry, &magic, &request);
    log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Found in archive: %d",
                       found);
    if (0 == found) {
      failure(404, &request);
    }
    free(url);
    free(response.data);

  clean:
    free(zip);
    free(entry);
    FCGX_Finish_r(&request);
  }
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "magic_close");
  magic_close(magic);
  curl_easy_cleanup(http_handle);
  curl_multi_cleanup(multi_handle);
  return 0;
}

void failure(int status, FCGX_Request *request) {

  FCGX_FPrintF(request->out, "Status: %d\r\n", status);
  FCGX_FPrintF(request->out, "Content-type: text/html\r\n\r\n");
}

int archive_open(struct archive __attribute__((unused)) * a,
                 void __attribute__((unused)) * client_data) {
  response_t *r = client_data;
  curl_easy_reset(r->http_handle);
  curl_easy_setopt(r->http_handle, CURLOPT_ENCODING, "identity");
  curl_easy_setopt(r->http_handle, CURLOPT_WRITEDATA, r);
  curl_easy_setopt(r->http_handle, CURLOPT_WRITEFUNCTION, curl_write_response);
  curl_easy_setopt(r->http_handle, CURLOPT_URL, r->url);
  curl_easy_setopt(r->http_handle, CURLOPT_FAILONERROR, 1l);
  curl_multi_add_handle(r->multi_handle, r->http_handle);
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Downloading: %s", r->url);
  int running;
  CURLMcode rc;
  while (CURLM_CALL_MULTI_PERFORM ==
         (rc = curl_multi_perform(r->multi_handle, &running)))
    ;
  if (!running || CURLM_OK != rc) {
    return ARCHIVE_FATAL;
  }
  return ARCHIVE_OK;
}

int archive_close(__attribute__((unused)) struct archive *a,
                  void *client_data) {
  response_t *r = client_data;
  curl_multi_remove_handle(r->multi_handle, r->http_handle);
  return 0;
}

size_t curl_write_response(void *ptr, size_t size, size_t nmemb, void *stream) {
  void *newdata;
  size_t realsize = size * nmemb;
  response_t *mem = stream;
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Curl write response: %d",
                     realsize);
  newdata = realloc(mem->data, mem->size + realsize + 1);
  if (newdata) {
    mem->data = newdata;
    memcpy(&(mem->data[mem->size]), ptr, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
  } else {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "failed to reallocate %zd bytes\n",
                       mem->size + realsize + 1);
    return 0;
  }

  return realsize;
}

int read_archive(response_t *response, const char *filename, magic_t *magic,
                 FCGX_Request *request) {
  int r;
  int not_found = 0;
  struct archive *a = archive_read_new();
  struct archive_entry *entry;
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);
  r = archive_read_open(a, response, archive_open, archive_read, archive_close);
  if (r != ARCHIVE_OK) {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "Error opening archive: %d", r);
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL,
                       "Error opening archive: %s", archive_error_string(a));
    archive_read_free(a);
    return r;
  }
  while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
    const char *entryname = archive_entry_pathname(entry);
    log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "\t Entry name: %s",
                       entryname);
    if (0 == strcmp(filename, entryname)) {
      not_found = 1;
      size_t entry_size = archive_entry_size(entry);
      log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG,
                         "\tExtracting file: %s, size: %d", entryname,
                         entry_size);
      if (0 < entry_size) {
        void *file_contents = malloc(entry_size);
        int dr = archive_read_data(a, file_contents, entry_size);
        log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG,
                           "\tBytes Extracted: %d", dr);
        log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "\tBytes Expected: %d",
                           entry_size);
        if (ARCHIVE_OK < dr) {
          const char *mime = magic_buffer(*magic, file_contents, entry_size);
          log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "\tMime-type: %s",
                             mime);
          FCGX_FPrintF(request->out, "Content-Type: %s\r\n", mime);
          FCGX_FPrintF(request->out, "Content-Length: %d\r\n\r\n", dr);
          FCGX_PutStr(file_contents, dr, request->out);
        }
        free(file_contents);
      }
      break;
    }
  }
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Archive Read: %d", r);
  archive_read_free(a);
  return not_found;
}

ssize_t archive_read(__attribute__((unused)) struct archive *a,
                     void *client_data, const void **block) {
  response_t *mem = client_data;

  int numfds;
  CURLMcode mc = curl_multi_wait(mem->multi_handle, NULL, 0, 10000, &numfds);
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "AR: fds: %d", numfds);
  if (CURLM_OK != mc) {
    log4c_category_log(logcat, LOG4C_PRIORITY_FATAL, "%s\n",
                       curl_easy_strerror(mc));
  }
  int running;
  CURLMcode rc;
  while (CURLM_CALL_MULTI_PERFORM ==
         (rc = curl_multi_perform(mem->multi_handle, &running)))
    ;
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "AR: Curl: %d", rc);
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "AR: Running: %d", running);
  size_t diff = mem->size - mem->read;
  while (running && 0 == diff) {
    log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Downloading");
    while (CURLM_CALL_MULTI_PERFORM ==
           (rc = curl_multi_perform(mem->multi_handle, &running)))
      ;
    diff = mem->size - mem->read;
  }

  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "Curl: %s\n",
                     curl_easy_strerror(mc));

  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "mem values %d, %d, %d",
                     mem->size, mem->read, diff);

  *block = &mem->data[mem->read];
  mem->read += diff;
  log4c_category_log(logcat, LOG4C_PRIORITY_DEBUG, "mem values %d, %d, %d",
                     mem->size, mem->read, diff);
  return diff;
}

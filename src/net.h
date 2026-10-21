#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct http http_t;

typedef int (*http_sink)(const uint8_t *data, size_t len, void *user);

typedef struct {
    const char *url;
    const char *post;
    size_t post_len;
    const char *const *headers;
    int64_t range_start;
    int64_t range_end;
    int timeout_s;
    int compress;
    int fail_http_error;
    volatile int *cancel;
    http_sink sink;
    void *user;
} http_req_t;

#define HTTP_OK 0
#define HTTP_STOPPED 1
#define HTTP_CANCELLED (-1)
#define HTTP_RANGE_IGNORED (-2)
#define HTTP_FAILED (-3)

int net_init(int insecure);
int net_wifi_connected(void);

http_t *http_new(void);
void http_free(http_t *h);
int http_run(http_t *h, const http_req_t *req, long *status);
int http_fetch(http_t *h, const http_req_t *req, buf_t *out, size_t max_len, long *status);
const char *http_last_error(http_t *h);

#endif

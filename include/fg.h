#ifndef FG_H
#define FG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fg_status {
    FG_OK = 0,
    FG_INVALID,
    FG_NOMEM,
    FG_PARSE,
    FG_IO,
    FG_HTTP,
    FG_LIMIT,
    FG_NOT_FOUND,
    FG_CHECKSUM
} fg_status;

typedef struct fg_error {
    fg_status code;
    int http_status;
    char message[256];
} fg_error;

/* Borrowed UTF-8 bytes; embedded NUL is allowed in data, not URLs or headers. */
typedef struct fg_str { const char *data; size_t len; } fg_str;
#define FG_STR(s) fg_string(s)
fg_str fg_string(const char *text);
int fg_str_equal(fg_str a, fg_str b);

/* Initialize to {0}. Owned buffers are NUL terminated; len excludes that byte. */
typedef struct fg_buffer { char *data; size_t len; size_t capacity; } fg_buffer;
fg_status fg_buffer_append(fg_buffer *out, const void *data, size_t len, fg_error *error);
fg_status fg_buffer_set(fg_buffer *out, fg_str text, fg_error *error);
void fg_buffer_free(fg_buffer *buffer);
fg_status fg_fail(fg_error *error, fg_status code, const char *message);
const char *fg_status_name(fg_status status);
fg_status fg_url_encode(fg_buffer *out, fg_str value, fg_error *error);

typedef struct fg_header { fg_str name; fg_str value; } fg_header;
typedef struct fg_request {
    fg_str url;
    const fg_header *headers;
    size_t header_count;
    fg_str body;
    size_t max_bytes;
    unsigned timeout_ms;
} fg_request;
typedef struct fg_response {
    int status;
    fg_buffer headers;
    fg_buffer body;
} fg_response;
void fg_response_free(fg_response *response);

/* Callbacks return transport status, not HTTP success. Response buffers use
   fg_buffer_append/set; the caller frees them, including on callback failure.
   The transport and its user context must outlive every borrowing client. */
typedef struct fg_transport {
    void *user;
    fg_status (*get)(void *user, const fg_request *request, fg_response *response, fg_error *error);
    fg_status (*post)(void *user, const fg_request *request, fg_response *response, fg_error *error);
} fg_transport;
fg_status fg_transport_default(fg_transport *out, fg_error *error);
void fg_transport_default_free(fg_transport *transport);

typedef struct fg_client fg_client;
typedef struct fg_client_options {
    fg_str user_agent;
    size_t max_response_bytes; /* Zero: 256 MiB. */
    unsigned timeout_ms;      /* Zero: 30 seconds. */
    /* Optional paired monotonic clock/sleep hooks for deterministic tests. */
    void *clock_user;
    uint64_t (*now_ms)(void *user);
    void (*sleep_ms)(void *user, unsigned milliseconds);
} fg_client_options;
fg_status fg_client_create(const fg_transport *transport, const fg_client_options *options,
                           fg_client **out, fg_error *error);
/* A NULL transport creates a client-owned default; otherwise it is borrowed.
   Initialize responses to {0}; non-2xx responses retain their headers/body. */
void fg_client_free(fg_client *client);
/* Client handles and default transports are single-threaded. No stdout logging. */
fg_status fg_get(fg_client *client, fg_str url, const fg_header *headers, size_t count,
                 fg_response *out, fg_error *error);
fg_status fg_post(fg_client *client, fg_str url, const fg_header *headers, size_t count,
                  fg_str body, fg_response *out, fg_error *error);
/* SEC uses the configured User-Agent and at least 120 ms between request starts. */
fg_status fg_sec_get(fg_client *client, fg_str url, fg_response *out, fg_error *error);
fg_status fg_read_file(fg_str path, size_t max_bytes, fg_buffer *out, fg_error *error);

#ifdef __cplusplus
}
#endif
#endif

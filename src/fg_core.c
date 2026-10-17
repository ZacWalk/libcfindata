#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "fg.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define FG_DEFAULT_LIMIT ((size_t)256 * 1024 * 1024)
#define FG_DEFAULT_TIMEOUT 30000u

struct fg_client {
    const fg_transport *transport;
    fg_transport owned;
    fg_buffer user_agent;
    size_t max_bytes;
    unsigned timeout_ms;
    void *clock_user;
    uint64_t (*now_ms)(void *);
    void (*sleep_ms)(void *, unsigned);
    uint64_t last_start;
    int started;
    int owns_transport;
};

fg_str fg_string(const char *text)
{
    fg_str result = {text, text ? strlen(text) : 0};
    return result;
}

int fg_str_equal(fg_str a, fg_str b)
{
    return a.len == b.len && (!a.len ||
           (a.data && b.data && memcmp(a.data, b.data, a.len) == 0));
}

fg_status fg_fail(fg_error *error, fg_status code, const char *message)
{
    if (error) {
        size_t len = message ? strlen(message) : 0;
        if (len >= sizeof(error->message)) len = sizeof(error->message) - 1;
        if (len) memmove(error->message, message, len);
        error->message[len] = '\0';
        error->code = code;
        error->http_status = 0;
    }
    return code;
}

const char *fg_status_name(fg_status status)
{
    switch (status) {
    case FG_OK: return "FG_OK";
    case FG_INVALID: return "FG_INVALID";
    case FG_NOMEM: return "FG_NOMEM";
    case FG_PARSE: return "FG_PARSE";
    case FG_IO: return "FG_IO";
    case FG_HTTP: return "FG_HTTP";
    case FG_LIMIT: return "FG_LIMIT";
    case FG_NOT_FOUND: return "FG_NOT_FOUND";
    case FG_CHECKSUM: return "FG_CHECKSUM";
    default: return "FG_UNKNOWN";
    }
}

static int fg_buffer_valid(const fg_buffer *buffer)
{
    return buffer && ((!buffer->data && !buffer->len && !buffer->capacity) ||
           (buffer->data && buffer->capacity && buffer->len < buffer->capacity));
}

static fg_status fg_buffer_write(fg_buffer *out, const void *data, size_t len,
                                  int append, fg_error *error)
{
    size_t base, needed, capacity, offset = 0;
    int aliases = 0;
    char *memory;
    if (!fg_buffer_valid(out) || (!data && len))
        return fg_fail(error, FG_INVALID, "Invalid buffer or input");
    base = append ? out->len : 0;
    if (len > SIZE_MAX - base - 1)
        return fg_fail(error, FG_LIMIT, "Buffer size overflow");
    needed = base + len + 1;
    /* Integer addresses avoid undefined relational comparisons of unrelated
       pointers. Retain an offset across realloc for overlapping source views. */
    if (len && out->data && (uintptr_t)data >= (uintptr_t)out->data &&
        (uintptr_t)data - (uintptr_t)out->data < out->capacity) {
        offset = (size_t)((uintptr_t)data - (uintptr_t)out->data);
        if (len > out->capacity - offset)
            return fg_fail(error, FG_INVALID, "Buffer source exceeds allocation");
        aliases = 1;
    }
    if (needed > out->capacity) {
        capacity = out->capacity ? out->capacity : 64;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        memory = (char *)realloc(out->data, capacity);
        if (!memory) return fg_fail(error, FG_NOMEM, "Cannot allocate buffer");
        out->data = memory;
        out->capacity = capacity;
    }
    if (len) memmove(out->data + base, aliases ? out->data + offset : data, len);
    out->len = base + len;
    out->data[out->len] = '\0';
    return FG_OK;
}

fg_status fg_buffer_append(fg_buffer *out, const void *data, size_t len, fg_error *error)
{
    return fg_buffer_write(out, data, len, 1, error);
}

fg_status fg_buffer_set(fg_buffer *out, fg_str text, fg_error *error)
{
    return fg_buffer_write(out, text.data, text.len, 0, error);
}

void fg_buffer_free(fg_buffer *buffer)
{
    if (buffer) {
        free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
    }
}

fg_status fg_url_encode(fg_buffer *out, fg_str value, fg_error *error)
{
    static const char hex[] = "0123456789ABCDEF";
    fg_buffer encoded = {0};
    size_t i, len = 0;
    if (!fg_buffer_valid(out) || (!value.data && value.len))
        return fg_fail(error, FG_INVALID, "Invalid URL encoding input");
    if (value.len > (SIZE_MAX - 1) / 3)
        return fg_fail(error, FG_LIMIT, "Encoded URL size overflow");
    encoded.capacity = value.len * 3 + 1;
    encoded.data = (char *)malloc(encoded.capacity);
    if (!encoded.data) return fg_fail(error, FG_NOMEM, "Cannot allocate encoded URL");
    for (i = 0; i < value.len; ++i) {
        unsigned char c = (unsigned char)value.data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            encoded.data[len++] = (char)c;
        else {
            encoded.data[len++] = '%';
            encoded.data[len++] = hex[c >> 4];
            encoded.data[len++] = hex[c & 15];
        }
    }
    encoded.data[len] = '\0';
    encoded.len = len;
    fg_buffer_free(out);
    *out = encoded;
    return FG_OK;
}

void fg_response_free(fg_response *response)
{
    if (response) {
        fg_buffer_free(&response->headers);
        fg_buffer_free(&response->body);
        response->status = 0;
    }
}

/* These internal entry points are shared with the native transports, so direct
   calls through default.get/post enforce the same policy as client calls. */
fg_status fg_internal_os_error(fg_error *error, fg_status status,
                               const char *operation, unsigned long number)
{
    char message[256];
    (void)snprintf(message, sizeof(message), "%s failed (OS error %lu)", operation, number);
    return fg_fail(error, status, message);
}

static int fg_ascii_equal(fg_str text, const char *literal)
{
    size_t i, len = strlen(literal);
    if (!text.data || text.len != len) return 0;
    for (i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text.data[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)literal[i]) return 0;
    }
    return 1;
}

static int fg_utf8_valid(fg_str text, int controls)
{
    size_t i = 0;
    if (!text.data && text.len) return 0;
    while (i < text.len) {
        unsigned char c = (unsigned char)text.data[i++];
        uint32_t cp, minimum;
        size_t n, j;
        if (c < 0x80) { cp = c; n = 0; minimum = 0; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; n = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; n = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; n = 3; minimum = 0x10000; }
        else return 0;
        if (n > text.len - i) return 0;
        for (j = 0; j < n; ++j) {
            c = (unsigned char)text.data[i++];
            if ((c & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (c & 63u);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            !cp || (controls && (cp < 32 || (cp >= 127 && cp <= 159)))) return 0;
    }
    return 1;
}

static int fg_header_token(fg_str name)
{
    size_t i;
    if (!name.data || !name.len) return 0;
    for (i = 0; i < name.len; ++i) {
        unsigned char c = (unsigned char)name.data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) continue;
        if (!c || !strchr("!#$%&'*+-.^_`|~", c)) return 0;
    }
    return 1;
}

static fg_status fg_validate_url(fg_str url, int sec, fg_error *error)
{
    size_t prefix, end, host_end, i;
    fg_str host;
    unsigned port = 0;
    int secure;
    if (!url.len || !fg_utf8_valid(url, 1))
        return fg_fail(error, FG_INVALID, "URL must be nonempty valid UTF-8 without controls");
    if (url.len >= 8 && fg_ascii_equal((fg_str){url.data, 8}, "https://"))
        prefix = 8, secure = 1;
    else if (url.len >= 7 && fg_ascii_equal((fg_str){url.data, 7}, "http://"))
        prefix = 7, secure = 0;
    else return fg_fail(error, FG_INVALID, "Only HTTPS URLs are permitted");
    for (i = prefix; i < url.len; ++i) {
        if (url.data[i] == '\\' || url.data[i] == ' ' || url.data[i] == '#')
            return fg_fail(error, FG_INVALID, "URL contains forbidden characters");
        if (url.data[i] == '%') {
            size_t j;
            if (url.len - i < 3)
                return fg_fail(error, FG_INVALID, "Invalid URL percent escape");
            for (j = 1; j <= 2; ++j) {
                char c = url.data[i + j];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F')))
                    return fg_fail(error, FG_INVALID, "Invalid URL percent escape");
            }
            i += 2;
        }
    }
    end = prefix;
    while (end < url.len && url.data[end] != '/' && url.data[end] != '?') ++end;
    host_end = prefix;
    if (prefix < end && url.data[prefix] == '[') {
        ++host_end;
        while (host_end < end && url.data[host_end] != ']') {
            char c = url.data[host_end++];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F') || c == ':' || c == '.'))
                return fg_fail(error, FG_INVALID, "Invalid IPv6 URL host");
        }
        if (host_end == end)
            return fg_fail(error, FG_INVALID, "Unclosed IPv6 URL host");
        ++host_end;
    } else {
        while (host_end < end && url.data[host_end] != ':') {
            char c = url.data[host_end++];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '-' || c == '.'))
                return fg_fail(error, FG_INVALID, "Invalid URL host");
        }
    }
    host = (fg_str){url.data + prefix, host_end - prefix};
    if (!host.len || host.data[0] == '.' || host.data[host.len - 1] == '.')
        return fg_fail(error, FG_INVALID, "Invalid URL host");
    if (host.data[0] != '[') {
        size_t label = 0;
        if (host.len > 253)
            return fg_fail(error, FG_INVALID, "URL host is too long");
        for (i = 0; i <= host.len; ++i) {
            if (i == host.len || host.data[i] == '.') {
                if (!label || label > 63 || host.data[i - label] == '-' || host.data[i - 1] == '-')
                    return fg_fail(error, FG_INVALID, "Invalid URL host label");
                label = 0;
            } else ++label;
        }
    }
    if (host_end < end) {
        if (url.data[host_end++] != ':' || host_end == end)
            return fg_fail(error, FG_INVALID, "Invalid URL port");
        for (i = host_end; i < end; ++i) {
            if (url.data[i] < '0' || url.data[i] > '9')
                return fg_fail(error, FG_INVALID, "Invalid URL port");
            port = port * 10 + (unsigned)(url.data[i] - '0');
            if (port > 65535) return fg_fail(error, FG_INVALID, "URL port is out of range");
        }
        if (!port) return fg_fail(error, FG_INVALID, "URL port is out of range");
    }
    if (sec) {
        int sec_host = fg_ascii_equal(host, "sec.gov");
        if (host.len > 8)
            sec_host = fg_ascii_equal((fg_str){host.data + host.len - 8, 8}, ".sec.gov");
        if (!secure || !sec_host || (port && port != 443))
            return fg_fail(error, FG_INVALID, "SEC requests require an HTTPS sec.gov origin on port 443");
    } else if (!secure && !fg_ascii_equal(host, "localhost") &&
               !fg_ascii_equal(host, "127.0.0.1") && !fg_ascii_equal(host, "[::1]"))
        return fg_fail(error, FG_INVALID, "Plain HTTP is restricted to loopback test servers");
    return FG_OK;
}

fg_status fg_internal_validate_request(const fg_request *request, fg_response *out,
                                       fg_error *error)
{
    size_t i;
    fg_status status;
    if (!request || !out || !fg_buffer_valid(&out->body) ||
        !fg_buffer_valid(&out->headers) || (!request->body.data && request->body.len) ||
        (!request->headers && request->header_count))
        return fg_fail(error, FG_INVALID, "Invalid request or response");
    if (request->header_count > SIZE_MAX / sizeof(fg_header))
        return fg_fail(error, FG_LIMIT, "Request header count overflow");
    status = fg_validate_url(request->url, 0, error);
    if (status != FG_OK) return status;
    for (i = 0; i < request->header_count; ++i) {
        const fg_header *h = request->headers + i;
        if (!fg_header_token(h->name) || !fg_utf8_valid(h->value, 1))
            return fg_fail(error, FG_INVALID, "Invalid HTTP header");
        /* Framing, routing, proxy authentication and connection management belong
           to the backend, not caller-supplied headers. */
        if (fg_ascii_equal(h->name, "host") || fg_ascii_equal(h->name, "content-length") ||
            fg_ascii_equal(h->name, "transfer-encoding") || fg_ascii_equal(h->name, "connection") ||
            fg_ascii_equal(h->name, "proxy-authorization") || fg_ascii_equal(h->name, "proxy-connection"))
            return fg_fail(error, FG_INVALID, "HTTP routing or framing header is not caller configurable");
    }
    return FG_OK;
}

static uint64_t fg_now_ms(void *user)
{
    (void)user;
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0 || counter.QuadPart < 0) return UINT64_MAX;
    return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000 +
           (uint64_t)((counter.QuadPart % frequency.QuadPart) * 1000 / frequency.QuadPart);
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return UINT64_MAX;
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
#endif
}

static void fg_sleep_ms(void *user, unsigned milliseconds)
{
    (void)user;
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec duration, left;
    duration.tv_sec = (time_t)(milliseconds / 1000);
    duration.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &left) != 0 && errno == EINTR) duration = left;
#endif
}

fg_status fg_client_create(const fg_transport *transport, const fg_client_options *options,
                           fg_client **out, fg_error *error)
{
    fg_client *client;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing client output");
    *out = NULL;
    if (transport && !transport->get && !transport->post)
        return fg_fail(error, FG_INVALID, "Transport has no callbacks");
    if (options && (!!options->now_ms != !!options->sleep_ms))
        return fg_fail(error, FG_INVALID, "Clock and sleep hooks must be supplied together");
    if (options && !fg_utf8_valid(options->user_agent, 1))
        return fg_fail(error, FG_INVALID, "Invalid User-Agent");
    if (options && options->user_agent.len) {
        size_t i;
        for (i = 0; i < options->user_agent.len; ++i)
            if (options->user_agent.data[i] != ' ') break;
        if (i == options->user_agent.len)
            return fg_fail(error, FG_INVALID, "User-Agent cannot consist only of whitespace");
    }
    client = (fg_client *)calloc(1, sizeof(*client));
    if (!client) return fg_fail(error, FG_NOMEM, "Cannot allocate client");
    client->max_bytes = options && options->max_response_bytes ?
                        options->max_response_bytes : FG_DEFAULT_LIMIT;
    client->timeout_ms = options && options->timeout_ms ? options->timeout_ms : FG_DEFAULT_TIMEOUT;
    client->clock_user = options ? options->clock_user : NULL;
    client->now_ms = options && options->now_ms ? options->now_ms : fg_now_ms;
    client->sleep_ms = options && options->sleep_ms ? options->sleep_ms : fg_sleep_ms;
    if (options) {
        status = fg_buffer_set(&client->user_agent, options->user_agent, error);
        if (status != FG_OK) { fg_client_free(client); return status; }
    }
    if (transport) client->transport = transport;
    else {
        status = fg_transport_default(&client->owned, error);
        if (status != FG_OK) { fg_client_free(client); return status; }
        client->transport = &client->owned;
        client->owns_transport = 1;
    }
    *out = client;
    return fg_fail(error, FG_OK, "");
}

void fg_client_free(fg_client *client)
{
    if (!client) return;
    if (client->owns_transport) fg_transport_default_free(&client->owned);
    fg_buffer_free(&client->user_agent);
    free(client);
}

static fg_status fg_client_request(fg_client *client, fg_str url,
                                   const fg_header *headers, size_t count, fg_str body,
                                   int post, int sec, fg_response *out, fg_error *error)
{
    fg_request request;
    fg_response response = {0};
    fg_header *combined = NULL;
    fg_status status;
    size_t i;
    int has_agent = 0;
    uint64_t now;
    if (!client || !out)
        return fg_fail(error, FG_INVALID, "Missing client or response output");
    request = (fg_request){url, headers, count, body, client->max_bytes, client->timeout_ms};
    status = fg_internal_validate_request(&request, out, error);
    if (status != FG_OK) return status;
    if ((post && !client->transport->post) || (!post && !client->transport->get))
        return fg_fail(error, FG_INVALID, "Transport does not implement requested method");
    if (sec) {
        status = fg_validate_url(url, 1, error);
        if (status != FG_OK) return status;
        if (!client->user_agent.len)
            return fg_fail(error, FG_INVALID, "SEC requests require a configured identifying User-Agent");
    }
    for (i = 0; i < count; ++i)
        if (fg_ascii_equal(headers[i].name, "user-agent")) has_agent = 1;
    if (!has_agent && client->user_agent.len) {
        if (count >= SIZE_MAX / sizeof(*combined))
            return fg_fail(error, FG_LIMIT, "Request header count overflow");
        combined = (fg_header *)malloc((count + 1) * sizeof(*combined));
        if (!combined) return fg_fail(error, FG_NOMEM, "Cannot allocate request headers");
        if (count) memcpy(combined, headers, count * sizeof(*combined));
        combined[count] = (fg_header){FG_STR("User-Agent"),
                           {client->user_agent.data, client->user_agent.len}};
        request.headers = combined;
        request.header_count = count + 1;
    }
    if (sec) {
        now = client->now_ms(client->clock_user);
        if (now == UINT64_MAX || (client->started && now < client->last_start)) {
            free(combined);
            return fg_fail(error, FG_IO, "SEC monotonic clock failed or moved backwards");
        }
        while (client->started && now - client->last_start < 120) {
            uint64_t previous = now;
            unsigned delay = (unsigned)(120 - (now - client->last_start));
            client->sleep_ms(client->clock_user, delay);
            now = client->now_ms(client->clock_user);
            if (now == UINT64_MAX || now <= previous) {
                free(combined);
                return fg_fail(error, FG_IO, "SEC sleep clock did not advance monotonically");
            }
        }
        client->last_start = now;
        client->started = 1;
    }
    (void)fg_fail(error, FG_OK, "");
    status = post ? client->transport->post(client->transport->user, &request, &response, error) :
                    client->transport->get(client->transport->user, &request, &response, error);
    free(combined);
    if (response.body.len > client->max_bytes || response.headers.len > client->max_bytes) {
        if (response.body.len > client->max_bytes) fg_buffer_free(&response.body);
        if (response.headers.len > client->max_bytes) fg_buffer_free(&response.headers);
        status = fg_fail(error, FG_LIMIT, "Response exceeds configured byte limit");
    } else if (status == FG_OK) {
        if (response.status < 100 || response.status > 599)
            status = fg_fail(error, FG_HTTP, "Transport returned an invalid HTTP status");
        else if (response.status < 200 || response.status >= 300)
            status = fg_fail(error, FG_HTTP, "HTTP response is not successful");
        else (void)fg_fail(error, FG_OK, "");
    } else if (error && error->code != status) {
        (void)fg_fail(error, status, "Transport callback failed");
    }
    fg_response_free(out);
    *out = response;
    if (error && status != FG_OK) error->http_status = response.status;
    return status;
}

fg_status fg_get(fg_client *client, fg_str url, const fg_header *headers, size_t count,
                 fg_response *out, fg_error *error)
{
    return fg_client_request(client, url, headers, count, (fg_str){NULL, 0}, 0, 0, out, error);
}

fg_status fg_post(fg_client *client, fg_str url, const fg_header *headers, size_t count,
                  fg_str body, fg_response *out, fg_error *error)
{
    return fg_client_request(client, url, headers, count, body, 1, 0, out, error);
}

fg_status fg_sec_get(fg_client *client, fg_str url, fg_response *out, fg_error *error)
{
    return fg_client_request(client, url, NULL, 0, (fg_str){NULL, 0}, 0, 1, out, error);
}

fg_status fg_read_file(fg_str path, size_t max_bytes, fg_buffer *out, fg_error *error)
{
    fg_buffer result = {0};
    fg_status status = FG_OK;
    FILE *file = NULL;
    char block[16384];
    size_t count, limit = max_bytes ? max_bytes : FG_DEFAULT_LIMIT;
    if (!fg_buffer_valid(out) || !path.len || !fg_utf8_valid(path, 0))
        return fg_fail(error, FG_INVALID, "Invalid UTF-8 file path or output buffer");
#ifdef _WIN32
    {
        int length;
        wchar_t *wide;
        errno_t code;
        if (path.len > INT_MAX)
            return fg_fail(error, FG_LIMIT, "File path is too long");
        length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, (int)path.len, NULL, 0);
        if (!length) return fg_internal_os_error(error, FG_IO, "UTF-8 file path conversion", GetLastError());
        wide = (wchar_t *)calloc((size_t)length + 1, sizeof(*wide));
        if (!wide) return fg_fail(error, FG_NOMEM, "Cannot allocate file path");
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, (int)path.len, wide, length)) {
            unsigned long code_win = GetLastError();
            free(wide);
            return fg_internal_os_error(error, FG_IO, "UTF-8 file path conversion", code_win);
        }
        code = _wfopen_s(&file, wide, L"rb");
        free(wide);
        if (code)
            return fg_internal_os_error(error, code == ENOENT ? FG_NOT_FOUND : FG_IO,
                                        "File open", (unsigned long)code);
    }
#else
    {
        fg_buffer name = {0};
        int code;
        status = fg_buffer_set(&name, path, error);
        if (status != FG_OK) return status;
        file = fopen(name.data, "rb");
        code = errno;
        fg_buffer_free(&name);
        if (!file) return fg_internal_os_error(error,
                    code == ENOENT || code == ENOTDIR ? FG_NOT_FOUND : FG_IO,
                    "File open", (unsigned long)code);
    }
#endif
    while ((count = fread(block, 1, sizeof(block), file)) != 0) {
        if (count > limit - result.len) {
            status = fg_fail(error, FG_LIMIT, "File exceeds configured byte limit");
            break;
        }
        status = fg_buffer_append(&result, block, count, error);
        if (status != FG_OK) break;
    }
    if (status == FG_OK && ferror(file))
        status = fg_internal_os_error(error, FG_IO, "File read", (unsigned long)errno);
    if (fclose(file) != 0 && status == FG_OK)
        status = fg_internal_os_error(error, FG_IO, "File close", (unsigned long)errno);
    if (status == FG_OK && !result.data)
        status = fg_buffer_set(&result, (fg_str){NULL, 0}, error);
    if (status == FG_OK) {
        fg_buffer_free(out);
        *out = result;
        (void)fg_fail(error, FG_OK, "");
    } else fg_buffer_free(&result);
    return status;
}

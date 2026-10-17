#include "fg.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FG_NATIVE_LIMIT ((size_t)256 * 1024 * 1024)
#define FG_NATIVE_TIMEOUT 30000u

fg_status fg_internal_validate_request(const fg_request *, fg_response *, fg_error *);
fg_status fg_internal_os_error(fg_error *, fg_status, const char *, unsigned long);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

typedef struct fg_native {
    HINTERNET session;
} fg_native;

static fg_status fg_to_wide(fg_str text, wchar_t **out, fg_error *error)
{
    int length;
    wchar_t *wide;
    *out = NULL;
    if (text.len > INT_MAX)
        return fg_fail(error, FG_LIMIT, "HTTP text is too long");
    if (!text.len) {
        *out = (wchar_t *)calloc(1, sizeof(wchar_t));
        return *out ? FG_OK : fg_fail(error, FG_NOMEM, "Cannot allocate HTTP text");
    }
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, (int)text.len, NULL, 0);
    if (!length) return fg_internal_os_error(error, FG_INVALID, "UTF-8 conversion", GetLastError());
    wide = (wchar_t *)calloc((size_t)length + 1, sizeof(*wide));
    if (!wide) return fg_fail(error, FG_NOMEM, "Cannot allocate HTTP text");
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, (int)text.len, wide, length)) {
        DWORD code = GetLastError();
        free(wide);
        return fg_internal_os_error(error, FG_INVALID, "UTF-8 conversion", code);
    }
    *out = wide;
    return FG_OK;
}

static fg_status fg_win_timeout(HINTERNET handle, ULONGLONG start, unsigned timeout,
                                fg_error *error)
{
    ULONGLONG elapsed = GetTickCount64() - start;
    unsigned remaining;
    if (elapsed >= timeout)
        return fg_internal_os_error(error, FG_IO, "HTTP deadline", ERROR_WINHTTP_TIMEOUT);
    remaining = timeout - (unsigned)elapsed;
    if (remaining > INT_MAX) remaining = INT_MAX;
    if (!WinHttpSetTimeouts(handle, (int)remaining, (int)remaining,
                            (int)remaining, (int)remaining))
        return fg_internal_os_error(error, FG_IO, "HTTP timeout configuration", GetLastError());
    return FG_OK;
}

static fg_status fg_win_headers(HINTERNET request, size_t limit, fg_buffer *out,
                                fg_error *error)
{
    DWORD size = 0, code;
    wchar_t *wide;
    int length;
    fg_status status;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, NULL, &size, WINHTTP_NO_HEADER_INDEX))
        return fg_buffer_set(out, (fg_str){NULL, 0}, error);
    code = GetLastError();
    if (code != ERROR_INSUFFICIENT_BUFFER)
        return fg_internal_os_error(error, FG_IO, "HTTP response header sizing", code);
    /* The raw Windows representation is UTF-16. Bound its allocation before
       conversion, then enforce the actual returned UTF-8 byte count as well. */
    if (size < sizeof(wchar_t) || (size_t)size / sizeof(wchar_t) - 1 > limit)
        return fg_fail(error, FG_LIMIT, "Response headers exceed configured byte limit");
    wide = (wchar_t *)calloc((size_t)size / sizeof(wchar_t) + 1, sizeof(wchar_t));
    if (!wide) return fg_fail(error, FG_NOMEM, "Cannot allocate response headers");
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, wide, &size, WINHTTP_NO_HEADER_INDEX)) {
        code = GetLastError();
        free(wide);
        return fg_internal_os_error(error, FG_IO, "HTTP response header read", code);
    }
    if (size / sizeof(wchar_t) > INT_MAX) {
        free(wide);
        return fg_fail(error, FG_LIMIT, "Response headers are too long");
    }
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                 (int)(size / sizeof(wchar_t)), NULL, 0, NULL, NULL);
    if (!length) {
        code = GetLastError();
        free(wide);
        return fg_internal_os_error(error, FG_IO, "Response header conversion", code);
    }
    if ((size_t)length > limit) {
        free(wide);
        return fg_fail(error, FG_LIMIT, "Response headers exceed configured byte limit");
    }
    out->data = (char *)malloc((size_t)length + 1);
    if (!out->data) { free(wide); return fg_fail(error, FG_NOMEM, "Cannot allocate response headers"); }
    out->capacity = (size_t)length + 1;
    status = FG_OK;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                             (int)(size / sizeof(wchar_t)), out->data, length, NULL, NULL))
        status = fg_internal_os_error(error, FG_IO, "Response header conversion", GetLastError());
    else {
        out->len = (size_t)length;
        while (out->len && out->data[out->len - 1] == '\0') --out->len;
        out->data[out->len] = '\0';
    }
    free(wide);
    return status;
}

static fg_status fg_native_request(void *user, const fg_request *input,
                                   fg_response *out, fg_error *error, int post)
{
    fg_native *native = (fg_native *)user;
    fg_response response = {0};
    fg_buffer headers = {0};
    wchar_t *url = NULL, *host = NULL, *path = NULL, *wide_headers = NULL;
    URL_COMPONENTS parts;
    HINTERNET connection = NULL, request = NULL;
    fg_status status;
    size_t i, limit;
    unsigned timeout;
    DWORD option, http_status = 0, size = sizeof(http_status), read;
    ULONGLONG start;
    char block[16384];
    status = fg_internal_validate_request(input, out, error);
    if (status != FG_OK) return status;
    if (!native || !native->session)
        return fg_fail(error, FG_INVALID, "Invalid default transport context");
    if (!post && input->body.len)
        return fg_fail(error, FG_INVALID, "GET requests cannot carry a body");
    if (input->body.len > MAXDWORD)
        return fg_fail(error, FG_LIMIT, "HTTP request body exceeds WinHTTP limit");
    limit = input->max_bytes ? input->max_bytes : FG_NATIVE_LIMIT;
    timeout = input->timeout_ms ? input->timeout_ms : FG_NATIVE_TIMEOUT;
    (void)fg_fail(error, FG_OK, "");
    status = fg_to_wide(input->url, &url, error);
    if (status != FG_OK) goto done;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
        status = fg_internal_os_error(error, FG_INVALID, "HTTP URL parsing", GetLastError());
        goto done;
    }
    host = (wchar_t *)calloc((size_t)parts.dwHostNameLength + 1, sizeof(*host));
    if (!host) { status = fg_fail(error, FG_NOMEM, "Cannot allocate HTTP host"); goto done; }
    memcpy(host, parts.lpszHostName, parts.dwHostNameLength * sizeof(*host));
    {
        size_t path_length = (size_t)parts.dwUrlPathLength + parts.dwExtraInfoLength;
        size_t offset = parts.dwUrlPathLength ? 0 : 1;
        if (path_length > SIZE_MAX / sizeof(*path) - offset - 1) {
            status = fg_fail(error, FG_LIMIT, "HTTP path size overflow"); goto done;
        }
        path = (wchar_t *)calloc(path_length + offset + 1, sizeof(*path));
        if (!path) { status = fg_fail(error, FG_NOMEM, "Cannot allocate HTTP path"); goto done; }
        if (offset) path[0] = L'/';
        if (parts.dwUrlPathLength)
            memcpy(path + offset, parts.lpszUrlPath, parts.dwUrlPathLength * sizeof(*path));
        if (parts.dwExtraInfoLength)
            memcpy(path + offset + parts.dwUrlPathLength, parts.lpszExtraInfo,
                    parts.dwExtraInfoLength * sizeof(*path));
    }
    for (i = 0; i < input->header_count; ++i) {
        const fg_header *header = input->headers + i;
        status = fg_buffer_append(&headers, header->name.data, header->name.len, error);
        if (status == FG_OK) status = fg_buffer_append(&headers, ": ", 2, error);
        if (status == FG_OK) status = fg_buffer_append(&headers, header->value.data, header->value.len, error);
        if (status == FG_OK) status = fg_buffer_append(&headers, "\r\n", 2, error);
        if (status != FG_OK) goto done;
    }
    if (headers.len) {
        status = fg_to_wide((fg_str){headers.data, headers.len}, &wide_headers, error);
        if (status != FG_OK) goto done;
    }
    start = GetTickCount64();
    connection = WinHttpConnect(native->session, host, parts.nPort, 0);
    if (!connection) { status = fg_internal_os_error(error, FG_IO, "HTTP connect", GetLastError()); goto done; }
    request = WinHttpOpenRequest(connection, post ? L"POST" : L"GET", path, NULL,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) { status = fg_internal_os_error(error, FG_IO, "HTTP request creation", GetLastError()); goto done; }
    /* WinHTTP has no automatic cookie jar. Raw Set-Cookie headers are returned
       unchanged so source adapters can supply a scoped Cookie header. Redirects
       are disabled, preventing every caller-supplied secret from changing origin. */
    option = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option))) {
        status = fg_internal_os_error(error, FG_IO, "HTTP security configuration", GetLastError()); goto done;
    }
    option = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &option, sizeof(option))) {
        status = fg_internal_os_error(error, FG_IO, "HTTP decompression configuration", GetLastError()); goto done;
    }
    /* Never enable SECURITY_FLAG_IGNORE_*: WinHTTP's certificate and hostname
       validation remain in force. No default/system proxy or Windows credentials. */
    status = fg_win_timeout(request, start, timeout, error);
    if (status != FG_OK) goto done;
    if (!WinHttpSendRequest(request, wide_headers, wide_headers ? (DWORD)-1 : 0,
                            input->body.len ? (void *)input->body.data : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)input->body.len, (DWORD)input->body.len, 0)) {
        status = fg_internal_os_error(error, FG_IO, "HTTP send", GetLastError()); goto done;
    }
    status = fg_win_timeout(request, start, timeout, error);
    if (status != FG_OK) goto done;
    if (!WinHttpReceiveResponse(request, NULL)) {
        status = fg_internal_os_error(error, FG_IO, "HTTP receive", GetLastError()); goto done;
    }
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &http_status, &size, WINHTTP_NO_HEADER_INDEX)) {
        status = fg_internal_os_error(error, FG_IO, "HTTP status read", GetLastError()); goto done;
    }
    response.status = (int)http_status;
    status = fg_win_headers(request, limit, &response.headers, error);
    if (status != FG_OK) goto done;
    for (;;) {
        status = fg_win_timeout(request, start, timeout, error);
        if (status != FG_OK) goto done;
        if (!WinHttpReadData(request, block, sizeof(block), &read)) {
            status = fg_internal_os_error(error, FG_IO, "HTTP body read", GetLastError()); goto done;
        }
        if (!read) break;
        if ((size_t)read > limit - response.body.len) {
            status = fg_fail(error, FG_LIMIT, "Response body exceeds configured byte limit"); goto done;
        }
        status = fg_buffer_append(&response.body, block, read, error);
        if (status != FG_OK) goto done;
    }
    if (!response.body.data) status = fg_buffer_set(&response.body, (fg_str){NULL, 0}, error);
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    free(wide_headers);
    free(path);
    free(host);
    free(url);
    fg_buffer_free(&headers);
    fg_response_free(out);
    *out = response;
    if (error && status != FG_OK) error->http_status = response.status;
    return status;
}

#else
#include <curl/curl.h>

typedef struct fg_native {
    CURL *easy;
} fg_native;

typedef struct fg_curl_receive {
    fg_response *response;
    fg_error *error;
    size_t limit;
    fg_status status;
} fg_curl_receive;

static size_t fg_curl_write(fg_curl_receive *receive, fg_buffer *target,
                            const char *data, size_t size, size_t count)
{
    size_t bytes;
    if (receive->status != FG_OK) return 0;
    if (size && count > SIZE_MAX / size) {
        receive->status = fg_fail(receive->error, FG_LIMIT, "HTTP response size overflow");
        return 0;
    }
    bytes = size * count;
    if (bytes > receive->limit - target->len) {
        receive->status = fg_fail(receive->error, FG_LIMIT, "Response exceeds configured byte limit");
        return 0;
    }
    receive->status = fg_buffer_append(target, data, bytes, receive->error);
    return receive->status == FG_OK ? bytes : 0;
}

static size_t fg_curl_body(char *data, size_t size, size_t count, void *user)
{
    fg_curl_receive *receive = (fg_curl_receive *)user;
    return fg_curl_write(receive, &receive->response->body, data, size, count);
}

static size_t fg_curl_headers(char *data, size_t size, size_t count, void *user)
{
    fg_curl_receive *receive = (fg_curl_receive *)user;
    return fg_curl_write(receive, &receive->response->headers, data, size, count);
}

static fg_status fg_curl_error(fg_error *error, CURLcode code, const char *operation)
{
    char message[256];
    (void)snprintf(message, sizeof(message), "%s failed (curl error %d: %s)",
                   operation, (int)code, curl_easy_strerror(code));
    return fg_fail(error, code == CURLE_OUT_OF_MEMORY ? FG_NOMEM : FG_IO, message);
}

static fg_status fg_native_request(void *user, const fg_request *input,
                                   fg_response *out, fg_error *error, int post)
{
    fg_native *native = (fg_native *)user;
    fg_response response = {0};
    fg_buffer url = {0}, header = {0};
    struct curl_slist *headers = NULL, *next;
    fg_curl_receive receive;
    fg_status status;
    CURLcode code;
    long http_status = 0, os_error = 0;
    size_t i;
    unsigned timeout;
    status = fg_internal_validate_request(input, out, error);
    if (status != FG_OK) return status;
    if (!native || !native->easy)
        return fg_fail(error, FG_INVALID, "Invalid default transport context");
    if (!post && input->body.len)
        return fg_fail(error, FG_INVALID, "GET requests cannot carry a body");
#if SIZE_MAX > INT64_MAX
    if (input->body.len > (uint64_t)INT64_MAX)
        return fg_fail(error, FG_LIMIT, "HTTP request body exceeds curl limit");
#endif
    timeout = input->timeout_ms ? input->timeout_ms : FG_NATIVE_TIMEOUT;
#if UINT_MAX > LONG_MAX
    if (timeout > (unsigned)LONG_MAX)
        return fg_fail(error, FG_LIMIT, "HTTP timeout exceeds curl limit");
#endif
    (void)fg_fail(error, FG_OK, "");
    receive = (fg_curl_receive){&response, error,
               input->max_bytes ? input->max_bytes : FG_NATIVE_LIMIT, FG_OK};
    status = fg_buffer_set(&url, input->url, error);
    if (status != FG_OK) goto done;
    for (i = 0; i < input->header_count; ++i) {
        status = fg_buffer_set(&header, input->headers[i].name, error);
        /* libcurl uses "Name;" to send a genuinely empty header. "Name:" would
           instead remove its internally generated header of that name. */
        if (status == FG_OK) status = fg_buffer_append(&header,
                 input->headers[i].value.len ? ": " : ";",
                 input->headers[i].value.len ? 2 : 1, error);
        if (status == FG_OK) status = fg_buffer_append(&header,
                 input->headers[i].value.data, input->headers[i].value.len, error);
        if (status != FG_OK) goto done;
        next = curl_slist_append(headers, header.data);
        if (!next) { status = fg_fail(error, FG_NOMEM, "Cannot allocate HTTP headers"); goto done; }
        headers = next;
    }
    curl_easy_reset(native->easy);
#define FG_CURL_SET(option, value) do { \
    code = curl_easy_setopt(native->easy, option, value); \
    if (code != CURLE_OK) { status = fg_curl_error(error, code, "HTTP option"); goto done; } \
} while (0)
    FG_CURL_SET(CURLOPT_URL, url.data);
    FG_CURL_SET(CURLOPT_NOSIGNAL, 1L);
    FG_CURL_SET(CURLOPT_TIMEOUT_MS, (long)timeout);
    FG_CURL_SET(CURLOPT_CONNECTTIMEOUT_MS, (long)timeout);
    FG_CURL_SET(CURLOPT_SSL_VERIFYPEER, 1L);
    FG_CURL_SET(CURLOPT_SSL_VERIFYHOST, 2L);
    FG_CURL_SET(CURLOPT_FOLLOWLOCATION, 0L);
    FG_CURL_SET(CURLOPT_MAXREDIRS, 0L);
    FG_CURL_SET(CURLOPT_UNRESTRICTED_AUTH, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    FG_CURL_SET(CURLOPT_PROTOCOLS_STR, "http,https");
    FG_CURL_SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    FG_CURL_SET(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    FG_CURL_SET(CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    FG_CURL_SET(CURLOPT_PROXY, "");
    FG_CURL_SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    FG_CURL_SET(CURLOPT_ACCEPT_ENCODING, "");
    FG_CURL_SET(CURLOPT_FAILONERROR, 0L);
    /* Never enable curl's cookie engine: only caller-scoped Cookie headers apply. */
    FG_CURL_SET(CURLOPT_COOKIEFILE, NULL);
    FG_CURL_SET(CURLOPT_HTTPHEADER, headers);
    FG_CURL_SET(CURLOPT_WRITEFUNCTION, fg_curl_body);
    FG_CURL_SET(CURLOPT_WRITEDATA, &receive);
    FG_CURL_SET(CURLOPT_HEADERFUNCTION, fg_curl_headers);
    FG_CURL_SET(CURLOPT_HEADERDATA, &receive);
    if (post) {
        FG_CURL_SET(CURLOPT_POST, 1L);
        FG_CURL_SET(CURLOPT_POSTFIELDS, input->body.data ? input->body.data : "");
        FG_CURL_SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)input->body.len);
    } else FG_CURL_SET(CURLOPT_HTTPGET, 1L);
    code = curl_easy_perform(native->easy);
    (void)curl_easy_getinfo(native->easy, CURLINFO_RESPONSE_CODE, &http_status);
    response.status = (int)http_status;
    if (receive.status != FG_OK) status = receive.status;
    else if (code != CURLE_OK) {
        (void)curl_easy_getinfo(native->easy, CURLINFO_OS_ERRNO, &os_error);
        status = fg_curl_error(error, code, "HTTP request");
        if (error && os_error) {
            size_t length = strlen(error->message);
            (void)snprintf(error->message + length, sizeof(error->message) - length,
                           " (OS error %ld)", os_error);
        }
    } else {
        status = FG_OK;
        if (!response.body.data) status = fg_buffer_set(&response.body, (fg_str){NULL, 0}, error);
    }
#undef FG_CURL_SET
done:
    /* Clear pointers to stack-owned callbacks and borrowed request memory. */
    curl_easy_reset(native->easy);
    curl_slist_free_all(headers);
    fg_buffer_free(&header);
    fg_buffer_free(&url);
    fg_response_free(out);
    *out = response;
    if (error && status != FG_OK) error->http_status = response.status;
    return status;
}
#endif

static fg_status fg_native_get(void *user, const fg_request *request,
                               fg_response *response, fg_error *error)
{
    return fg_native_request(user, request, response, error, 0);
}

static fg_status fg_native_post(void *user, const fg_request *request,
                                fg_response *response, fg_error *error)
{
    return fg_native_request(user, request, response, error, 1);
}

fg_status fg_transport_default(fg_transport *out, fg_error *error)
{
    fg_native *native;
    if (!out) return fg_fail(error, FG_INVALID, "Missing default transport output");
    memset(out, 0, sizeof(*out));
    native = (fg_native *)calloc(1, sizeof(*native));
    if (!native) return fg_fail(error, FG_NOMEM, "Cannot allocate default transport");
#ifdef _WIN32
    native->session = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!native->session) {
        DWORD code = GetLastError();
        free(native);
        return fg_internal_os_error(error, FG_IO, "HTTP session creation", code);
    }
#else
    {
        CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) { free(native); return fg_curl_error(error, code, "curl initialization"); }
        native->easy = curl_easy_init();
        if (!native->easy) {
            curl_global_cleanup();
            free(native);
            return fg_fail(error, FG_NOMEM, "Cannot allocate curl session");
        }
    }
#endif
    out->user = native;
    out->get = fg_native_get;
    out->post = fg_native_post;
    return fg_fail(error, FG_OK, "");
}

void fg_transport_default_free(fg_transport *transport)
{
    fg_native *native;
    if (!transport || transport->get != fg_native_get || transport->post != fg_native_post)
        return;
    native = (fg_native *)transport->user;
    if (native) {
#ifdef _WIN32
        if (native->session) WinHttpCloseHandle(native->session);
#else
        if (native->easy) curl_easy_cleanup(native->easy);
        curl_global_cleanup();
#endif
        free(native);
    }
    memset(transport, 0, sizeof(*transport));
}

#include "test.h"

#include <errno.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct test_context {
    uint64_t now;
    uint64_t starts[32];
    unsigned sleeps[32];
    size_t calls, sleep_count;
    int post, code, clock_mode;
    size_t seen_limit;
    unsigned seen_timeout;
    fg_status failure;
    fg_buffer body, agent;
    const char *response_body;
} test_context;

static uint64_t test_now(void *user)
{
    return ((test_context *)user)->now;
}

static void test_sleep(void *user, unsigned milliseconds)
{
    test_context *context = (test_context *)user;
    if (context->sleep_count < 32) context->sleeps[context->sleep_count++] = milliseconds;
    if (context->clock_mode == 1) return;
    if (context->clock_mode == 2) { --context->now; return; }
    if (context->clock_mode == 3) milliseconds = 1;
    context->now += milliseconds;
}

static fg_status test_request(test_context *context, const fg_request *request,
                              fg_response *response, fg_error *error, int post)
{
    size_t i;
    fg_status status;
    if (context->calls < 32) context->starts[context->calls] = context->now;
    ++context->calls;
    context->post = post;
    context->seen_limit = request->max_bytes;
    context->seen_timeout = request->timeout_ms;
    status = fg_buffer_set(&context->body, request->body, error);
    if (status != FG_OK) return status;
    status = fg_buffer_set(&context->agent, (fg_str){NULL, 0}, error);
    if (status != FG_OK) return status;
    for (i = 0; i < request->header_count; ++i) {
        if (fg_str_equal(request->headers[i].name, FG_STR("User-Agent")) ||
            fg_str_equal(request->headers[i].name, FG_STR("user-agent"))) {
            status = fg_buffer_set(&context->agent, request->headers[i].value, error);
            if (status != FG_OK) return status;
        }
    }
    response->status = context->code ? context->code : 200;
    status = fg_buffer_set(&response->body, FG_STR(context->response_body ?
                           context->response_body : "fixture response"), error);
    if (status != FG_OK) return status;
    status = fg_buffer_set(&response->headers, FG_STR("Set-Cookie: A=seed; Secure\r\n"), error);
    if (status != FG_OK) return status;
    return context->failure ? fg_fail(error, context->failure, "fixture failure 123") : FG_OK;
}

static fg_status test_get(void *user, const fg_request *request,
                          fg_response *response, fg_error *error)
{
    return test_request((test_context *)user, request, response, error, 0);
}

static fg_status test_post(void *user, const fg_request *request,
                           fg_response *response, fg_error *error)
{
    return test_request((test_context *)user, request, response, error, 1);
}

static void test_context_free(test_context *context)
{
    fg_buffer_free(&context->body);
    fg_buffer_free(&context->agent);
}

static int test_buffers(void)
{
    fg_buffer buffer = {0};
    fg_error error = {0};
    char binary[] = {'a', '\0', 'b'};
    char data[100];
    size_t i;
    memset(data, 'x', sizeof(data));
    CHECK(fg_string(NULL).len == 0);
    CHECK(fg_str_equal(FG_STR("a"), (fg_str){"abc", 1}));
    CHECK(fg_str_equal((fg_str){NULL, 0}, FG_STR("")));
    CHECK(!fg_str_equal((fg_str){NULL, 1}, FG_STR("a")));
    OK(fg_buffer_append(&buffer, NULL, 0, &error));
    CHECK(buffer.data && !buffer.len && !buffer.data[0]);
    OK(fg_buffer_set(&buffer, (fg_str){binary, sizeof(binary)}, &error));
    CHECK(buffer.len == 3 && !memcmp(buffer.data, binary, 3) && !buffer.data[3]);
    OK(fg_buffer_append(&buffer, buffer.data, buffer.len, &error));
    CHECK(buffer.len == 6 && !memcmp(buffer.data + 3, binary, 3));
    OK(fg_buffer_set(&buffer, (fg_str){data, sizeof(data)}, &error));
    OK(fg_buffer_append(&buffer, buffer.data, buffer.len, &error));
    CHECK(buffer.len == 200);
    for (i = 0; i < buffer.len; ++i) CHECK(buffer.data[i] == 'x');
    OK(fg_buffer_set(&buffer, FG_STR("0123456789"), &error));
    OK(fg_buffer_set(&buffer, (fg_str){buffer.data + 2, 5}, &error));
    CHECK(fg_str_equal(FG_STR("23456"), (fg_str){buffer.data, buffer.len}));
    CHECK(fg_buffer_append(&buffer, "x", SIZE_MAX, &error) == FG_LIMIT);
    CHECK(buffer.len == 5 && !strcmp(buffer.data, "23456"));
    CHECK(fg_buffer_set(&buffer, (fg_str){NULL, 1}, &error) == FG_INVALID);
    CHECK(fg_buffer_append(NULL, NULL, 0, &error) == FG_INVALID);
    CHECK(fg_buffer_append(&buffer, buffer.data + 2, buffer.capacity, &error) == FG_INVALID);
    OK(fg_buffer_set(&buffer, FG_STR("a b/\xc3\xa9?"), &error));
    OK(fg_url_encode(&buffer, (fg_str){buffer.data, buffer.len}, &error));
    CHECK(!strcmp(buffer.data, "a%20b%2F%C3%A9%3F"));
    OK(fg_url_encode(&buffer, (fg_str){binary, sizeof(binary)}, &error));
    CHECK(!strcmp(buffer.data, "a%00b"));
    CHECK(fg_url_encode(&buffer, (fg_str){"x", SIZE_MAX}, &error) == FG_LIMIT);
    CHECK(!strcmp(buffer.data, "a%00b"));
    OK(fg_url_encode(&buffer, FG_STR("AZaz09-_.~"), &error));
    CHECK(!strcmp(buffer.data, "AZaz09-_.~"));
    OK(fg_url_encode(&buffer, (fg_str){NULL, 0}, &error));
    CHECK(buffer.data && !buffer.len && !buffer.data[0]);
    fg_buffer_free(&buffer);
    fg_buffer_free(&buffer);
    CHECK(!buffer.data && !buffer.len && !buffer.capacity);
    fg_buffer_free(NULL);
    fg_response_free(NULL);
    fg_client_free(NULL);
    fg_transport_default_free(NULL);
    CHECK(!strcmp(fg_status_name(FG_CHECKSUM), "FG_CHECKSUM"));
    CHECK(!strcmp(fg_status_name((fg_status)100), "FG_UNKNOWN"));
    memset(data, 'm', sizeof(data) - 1);
    data[sizeof(data) - 1] = 0;
    CHECK(fg_fail(NULL, FG_IO, data) == FG_IO);
    error.http_status = 500;
    CHECK(fg_fail(&error, FG_PARSE, data) == FG_PARSE);
    CHECK(error.code == FG_PARSE && error.http_status == 0 && !strcmp(error.message, data));
    CHECK(fg_fail(&error, FG_INVALID, error.message) == FG_INVALID);
    CHECK(!strcmp(error.message, data));
    return 0;
}

static int test_requests(void)
{
    test_context context = {0};
    fg_transport transport = {&context, test_get, test_post};
    fg_client *client = NULL;
    fg_client_options options = {0};
    fg_response response = {0};
    fg_error error = {0};
    fg_header header = {FG_STR("X-API-Key"), FG_STR("private-value")};
    const char binary[] = {'a', '\0', 'b'};
    const char *invalid_urls[] = {
        "", "file:///secret", "http://example.com/", "ftp://example.com/",
        "https://user:password@example.com/", "https://example.com/\r\nX: a",
        "https://example.com/a b", "https://example.com\\@evil.test/",
        "https://example.com/#fragment", "https://example.com:0/",
        "https://example.com:65536/", "https://example.com:/",
        "https://example.com:abc/", "https:///x", "https://.example.com/",
        "https://example..com/", "https://example.com./", "https://-example.com/",
        "https://example-.com/", "https://example.com/%G0", "https://example.com/%",
        "https://example.com/\xc0\xaf", "https://[::1", "https://[::g]/",
        "http://127.0.0.1.evil.test/", "http://localhost.evil.test/", "http://127.1/"
    };
    size_t i, calls;
    OK(fg_client_create(&transport, NULL, &client, &error));
    OK(fg_get(client, FG_STR("https://example.com/path?x=1"), &header, 1, &response, &error));
    CHECK(!context.post && context.calls == 1 && response.status == 200);
    CHECK(context.seen_limit == (size_t)256 * 1024 * 1024 && context.seen_timeout == 30000);
    CHECK(fg_str_equal((fg_str){response.body.data, response.body.len}, FG_STR("fixture response")));
    OK(fg_post(client, FG_STR("https://example.com/post"), &header, 1,
               (fg_str){binary, sizeof(binary)}, &response, &error));
    CHECK(context.post && context.body.len == sizeof(binary));
    CHECK(!memcmp(context.body.data, binary, sizeof(binary)));
    OK(fg_post(client, FG_STR("https://example.com/post"), NULL, 0,
               (fg_str){NULL, 0}, &response, &error));
    CHECK(context.post && !context.body.len);
    for (i = 0; i < sizeof(invalid_urls) / sizeof(invalid_urls[0]); ++i) {
        calls = context.calls;
        CHECK(fg_get(client, FG_STR(invalid_urls[i]), NULL, 0, &response, &error) == FG_INVALID);
        CHECK(context.calls == calls);
    }
    {
        const char nul_url[] = "https://example.com/\0secret";
        CHECK(fg_get(client, (fg_str){nul_url, sizeof(nul_url) - 1}, NULL, 0,
                      &response, &error) == FG_INVALID);
    }
    OK(fg_get(client, (fg_str){"https://example.com/suffix", 19}, NULL, 0, &response, &error));
    OK(fg_get(client, FG_STR("http://localhost:1234/"), NULL, 0, &response, &error));
    OK(fg_get(client, FG_STR("http://127.0.0.1:1234/"), NULL, 0, &response, &error));
    OK(fg_get(client, FG_STR("http://[::1]:1234/"), NULL, 0, &response, &error));
    calls = context.calls;
    header.name = FG_STR("Bad Header");
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error) == FG_INVALID);
    header.name = FG_STR("X-Test\r\nEvil");
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error) == FG_INVALID);
    header.name = FG_STR("X-Test");
    header.value = FG_STR("value\r\nEvil: bad");
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error) == FG_INVALID);
    header.value = (fg_str){"nul\0value", 9};
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error) == FG_INVALID);
    header.name = FG_STR("Host");
    header.value = FG_STR("evil.test");
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error) == FG_INVALID);
    CHECK(fg_get(client, FG_STR("https://example.com"), NULL, 1, &response, &error) == FG_INVALID);
    CHECK(fg_get(client, FG_STR("https://example.com"), &header, SIZE_MAX,
                  &response, &error) == FG_LIMIT);
    CHECK(fg_post(client, FG_STR("https://example.com"), NULL, 0, (fg_str){NULL, 1},
                   &response, &error) == FG_INVALID);
    CHECK(context.calls == calls);
    context.code = 404;
    CHECK(fg_get(client, FG_STR("https://example.test/missing"), NULL, 0, &response, &error) == FG_HTTP);
    CHECK(response.status == 404 && error.http_status == 404 && error.code == FG_HTTP);
    CHECK(response.body.len && strstr(response.headers.data, "Set-Cookie: A=seed"));
    CHECK(!strstr(error.message, "private-value") && !strstr(error.message, "fixture response"));
    context.code = 302;
    CHECK(fg_get(client, FG_STR("https://example.com/redirect"), NULL, 0, &response, &error) == FG_HTTP);
    CHECK(error.http_status == 302);
    context.code = 99;
    CHECK(fg_get(client, FG_STR("https://example.com"), NULL, 0, &response, &error) == FG_HTTP);
    context.code = 500;
    context.failure = FG_IO;
    CHECK(fg_get(client, FG_STR("https://example.com"), NULL, 0, &response, &error) == FG_IO);
    CHECK(error.code == FG_IO && error.http_status == 500 && !strcmp(error.message, "fixture failure 123"));
    CHECK(response.status == 500 && response.body.len);
    context.failure = FG_OK;
    context.code = 204;
    context.response_body = "";
    OK(fg_get(client, FG_STR("https://example.com"), NULL, 0, &response, &error));
    CHECK(error.code == FG_OK && !error.http_status && !error.message[0]);
    CHECK(response.body.data && !response.body.len);
    fg_client_free(client);
    client = NULL;
    options.max_response_bytes = 32;
    options.timeout_ms = 750;
    options.user_agent = FG_STR("research-client");
    OK(fg_client_create(&transport, &options, &client, &error));
    context.response_body = "01234567890123456789012345678901";
    OK(fg_get(client, FG_STR("https://example.com"), NULL, 0, &response, &error));
    CHECK(context.seen_limit == 32 && context.seen_timeout == 750);
    CHECK(!strcmp(context.agent.data, "research-client"));
    header.name = FG_STR("user-agent");
    header.value = FG_STR("explicit-agent");
    OK(fg_get(client, FG_STR("https://example.com"), &header, 1, &response, &error));
    CHECK(!strcmp(context.agent.data, "explicit-agent"));
    context.response_body = "012345678901234567890123456789012";
    CHECK(fg_get(client, FG_STR("https://example.com"), NULL, 0, &response, &error) == FG_LIMIT);
    CHECK(response.body.len <= 32 && error.code == FG_LIMIT);
    fg_client_free(client);
    client = NULL;
    transport.post = NULL;
    OK(fg_client_create(&transport, NULL, &client, &error));
    CHECK(fg_post(client, FG_STR("https://example.com"), NULL, 0, FG_STR(""),
                   &response, &error) == FG_INVALID);
    fg_client_free(client);
    fg_response_free(&response);
    fg_response_free(&response);
    test_context_free(&context);
    return 0;
}

static int test_sec(void)
{
    test_context context = {0};
    fg_transport transport = {&context, test_get, test_post};
    fg_client_options options = {0};
    fg_client *client = NULL;
    fg_response response = {0};
    fg_error error = {0};
    size_t calls;
    const char *invalid[] = {
        "https://sec.gov.evil.test/", "https://evilsec.gov/", "https://sec.gov@evil.test/",
        "https://evil.test/", "http://sec.gov/", "https://sec.gov:444/",
        "http://127.0.0.1/", "https://sec.gov./"
    };
    size_t i;
    options.now_ms = test_now;
    options.clock_user = &context;
    CHECK(fg_client_create(&transport, &options, &client, &error) == FG_INVALID);
    CHECK(!client);
    options.sleep_ms = test_sleep;
    OK(fg_client_create(&transport, &options, &client, &error));
    CHECK(fg_sec_get(client, FG_STR("https://data.sec.gov/x"), &response, &error) == FG_INVALID);
    CHECK(!context.calls);
    fg_client_free(client);
    client = NULL;
    options.user_agent = FG_STR("configured-by-the-caller");
    OK(fg_client_create(&transport, &options, &client, &error));
    OK(fg_sec_get(client, FG_STR("https://data.sec.gov/x"), &response, &error));
    OK(fg_sec_get(client, FG_STR("https://www.sec.gov/y"), &response, &error));
    CHECK(context.calls == 2 && context.starts[0] == 0 && context.starts[1] == 120);
    CHECK(context.sleep_count == 1 && context.sleeps[0] == 120);
    CHECK(!strcmp(context.agent.data, "configured-by-the-caller"));
    context.now += 40;
    OK(fg_sec_get(client, FG_STR("https://SEC.GOV:443/z"), &response, &error));
    CHECK(context.starts[2] == 240 && context.sleeps[1] == 80);
    context.now += 1000;
    OK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error));
    CHECK(context.sleep_count == 2 && context.starts[3] == 1240);
    context.failure = FG_IO;
    CHECK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error) == FG_IO);
    context.failure = FG_OK;
    OK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error));
    CHECK(context.starts[5] - context.starts[4] == 120);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        calls = context.calls;
        CHECK(fg_sec_get(client, FG_STR(invalid[i]), &response, &error) == FG_INVALID);
        CHECK(context.calls == calls);
    }
    calls = context.calls;
    --context.now;
    CHECK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error) == FG_IO);
    CHECK(context.calls == calls);
    ++context.now;
    context.clock_mode = 1;
    CHECK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error) == FG_IO);
    CHECK(context.calls == calls);
    context.clock_mode = 2;
    CHECK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error) == FG_IO);
    CHECK(context.calls == calls);
    ++context.now;
    context.clock_mode = 3;
    OK(fg_sec_get(client, FG_STR("https://www.sec.gov/z"), &response, &error));
    CHECK(context.starts[6] - context.starts[5] >= 120);
    fg_client_free(client);
    fg_response_free(&response);
    test_context_free(&context);
    return 0;
}

static int test_files(void)
{
    fg_buffer buffer = {0};
    fg_error error = {0};
    FILE *file;
    const char bytes[] = {'a', '\0', 'b', '\xff'};
    /* CTest runs in the build directory. Keep fixtures beneath that directory,
       never in a global/system temporary directory. */
#ifdef _WIN32
    CHECK(_mkdir("tmp") == 0 || errno == EEXIST);
    CHECK(fopen_s(&file, "tmp\\fg_transport_file.bin", "wb") == 0);
#else
    CHECK(mkdir("tmp", 0700) == 0 || errno == EEXIST);
    file = fopen("tmp/fg_transport_file.bin", "wb");
#endif
    CHECK(file != NULL);
    CHECK(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    CHECK(fclose(file) == 0);
#ifdef _WIN32
#define TEST_FILE "tmp\\fg_transport_file.bin"
#else
#define TEST_FILE "tmp/fg_transport_file.bin"
#endif
    OK(fg_read_file(FG_STR(TEST_FILE), sizeof(bytes), &buffer, &error));
    CHECK(buffer.len == sizeof(bytes) && !memcmp(buffer.data, bytes, sizeof(bytes)));
    CHECK(buffer.data[buffer.len] == '\0');
    CHECK(fg_read_file(FG_STR(TEST_FILE), 3, &buffer, &error) == FG_LIMIT);
    CHECK(buffer.len == sizeof(bytes) && !memcmp(buffer.data, bytes, sizeof(bytes)));
    OK(fg_read_file(FG_STR(TEST_FILE), 0, &buffer, &error));
    CHECK(fg_read_file((fg_str){NULL, 1}, 0, &buffer, &error) == FG_INVALID);
    CHECK(fg_read_file((fg_str){"name\0hidden", 11}, 0, &buffer, &error) == FG_INVALID);
    CHECK(fg_read_file(FG_STR(""), 0, &buffer, &error) == FG_INVALID);
    CHECK(remove(TEST_FILE) == 0);
    CHECK(fg_read_file(FG_STR(TEST_FILE), 0, &buffer, &error) == FG_NOT_FOUND);
    CHECK(strstr(error.message, "OS error") && buffer.len == sizeof(bytes));
#ifdef _WIN32
    CHECK(_wfopen_s(&file, L"tmp\\fg_transport_\x03bb.txt", L"wb") == 0);
#else
    file = fopen("tmp/fg_transport_\xce\xbb.txt", "wb");
#endif
    CHECK(file);
    CHECK(fclose(file) == 0);
#ifdef _WIN32
    OK(fg_read_file(FG_STR("tmp\\fg_transport_\xce\xbb.txt"), 1, &buffer, &error));
    CHECK(_wremove(L"tmp\\fg_transport_\x03bb.txt") == 0);
#else
    OK(fg_read_file(FG_STR("tmp/fg_transport_\xce\xbb.txt"), 1, &buffer, &error));
    CHECK(remove("tmp/fg_transport_\xce\xbb.txt") == 0);
#endif
    CHECK(buffer.data && !buffer.len && !buffer.data[0]);
    CHECK(fg_read_file(FG_STR("tmp"), 1, &buffer, &error) == FG_IO);
    fg_buffer_free(&buffer);
    return 0;
}

static int test_native_validation(void)
{
    fg_transport transport = {0}, empty = {0};
    fg_client *client = NULL, *borrower = NULL;
    fg_response response = {0};
    fg_error error = {0};
    fg_request request = {0};
    fg_header bad = {FG_STR("X-Test"), FG_STR("evil\r\nHeader: value")};
    size_t i;
    CHECK(fg_client_create(&empty, NULL, &client, &error) == FG_INVALID);
    CHECK(!client);
    CHECK(fg_client_create(NULL, NULL, NULL, &error) == FG_INVALID);
    CHECK(fg_transport_default(NULL, &error) == FG_INVALID);
    for (i = 0; i < 3; ++i) {
        OK(fg_transport_default(&transport, &error));
        CHECK(transport.user && transport.get && transport.post);
        OK(fg_client_create(&transport, NULL, &client, &error));
        OK(fg_client_create(&transport, NULL, &borrower, &error));
        fg_client_free(client);
        fg_client_free(borrower);
        client = borrower = NULL;
        request.url = FG_STR("http://external.example/");
        CHECK(transport.get(transport.user, &request, &response, &error) == FG_INVALID);
        CHECK(transport.post(transport.user, &request, &response, &error) == FG_INVALID);
        request.url = FG_STR("https://example.com");
        request.headers = &bad;
        request.header_count = 1;
        CHECK(transport.get(transport.user, &request, &response, &error) == FG_INVALID);
        CHECK(transport.post(transport.user, &request, &response, &error) == FG_INVALID);
        request.headers = NULL;
        request.header_count = 0;
        request.body = FG_STR("GET body not allowed");
        CHECK(transport.get(transport.user, &request, &response, &error) == FG_INVALID);
        request.body = (fg_str){NULL, 0};
        CHECK(transport.get(NULL, &request, &response, &error) == FG_INVALID);
        fg_transport_default_free(&transport);
        fg_transport_default_free(&transport);
        CHECK(!transport.user && !transport.get && !transport.post);
    }
    OK(fg_client_create(NULL, NULL, &client, &error));
    fg_client_free(client);
    fg_response_free(&response);
    return 0;
}

static int test_shared_fixture(void)
{
    const fg_fixture item = {"https://fixture.example/value", "value", 200, "",
                             "User-Agent", "fixture-client"};
    fg_fixture_transport fixtures = {&item, 1, 0};
    fg_transport transport = {&fixtures, fg_fixture_get, NULL};
    fg_client_options options = {0};
    fg_client *client = NULL;
    fg_response response = {0};
    options.user_agent = FG_STR("fixture-client");
    OK(fg_client_create(&transport, &options, &client, NULL));
    OK(fg_get(client, FG_STR(item.url), NULL, 0, &response, NULL));
    CHECK(fixtures.next == 1 && response.status == 200 && !strcmp(response.body.data, "value"));
    fg_response_free(&response);
    fg_client_free(client);
    return 0;
}

static int test_loopback(const char *base)
{
    fg_transport transport = {0};
    fg_client_options options = {0};
    fg_client *client = NULL;
    fg_response response = {0};
    fg_buffer url = {0};
    fg_error error = {0};
    fg_header secret = {FG_STR("Authorization"), FG_STR("Bearer loopback-test-only")};
    fg_header cookie = {FG_STR("Cookie"), FG_STR("seed=retained")};
    const char binary[] = {'a', '\0', 'b'};
    size_t i;
    options.user_agent = FG_STR("offline-loopback-test");
    options.timeout_ms = 3000;
    OK(fg_transport_default(&transport, &error));
    OK(fg_client_create(&transport, &options, &client, &error));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/ok", 3, &error));
    OK(fg_get(client, (fg_str){url.data, url.len}, &secret, 1, &response, &error));
    CHECK(response.status == 200 && !strcmp(response.body.data, "success"));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/post", 5, &error));
    OK(fg_post(client, (fg_str){url.data, url.len}, NULL, 0,
               (fg_str){binary, sizeof(binary)}, &response, &error));
    CHECK(response.body.len == sizeof(binary) && !memcmp(response.body.data, binary, sizeof(binary)));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/seed", 5, &error));
    CHECK(fg_get(client, (fg_str){url.data, url.len}, NULL, 0, &response, &error) == FG_HTTP);
    CHECK(response.status == 404 && error.http_status == 404);
    CHECK(!strcmp(response.body.data, "cookie seed") && strstr(response.headers.data, "Set-Cookie: seed=retained"));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/cookie", 7, &error));
    OK(fg_get(client, (fg_str){url.data, url.len}, NULL, 0, &response, &error));
    CHECK(!strcmp(response.body.data, "no cookie"));
    OK(fg_get(client, (fg_str){url.data, url.len}, &cookie, 1, &response, &error));
    CHECK(!strcmp(response.body.data, "cookie accepted"));
    OK(fg_get(client, (fg_str){url.data, url.len}, NULL, 0, &response, &error));
    CHECK(!strcmp(response.body.data, "no cookie"));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/redirect", 9, &error));
    CHECK(fg_get(client, (fg_str){url.data, url.len}, &secret, 1, &response, &error) == FG_HTTP);
    CHECK(response.status == 302 && error.http_status == 302 && !strcmp(response.body.data, "not followed"));
    fg_client_free(client);
    client = NULL;
    options.max_response_bytes = 256;
    OK(fg_client_create(&transport, &options, &client, &error));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/large", 6, &error));
    CHECK(fg_get(client, (fg_str){url.data, url.len}, NULL, 0, &response, &error) == FG_LIMIT);
    CHECK(response.body.len <= 256 && error.code == FG_LIMIT);
    fg_client_free(client);
    client = NULL;
    options.timeout_ms = 100;
    OK(fg_client_create(&transport, &options, &client, &error));
    OK(fg_buffer_set(&url, FG_STR(base), &error));
    OK(fg_buffer_append(&url, "/slow", 5, &error));
    CHECK(fg_get(client, (fg_str){url.data, url.len}, NULL, 0, &response, &error) == FG_IO);
    CHECK(error.code == FG_IO && (strstr(error.message, "OS error") || strstr(error.message, "curl error")));
    for (i = 0; i < sizeof(error.message); ++i)
        if (!error.message[i]) break;
    CHECK(i < sizeof(error.message));
    fg_client_free(client);
    fg_transport_default_free(&transport);
    fg_response_free(&response);
    fg_buffer_free(&url);
    puts("native loopback tests passed");
    return 0;
}

int main(void)
{
    const char *loopback = getenv("FG_TEST_HTTP_BASE");
    CHECK(test_buffers() == 0);
    CHECK(test_requests() == 0);
    CHECK(test_sec() == 0);
    CHECK(test_files() == 0);
    CHECK(test_native_validation() == 0);
    CHECK(test_shared_fixture() == 0);
    if (loopback && loopback[0]) CHECK(test_loopback(loopback) == 0);
    puts("transport/core tests passed (offline)");
    return 0;
}

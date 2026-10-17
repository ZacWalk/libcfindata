#ifndef FG_TEST_H
#define FG_TEST_H
#include "fg.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)
#define OK(expression) CHECK((expression) == FG_OK)

typedef struct fg_fixture {
    const char *url;
    const char *body;
    int status;
    const char *headers;
    const char *required_header;
    const char *required_value;
} fg_fixture;
typedef struct fg_fixture_transport {
    const fg_fixture *items;
    size_t count;
    size_t next;
} fg_fixture_transport;

static fg_status fg_fixture_get(void *user, const fg_request *request,
                                fg_response *response, fg_error *error)
{
    fg_fixture_transport *fixture = (fg_fixture_transport *)user;
    const fg_fixture *item;
    size_t i;
    int found = 0;
    if (fixture->next >= fixture->count)
        return fg_fail(error, FG_IO, "Unexpected fixture request");
    item = &fixture->items[fixture->next++];
    if (!fg_str_equal(request->url, FG_STR(item->url)))
        return fg_fail(error, FG_INVALID, "Fixture URL mismatch");
    if (item->required_header) {
        for (i = 0; i < request->header_count; ++i) {
            if (fg_str_equal(request->headers[i].name, FG_STR(item->required_header)) &&
                fg_str_equal(request->headers[i].value, FG_STR(item->required_value)))
                found = 1;
        }
        if (!found) return fg_fail(error, FG_INVALID, "Fixture header mismatch");
    }
    response->status = item->status;
    if (fg_buffer_set(&response->headers, FG_STR(item->headers), error) != FG_OK)
        return error ? error->code : FG_NOMEM;
    return fg_buffer_set(&response->body, FG_STR(item->body), error);
}
#endif

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "test.h"
#include "fg_data.h"
#include <stdlib.h>

int fg_cli_run(int argc, char **argv, const fg_transport *transport);
int fg_cli_run_streams(int argc, char **argv, const fg_transport *transport,
                      FILE *output, FILE *errors);

#undef CHECK
#undef OK
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define OK(x) CHECK((x) == FG_OK)
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static const char *fixture_directory;
static const char *const env_keys[] = {
    "SEC_USER_AGENT", "FRED_API_KEY"
};
static const char *const env_values[] = {
    "cli-test-agent-process", "cli-test-fred-process"
};

/* Test process only: overwrite inherited values without reading, logging, or
   persisting user secrets. CTest may run this executable in parallel. */
static void synthetic_environment(int present)
{
    size_t i;
    for (i = 0; i < COUNT(env_keys); ++i) {
#ifdef _WIN32
        CHECK(_putenv_s(env_keys[i], present ? env_values[i] : "") == 0);
#else
        CHECK(setenv(env_keys[i], present ? env_values[i] : "", 1) == 0);
#endif
    }
}

static fg_str view(const fg_buffer *b) { return (fg_str){b->data, b->len}; }

static void fixture_path(char *out, size_t capacity, const char *name)
{
    int n = snprintf(out, capacity, "%s/%s", fixture_directory, name);
    CHECK(n > 0 && (size_t)n < capacity);
#ifdef _WIN32
    {
        size_t i;
        for (i = 0; out[i]; ++i) if (out[i] == '/') out[i] = '\\';
    }
#endif
}

static fg_buffer fixture(const char *name)
{
    char path[2048];
    fg_buffer b = {0};
    fixture_path(path, sizeof(path), name);
    OK(fg_read_file(FG_STR(path), 1024 * 1024, &b, NULL));
    return b;
}

static int contains(fg_str text, const char *needle)
{
    size_t i, n = strlen(needle);
    for (i = 0; i + n <= text.len; ++i)
        if (!memcmp(text.data + i, needle, n)) return 1;
    return 0;
}

static fg_str header(const fg_request *r, const char *name)
{
    size_t i;
    for (i = 0; i < r->header_count; ++i)
        if (fg_str_equal(r->headers[i].name, FG_STR(name))) return r->headers[i].value;
    return FG_STR("");
}

typedef struct step {
    const char *url, *query, *query2, *forbidden, *file, *body, *headers;
    const char *agent;
    int http;
    fg_status failure;
} step;

typedef struct replay_state {
    const step *steps;
    size_t count, next;
} replay_state;

static fg_status replay(void *user, const fg_request *request, fg_response *response, fg_error *e)
{
    replay_state *r = user;
    const step *s;
    fg_buffer text = {0}, url = {0};
    fg_fixture item;
    fg_fixture_transport f;
    fg_status status;
    CHECK(r->next < r->count);
    s = r->steps + r->next++;
    CHECK(contains(request->url, s->url));
    if (s->query) CHECK(contains(request->url, s->query));
    if (s->query2) CHECK(contains(request->url, s->query2));
    if (s->forbidden) CHECK(!contains(request->url, s->forbidden));
    CHECK(!header(request, "Authorization").len);
    if (s->agent) CHECK(fg_str_equal(header(request, "User-Agent"), FG_STR(s->agent)));
    CHECK(!header(request, "Cookie").len);
    if (s->file) text = fixture(s->file);
    else OK(fg_buffer_set(&text, FG_STR(s->body ? s->body : ""), e));
    OK(fg_buffer_set(&url, request->url, e));
    item = (fg_fixture){url.data, text.data, s->http ? s->http : 200, s->headers, NULL, NULL};
    f = (fg_fixture_transport){&item, 1, 0};
    status = fg_fixture_get(&f, request, response, e);
    fg_buffer_free(&text);
    fg_buffer_free(&url);
    if (s->failure)
        return fg_fail(e, s->failure, "cli-test-fred-process echoed by transport");
    return status;
}

typedef struct result {
    fg_buffer output, errors;
    int status;
    size_t requests;
} result;

static fg_buffer read_stream(FILE *stream)
{
    char block[4096];
    size_t n;
    fg_buffer out = {0};
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0, SEEK_SET) == 0);
    while ((n = fread(block, 1, sizeof(block), stream)) != 0)
        OK(fg_buffer_append(&out, block, n, NULL));
    CHECK(!ferror(stream));
    if (!out.data) OK(fg_buffer_set(&out, FG_STR(""), NULL));
    return out;
}

static result invoke(char **argv, size_t argc, const step *steps, size_t count, int write_failure)
{
    replay_state r = {steps, count, 0};
    fg_transport transport = {&r, replay, NULL};
    FILE *output = tmpfile(), *errors = tmpfile();
    result out = {0};
    CHECK(output && errors);
    if (write_failure) {
        char path[2048];
        CHECK(fclose(output) == 0);
        fixture_path(path, sizeof(path), "sec/tickers.json");
        output = fopen(path, "rb");
        CHECK(output != NULL);
        CHECK(setvbuf(output, NULL, _IONBF, 0) == 0);
    }
    out.status = fg_cli_run_streams((int)argc, argv, &transport, output, errors);
    out.requests = r.next;
    CHECK(r.next == count);
    if (!write_failure) out.output = read_stream(output);
    else CHECK(out.status == FG_IO);
    out.errors = read_stream(errors);
    CHECK(fclose(output) == 0);
    CHECK(fclose(errors) == 0);
    CHECK(!contains(view(&out.errors), "cli-test-"));
    CHECK(!contains(view(&out.output), "cli-test-fred"));
    if (out.status == FG_OK) CHECK(out.errors.len == 0);
    else CHECK(out.errors.len != 0);
    return out;
}

static void release(result *r)
{
    fg_buffer_free(&r->output);
    fg_buffer_free(&r->errors);
}

static fg_json *json(const result *r)
{
    fg_json *tree = NULL;
    CHECK(r->status == FG_OK);
    OK(fg_json_parse(view(&r->output), &tree, NULL));
    return tree;
}

static fg_json *line(const result *r, size_t index)
{
    size_t i, start = 0, n = 0;
    fg_json *tree = NULL;
    for (i = 0; i < r->output.len; ++i) {
        if (r->output.data[i] == '\n') {
            if (n++ == index) {
                OK(fg_json_parse((fg_str){r->output.data + start, i - start}, &tree, NULL));
                return tree;
            }
            start = i + 1;
        }
    }
    CHECK(0);
    return NULL;
}

static size_t lines(const result *r)
{
    size_t i, n = 0;
    for (i = 0; i < r->output.len; ++i) if (r->output.data[i] == '\n') ++n;
    return n;
}

static const fg_json *get(const fg_json *tree, const char *key)
{
    const fg_json *v = fg_json_get(tree, FG_STR(key));
    CHECK(v != NULL);
    return v;
}

static void exact(const fg_json *tree, const char *key, const char *value)
{
    CHECK(fg_str_equal(fg_json_number_text(get(tree, key)), FG_STR(value)));
}

static void check_bool(const fg_json *tree, const char *key, int expected)
{
    int value;
    OK(fg_json_bool(get(tree, key), &value, NULL));
    CHECK(value == expected);
}

static void no_requests(void)
{
    static char *const invalid[][26] = {
        {"cli", "ingest"}, {"cli", "analyze"}, {"cli", "score"}, {"cli", "llm"},
        {"cli", "--help", "--api-key", "cli-test-fred-flag"},
        {"cli", "sec", "unknown", "--help"},
        {"cli", "sec", "tickers", "--api-key", "cli-test-fred-flag"},
        {"cli", "sec", "map"},
        {"cli", "sec", "map", "--ticker"},
        {"cli", "sec", "map", "--ticker", "DEMO", "extra"},
        {"cli", "sec", "map", "--ticker", "DEMO", "--ticker", "OTHER"},
        {"cli", "sec", "filings", "--cik", "42"},
        {"cli", "sec", "filings", "--cik", "42", "--since", "2023-02-29"},
        {"cli", "sec", "filings", "--cik", "42", "--since", "2023-01-01", "--form", "10-Q", "--all-forms"},
        {"cli", "sec", "document", "--cik", "42", "--accession", "bad", "--document", "quarter.htm"},
        {"cli", "sec", "companyfacts", "--cik", "0"},
        {"cli", "sec", "companyfacts", "--cik", "4x"},
        {"cli", "sec", "companyfacts", "--cik", "12345678901"},
        {"cli", "sec", "tickers", "--env-dir", "relative"},
        {"cli", "sec", "tickers", "--env-dir", "C:relative"},
        {"cli", "fred", "observations", "--series", "X", "--start", "2023-01-01"},
        {"cli", "fred", "observations", "--series", "X", "--start", "2023-02-30", "--end", "2023-03-01"},
        {"cli", "fred", "observations", "--series", "X", "--start", "2023-03-01", "--end", "2023-01-01"},
        {"cli", "cftc"},
        {"cli", "cftc", "--url", "https://example.test/data", "--file", "unused"},
        {"cli", "finra", "--url", "https://example.test/data", "--delimiter", "pipe"},
        {"cli", "finra", "--url", "https://example.test/data", "--known-at", "1e3"},
        {"cli", "cftc", "--file", "unused", "--user-agent", "unused"},
        {"cli", "finra", "--url", "http://example.test/data"},
        {"cli", "convert"}, {"cli", "convert", "--file", "unused", "--env-dir", "C:\\"},
        {"cli", "xbrl", "--file", "unused", "--help"}
    };
    size_t i;
    for (i = 0; i < COUNT(invalid); ++i) {
        char *argv[26];
        size_t n = 0;
        result r;
        while (n < COUNT(argv) && invalid[i][n]) { argv[n] = invalid[i][n]; ++n; }
        r = invoke(argv, n, NULL, 0, 0);
        CHECK(r.status != FG_OK);
        CHECK(r.output.len == 0);
        release(&r);
    }
    {
        char *argv[] = {"cli", "sec", "tickers"};
        fg_transport t = {NULL, replay, NULL};
        /* Exercise the required stdout/stderr wrapper itself, not just streams. */
        char *bad[] = {"cli", "no-such-command"};
        CHECK(fg_cli_run(2, bad, &t) == FG_INVALID);
        synthetic_environment(0);
        {
            result r = invoke(argv, COUNT(argv), NULL, 0, 0);
            CHECK(r.status == FG_INVALID);
            release(&r);
        }
        argv[1] = "--help";
        {
            result r = invoke(argv, 2, NULL, 0, 0);
            CHECK(r.status == FG_OK);
            CHECK(contains(view(&r.output), "output_type=4"));
            CHECK(contains(view(&r.output), "--env-dir"));
            CHECK(contains(view(&r.output), "No inferred publication lag"));
            CHECK(contains(view(&r.output), "unrecognized options fail."));
            {
                result other = invoke(argv, 1, NULL, 0, 0);
                CHECK(other.status == FG_OK && fg_str_equal(view(&r.output), view(&other.output)));
                release(&other);
            }
            release(&r);
            r = invoke(argv, 1, NULL, 0, 0);
            CHECK(r.status == FG_OK);
            release(&r);
            r = invoke(argv, 2, NULL, 0, 1);
            release(&r);
        }
        synthetic_environment(1);
    }
    {
        char *fred[] = {"cli", "fred", "observations", "--series", "X", "--start",
            "2023-01-01", "--end", "2023-02-01"};
        char *group_help[] = {"cli", "fred", "--help"};
        char *command_help[] = {"cli", "fred", "observations", "--help"};
        result r;
        synthetic_environment(0);
        r = invoke(fred, COUNT(fred), NULL, 0, 0);
        CHECK(r.status == FG_INVALID && !r.output.len);
        release(&r);
        r = invoke(group_help, COUNT(group_help), NULL, 0, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        r = invoke(command_help, COUNT(command_help), NULL, 0, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        synthetic_environment(1);
    }
}

static void sec_tests(void)
{
    const step tickers = {.url="https://www.sec.gov/files/company_tickers.json",
        .file="sec/tickers.json", .agent="cli-test-agent-process"};
    char *argv[] = {"cli", "sec", "tickers"};
    result r = invoke(argv, COUNT(argv), &tickers, 1, 0);
    fg_json *tree = json(&r);
    CHECK(fg_json_size(tree) == 2);
    fg_json_free(tree);
    release(&r);
    {
        char *map[] = {"cli", "sec", "map", "--ticker", "demo/b"};
        r = invoke(map, COUNT(map), &tickers, 1, 0);
        tree = json(&r);
        exact(tree, "cik", "43");
        fg_json_free(tree);
        release(&r);
        map[4] = "missing";
        r = invoke(map, COUNT(map), &tickers, 1, 0);
        CHECK(r.status == FG_NOT_FOUND && !r.output.len);
        release(&r);
    }
    {
        char *facts[] = {"cli", "sec", "companyfacts", "--cik", "0000000042"};
        const step s = {.url="/api/xbrl/companyfacts/CIK0000000042.json", .file="sec/companyfacts.json"};
        r = invoke(facts, COUNT(facts), &s, 1, 0);
        tree = json(&r);
        CHECK(fg_json_type(get(tree, "facts")) == FG_JSON_OBJECT);
        CHECK(contains(view(&r.output), "10000000000000001"));
        fg_json_free(tree);
        release(&r);
    }
    {
        char *filings[] = {"cli", "sec", "filings", "--cik", "42", "--since", "2022-01-01",
            "--recent-only"};
        step s[] = {
            {.url="/submissions/CIK0000000042.json", .file="sec/submissions.json"},
            {.url="/submissions/CIK0000000042-submissions-001.json", .file="sec/older.json"}
        };
        r = invoke(filings, COUNT(filings) - 1, s, 2, 0);
        CHECK(r.status == FG_OK && lines(&r) == 2);
        tree = line(&r, 0);
        exact(tree, "cik", "42");
        CHECK(fg_str_equal(fg_json_string(get(tree, "form")), FG_STR("10-Q")));
        fg_json_free(tree);
        release(&r);
        r = invoke(filings, COUNT(filings), s, 1, 0);
        CHECK(r.status == FG_OK && lines(&r) == 1);
        release(&r);
        r = invoke(filings, COUNT(filings) - 1, s, 1, 1);
        release(&r);
        s[1].http = 500;
        r = invoke(filings, COUNT(filings) - 1, s, 2, 0);
        CHECK(r.status == FG_HTTP && lines(&r) == 1);
        release(&r);
        filings[7] = "--all-forms";
        s[1].http = 200;
        r = invoke(filings, COUNT(filings), s, 2, 0);
        CHECK(r.status == FG_OK && lines(&r) >= 2);
        release(&r);
        {
            char *one_form[] = {"cli", "sec", "filings", "--cik", "42", "--since",
                "2022-01-01", "--form", "10-K"};
            r = invoke(one_form, COUNT(one_form), s, 2, 0);
            CHECK(r.status == FG_OK && lines(&r) == 1);
            tree = line(&r, 0);
            CHECK(fg_str_equal(fg_json_string(get(tree, "form")), FG_STR("10-K")));
            fg_json_free(tree);
            release(&r);
        }
    }
    {
        char *doc[] = {"cli", "sec", "document", "--cik", "42", "--accession",
            "0000000042-24-000001", "--document", "quarter.htm"};
        const step s = {.url="/Archives/edgar/data/42/000000004224000001/quarter.htm", .file="sec/filing.htm"};
        r = invoke(doc, COUNT(doc), &s, 1, 0);
        tree = json(&r);
        CHECK(contains(fg_json_string(get(tree, "html")), "ix:"));
        fg_json_free(tree);
        release(&r);
        doc[2] = "markdown";
        r = invoke(doc, COUNT(doc), &s, 1, 0);
        CHECK(r.status == FG_OK && contains(view(&r.output), "## PART I"));
        release(&r);
        doc[2] = "xbrl";
        r = invoke(doc, COUNT(doc), &s, 1, 0);
        tree = json(&r);
        CHECK(fg_json_size(tree) == 5);
        exact(fg_json_at(tree, 0), "value", "84000e6");
        fg_json_free(tree);
        release(&r);
    }
    {
        step s = tickers;
        s.http = 429;
        r = invoke(argv, COUNT(argv), &s, 1, 0);
        CHECK(r.status == FG_HTTP && !r.output.len);
        release(&r);
        s.http = 200;
        s.failure = FG_IO;
        r = invoke(argv, COUNT(argv), &s, 1, 0);
        CHECK(r.status == FG_IO && !r.output.len);
        release(&r);
        s.failure = FG_OK;
        s.file = NULL;
        s.body = "{";
        r = invoke(argv, COUNT(argv), &s, 1, 0);
        CHECK(r.status == FG_PARSE && !r.output.len);
        release(&r);
    }
}

static void fred_tests(void)
{
    char *argv[] = {"cli", "fred", "observations", "--series", "JTSJOL", "--start",
        "2023-01-01", "--end", "2023-03-01", "--latest"};
    step first[] = {
        {.url="/fred/series/observations?", .query="output_type=4", .query2="api_key=cli-test-fred-process",
         .forbidden="output_type=1", .file="sources/fred_first_1.json"},
        {.url="realtime_start=1776-07-04&realtime_end=9999-12-31", .query="offset=2",
         .query2="limit=100000", .file="sources/fred_first_2.json"}
    };
    step latest = {.url="output_type=1", .forbidden="output_type=4", .file="sources/fred_latest.json"};
    result r = invoke(argv, COUNT(argv) - 1, first, 2, 0);
    fg_json *tree;
    CHECK(r.status == FG_OK && lines(&r) == 3);
    tree = line(&r, 0);
    exact(tree, "value", "10824");
    exact(tree, "knowledge_time", "1678233600");
    check_bool(tree, "is_first_release", 1);
    fg_json_free(tree);
    tree = line(&r, 1);
    CHECK(fg_json_type(get(tree, "value")) == FG_JSON_NULL);
    check_bool(tree, "has_value", 0);
    fg_json_free(tree);
    release(&r);
    r = invoke(argv, COUNT(argv), &latest, 1, 0);
    CHECK(r.status == FG_OK && lines(&r) == 1);
    tree = line(&r, 0);
    exact(tree, "value", "10318");
    CHECK(fg_json_type(get(tree, "knowledge_time")) == FG_JSON_NULL);
    check_bool(tree, "is_first_release", 0);
    fg_json_free(tree);
    release(&r);
    r = invoke(argv, COUNT(argv) - 1, first, 1, 1);
    release(&r);
    first[0].http = 403;
    r = invoke(argv, COUNT(argv) - 1, first, 1, 0);
    CHECK(r.status == FG_HTTP && !r.output.len);
    release(&r);
    first[0].http = 200;
    first[1].http = 500;
    r = invoke(argv, COUNT(argv) - 1, first, 2, 0);
    CHECK(r.status == FG_HTTP && lines(&r) == 2);
    release(&r);
    first[0].file = NULL;
    first[0].body = "{}";
    r = invoke(argv, COUNT(argv) - 1, first, 1, 0);
    CHECK(r.status == FG_PARSE && !r.output.len);
    release(&r);
    first[0].body = "{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":["
        "{\"date\":\"2023-01-01\",\"realtime_start\":\"2023-03-08\","
        "\"realtime_end\":\"9999-12-31\",\"value\":\"10000000000000001.123456789\"}]}";
    r = invoke(argv, COUNT(argv) - 1, first, 1, 0);
    CHECK(r.status == FG_OK);
    tree = line(&r, 0);
    exact(tree, "value", "10000000000000001.123456789");
    fg_json_free(tree);
    release(&r);
}

static void positioning_tests(void)
{
    char path[2048];
    char *cftc[] = {"cli", "cftc", "--url", "https://example.test/cftc.csv",
        "--known-date-column", "Published", "--market-contains", "NASDAQ"};
    step cot = {.url="https://example.test/cftc.csv", .file="sources/cftc.csv"};
    result r = invoke(cftc, COUNT(cftc), &cot, 1, 0);
    fg_json *tree;
    CHECK(r.status == FG_OK && lines(&r) == 2);
    tree = line(&r, 0);
    exact(tree, "open_interest", "9007199254740993");
    exact(tree, "knowledge_time", "1672963200");
    CHECK(contains(fg_json_string(get(tree, "market")), "\"FIXTURE\"\r\n"));
    fg_json_free(tree);
    tree = line(&r, 1);
    CHECK(fg_json_type(get(tree, "knowledge_time")) == FG_JSON_NULL);
    CHECK(fg_json_type(get(tree, "dealer_long")) == FG_JSON_NULL);
    fg_json_free(tree);
    release(&r);
    r = invoke(cftc, COUNT(cftc), &cot, 1, 1);
    release(&r);
    fixture_path(path, sizeof(path), "sources/cftc.csv");
    cftc[2] = "--file";
    cftc[3] = path;
    r = invoke(cftc, COUNT(cftc), NULL, 0, 0);
    CHECK(r.status == FG_OK && lines(&r) == 2);
    release(&r);
    {
        char *known[] = {"cli", "cftc", "--file", path, "--known-date-column", "Published",
            "--known-at", "1678838400"};
        r = invoke(known, COUNT(known), NULL, 0, 0);
        CHECK(r.status == FG_OK && lines(&r) == 3);
        tree = line(&r, 2);
        exact(tree, "knowledge_time", "1678838400");
        fg_json_free(tree);
        release(&r);
    }
    {
        char *finra[] = {"cli", "finra", "--url", "https://example.test/finra.csv",
            "--known-date-column", "Published"};
        step margin = {.url="https://example.test/finra.csv", .file="sources/finra.csv"};
        r = invoke(finra, COUNT(finra), &margin, 1, 0);
        CHECK(r.status == FG_OK && lines(&r) == 3);
        tree = line(&r, 0);
        exact(tree, "debit_balances", "641236.5");
        CHECK(fg_str_equal(fg_json_string(get(tree, "debit_text")), FG_STR("641,236.50")));
        fg_json_free(tree);
        tree = line(&r, 1);
        CHECK(fg_json_type(get(tree, "debit_balances")) == FG_JSON_NULL);
        check_bool(tree, "has_debit_balances", 0);
        fg_json_free(tree);
        release(&r);
        r = invoke(finra, COUNT(finra), &margin, 1, 1);
        release(&r);
        margin.http = 503;
        r = invoke(finra, COUNT(finra), &margin, 1, 0);
        CHECK(r.status == FG_HTTP);
        release(&r);
        margin.http = 200;
        margin.file = NULL;
        margin.body = "<html>not CSV</html>";
        r = invoke(finra, COUNT(finra), &margin, 1, 0);
        CHECK(r.status == FG_PARSE);
        release(&r);
        fixture_path(path, sizeof(path), "sources/finra.csv");
        finra[2] = "--file";
        finra[3] = path;
        r = invoke(finra, COUNT(finra), NULL, 0, 0);
        CHECK(r.status == FG_OK && lines(&r) == 3);
        release(&r);
    }
    {
        char *tsv[] = {"cli", "finra", "--file", path, "--delimiter", "tsv"};
        fixture_path(path, sizeof(path), "sources/finra.tsv");
        r = invoke(tsv, COUNT(tsv), NULL, 0, 0);
        CHECK(r.status == FG_OK && lines(&r) > 0);
        release(&r);
        tsv[2] = "--url";
        tsv[3] = "https://example.test/finra.tsv";
        {
            step s = {.url="https://example.test/finra.tsv", .file="sources/finra.tsv"};
            r = invoke(tsv, COUNT(tsv), &s, 1, 0);
            CHECK(r.status == FG_OK);
            release(&r);
        }
    }
}

static void local_and_environment_tests(void)
{
    char path[2048], directory[2048];
    char *local[] = {"cli", "convert", "--file", path};
    result r;
    fg_json *tree;
    synthetic_environment(0);
    fixture_path(path, sizeof(path), "sec/filing.htm");
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status == FG_OK && contains(view(&r.output), "## PART I"));
    release(&r);
    r = invoke(local, COUNT(local), NULL, 0, 1);
    release(&r);
    local[1] = "xbrl";
    r = invoke(local, COUNT(local), NULL, 0, 0);
    tree = json(&r);
    CHECK(fg_json_size(tree) == 5);
    fg_json_free(tree);
    release(&r);
    r = invoke(local, COUNT(local), NULL, 0, 1);
    release(&r);
    fixture_path(path, sizeof(path), "cli/bad-xbrl.htm");
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status == FG_PARSE && !r.output.len);
    release(&r);
    fixture_path(path, sizeof(path), "cli/no-such-file.htm");
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status != FG_OK && !r.output.len);
    release(&r);
    local[1] = "convert";
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status != FG_OK && !r.output.len);
    release(&r);
    fixture_path(path, sizeof(path), "cli/bad-xbrl.htm");
    local[1] = "cftc";
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status == FG_PARSE && !r.output.len);
    release(&r);
    local[1] = "finra";
    r = invoke(local, COUNT(local), NULL, 0, 0);
    CHECK(r.status == FG_PARSE && !r.output.len);
    release(&r);
    {
        char *fred[] = {"cli", "fred", "observations", "--series", "JTSJOL", "--start",
            "2023-01-01", "--end", "2023-03-01", "--env-dir", directory, "--api-key",
            "cli-test-fred-flag", "--latest"};
        step s = {.url="output_type=1", .query="api_key=cli-test-fred-flag", .file="sources/fred_latest.json"};
        fixture_path(directory, sizeof(directory), "cli/env");
        r = invoke(fred, COUNT(fred), &s, 1, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        fred[11] = "--latest";
        s.query = "api_key=cli-test-fred-file";
        r = invoke(fred, 12, &s, 1, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        synthetic_environment(1);
        s.query = "api_key=cli-test-fred-process";
        r = invoke(fred, 12, &s, 1, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        fixture_path(directory, sizeof(directory), "cli/invalid-env");
        r = invoke(fred, 12, NULL, 0, 0);
        CHECK(r.status == FG_PARSE && !r.output.len);
        release(&r);
    }
    {
        char *sec[] = {"cli", "sec", "tickers", "--env-dir", directory, "--user-agent",
            "cli-test-agent-flag"};
        step s = {.url="/files/company_tickers.json", .file="sec/tickers.json",
            .agent="cli-test-agent-flag"};
        fixture_path(directory, sizeof(directory), "cli/env");
        r = invoke(sec, COUNT(sec), &s, 1, 0);
        CHECK(r.status == FG_OK);
        release(&r);
        synthetic_environment(0);
        s.agent = "cli-test-agent-file";
        r = invoke(sec, COUNT(sec) - 2, &s, 1, 0);
        CHECK(r.status == FG_OK);
        release(&r);
    }
    synthetic_environment(1);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    fixture_directory = argv[1]; /* Absolute tests/fixtures directory. */
    synthetic_environment(1);
    no_requests();
    sec_tests();
    fred_tests();
    positioning_tests();
    local_and_environment_tests();
    printf("cli: %u checks passed (fixture transport only)\n", checks);
    return 0;
}

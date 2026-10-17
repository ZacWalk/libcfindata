#include "fg_sec_internal.h"
#include <stdio.h>
#include <string.h>

static const fg_json *get(const fg_json *v, const char *key)
{
    return fg_json_get(v, FG_STR(key));
}

fg_status fg_sec_set_value(fg_json *obj, const char *key, fg_json *value, fg_error *e)
{
    fg_status s = fg_json_set(obj, FG_STR(key), value, e);
    if (s != FG_OK) fg_json_free(value);
    return s;
}

fg_status fg_sec_set_string(fg_json *obj, const char *key, fg_str text, fg_error *e)
{
    fg_json *v = NULL;
    fg_status s = fg_json_new_string(text, &v, e);
    return s == FG_OK ? fg_sec_set_value(obj, key, v, e) : s;
}

fg_status fg_sec_set_i64(fg_json *obj, const char *key, int64_t value, fg_error *e)
{
    fg_json *v = NULL;
    fg_status s = fg_json_new_i64(value, &v, e);
    return s == FG_OK ? fg_sec_set_value(obj, key, v, e) : s;
}

static int digit(char c) { return c >= '0' && c <= '9'; }
static int cik_valid(uint64_t cik) { return cik && cik <= UINT64_C(9999999999); }

fg_status fg_sec_date(fg_str d, int64_t *out, fg_error *e)
{
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y, m, day, leap, adjusted, era, yoe, doy, doe;
    size_t i;
    if (!d.data || d.len != 10 || d.data[4] != '-' || d.data[7] != '-')
        return fg_fail(e, FG_PARSE, "SEC date must be YYYY-MM-DD");
    for (i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && !digit(d.data[i]))
            return fg_fail(e, FG_PARSE, "Invalid SEC date digit");
    y = (d.data[0]-'0')*1000 + (d.data[1]-'0')*100 + (d.data[2]-'0')*10 + d.data[3]-'0';
    m = (d.data[5]-'0')*10 + d.data[6]-'0';
    day = (d.data[8]-'0')*10 + d.data[9]-'0';
    leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if (y < 1 || m < 1 || m > 12 || day < 1 ||
        day > lengths[m-1] + (m == 2 && leap))
        return fg_fail(e, FG_PARSE, "Invalid SEC calendar date");
    adjusted = y - (m <= 2);
    era = adjusted / 400;
    yoe = adjusted - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    *out = ((int64_t)era * 146097 + doe - 719468) * 86400;
    return FG_OK;
}

static fg_status required_string(const fg_json *v, const char *key, int nonempty,
                                fg_str *out, fg_error *e)
{
    const fg_json *n = get(v, key);
    if (fg_json_type(n) != FG_JSON_STRING || (nonempty && !fg_json_string(n).len))
        return fg_fail(e, FG_PARSE, "SEC payload is missing a required string");
    *out = fg_json_string(n);
    return FG_OK;
}

static fg_status optional_string(const fg_json *v, const char *key, fg_str *out, fg_error *e)
{
    const fg_json *n = get(v, key);
    if (n && fg_json_type(n) != FG_JSON_STRING)
        return fg_fail(e, FG_PARSE, "SEC optional field is not a string");
    *out = fg_json_string(n);
    return FG_OK;
}

static fg_status cik_value(const fg_json *v, uint64_t *out, fg_error *e)
{
    uint64_t n = 0;
    fg_status s;
    if (fg_json_type(v) == FG_JSON_STRING) {
        fg_str text = fg_json_string(v);
        size_t i;
        if (!text.len || text.len > 10)
            return fg_fail(e, FG_PARSE, "Invalid SEC CIK string");
        for (i = 0; i < text.len; ++i) {
            if (!digit(text.data[i])) return fg_fail(e, FG_PARSE, "Invalid SEC CIK digit");
            n = n * 10 + (unsigned)(text.data[i] - '0');
        }
    } else {
        s = fg_json_u64(v, &n, e);
        if (s != FG_OK) return fg_fail(e, FG_PARSE, "Invalid SEC CIK number");
    }
    if (!cik_valid(n)) return fg_fail(e, FG_PARSE, "SEC CIK out of range");
    *out = n;
    return FG_OK;
}

static fg_status fetch_json(fg_client *c, const char *url, fg_json **out, fg_error *e)
{
    fg_response r = {0};
    fg_status s;
    if (!out) return fg_fail(e, FG_INVALID, "Missing SEC JSON output");
    *out = NULL;
    s = fg_sec_get(c, FG_STR(url), &r, e);
    if (s == FG_OK) s = fg_json_parse((fg_str){r.body.data, r.body.len}, out, e);
    fg_response_free(&r);
    return s;
}

static fg_status ticker_shape(const fg_json *v, fg_error *e)
{
    size_t i;
    if (fg_json_type(v) != FG_JSON_OBJECT)
        return fg_fail(e, FG_PARSE, "SEC tickers must be an object");
    for (i = 0; i < fg_json_size(v); ++i) {
        const fg_json *row = fg_json_get(v, fg_json_key(v, i));
        uint64_t cik;
        fg_str text;
        if (cik_value(get(row, "cik_str"), &cik, e) != FG_OK ||
            required_string(row, "ticker", 1, &text, e) != FG_OK ||
            required_string(row, "title", 0, &text, e) != FG_OK) return FG_PARSE;
    }
    return FG_OK;
}

fg_status fg_sec_tickers(fg_client *c, fg_json **out, fg_error *e)
{
    fg_status s = fetch_json(c, "https://www.sec.gov/files/company_tickers.json", out, e);
    if (s == FG_OK && (s = ticker_shape(*out, e)) != FG_OK) {
        fg_json_free(*out); *out = NULL;
    }
    return s;
}

static int ticker_equal(fg_str a, fg_str b, int aliases)
{
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; ++i) {
        fg_str x = {a.data+i, 1}, y = {b.data+i, 1};
        if (fg_sec_iequal(x, y)) continue;
        if (aliases && (a.data[i] == '.' || a.data[i] == '/' || a.data[i] == '-') &&
            (b.data[i] == '.' || b.data[i] == '/' || b.data[i] == '-')) continue;
        return 0;
    }
    return 1;
}

fg_status fg_sec_ticker_cik(const fg_json *v, fg_str ticker, uint64_t *out, fg_error *e)
{
    size_t i;
    int pass;
    fg_status s;
    if (!out || !ticker.data || !ticker.len)
        return fg_fail(e, FG_INVALID, "Missing SEC ticker or CIK output");
    if ((s = ticker_shape(v, e)) != FG_OK) return s;
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < fg_json_size(v); ++i) {
            const fg_json *r = fg_json_get(v, fg_json_key(v, i));
            if (ticker_equal(ticker, fg_json_string(get(r, "ticker")), pass))
                return cik_value(get(r, "cik_str"), out, e);
        }
    return fg_fail(e, FG_NOT_FOUND, "SEC ticker not found");
}

static fg_status recent_shape(const fg_json *v, fg_error *e)
{
    static const char *keys[] = {"accessionNumber", "form", "filingDate", "primaryDocument"};
    size_t i, n = fg_json_size(get(v, keys[0]));
    if (fg_json_type(v) != FG_JSON_OBJECT) return fg_fail(e, FG_PARSE, "Missing SEC recent filings");
    for (i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
        if (fg_json_type(get(v, keys[i])) != FG_JSON_ARRAY || fg_json_size(get(v, keys[i])) != n)
            return fg_fail(e, FG_PARSE, "SEC filing arrays have inconsistent lengths");
    return FG_OK;
}

fg_status fg_sec_submissions(fg_client *c, uint64_t cik, fg_json **out, fg_error *e)
{
    char url[100];
    fg_status s;
    const fg_json *files;
    if (!out) return fg_fail(e, FG_INVALID, "Missing submissions output");
    *out = NULL;
    if (!cik_valid(cik)) return fg_fail(e, FG_INVALID, "Invalid CIK");
    snprintf(url, sizeof(url), "https://data.sec.gov/submissions/CIK%010llu.json",
             (unsigned long long)cik);
    s = fetch_json(c, url, out, e);
    if (s != FG_OK) return s;
    s = recent_shape(get(get(*out, "filings"), "recent"), e);
    if (s == FG_OK && get(*out, "cik")) {
        uint64_t actual = 0;
        s = cik_value(get(*out, "cik"), &actual, e);
        if (s == FG_OK && actual != cik)
            s = fg_fail(e, FG_PARSE, "SEC submissions CIK mismatch");
    }
    files = get(get(*out, "filings"), "files");
    if (s == FG_OK && files && fg_json_type(files) != FG_JSON_ARRAY)
        s = fg_fail(e, FG_PARSE, "SEC submissions files must be an array");
    if (s != FG_OK) { fg_json_free(*out); *out = NULL; }
    return s;
}

static int accession_valid(fg_str s)
{
    size_t i;
    if (!s.data || s.len != 20) return 0;
    for (i = 0; i < s.len; ++i)
        if ((i == 10 || i == 13) ? s.data[i] != '-' : !digit(s.data[i])) return 0;
    return 1;
}

static int filename_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           digit((char)c) || c == '.' || c == '_' || c == '-';
}

static int filename_valid(fg_str s)
{
    size_t i;
    if (!s.data || !s.len || s.len > 255 || s.data[0] == '.') return 0;
    for (i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (!filename_char(c)) return 0;
        if (c == '.' && i && s.data[i-1] == '.') return 0;
    }
    return 1;
}

static int document_valid(fg_str s)
{
    size_t i, start = 0;
    if (!s.data || !s.len || s.len > 255) return 0;
    for (i = 0; i <= s.len; ++i) {
        if (i == s.len || s.data[i] == '/') {
            size_t n = i - start;
            if (!n || (s.data[start] == '.' &&
                (n == 1 || (n == 2 && s.data[start+1] == '.')))) return 0;
            start = i + 1;
        } else if (!filename_char((unsigned char)s.data[i])) return 0;
    }
    return 1;
}

fg_status fg_sec_filing_fetch(fg_client *c, uint64_t cik, fg_str acc, fg_str doc,
                            fg_buffer *out, fg_error *e)
{
    fg_response r = {0};
    char url[400], compact[19];
    size_t i, j = 0;
    fg_status s;
    if (!out || !cik_valid(cik) || !accession_valid(acc) || !document_valid(doc))
        return fg_fail(e, FG_INVALID, "Invalid SEC archive CIK, accession or document path");
    for (i = 0; i < acc.len; ++i) if (acc.data[i] != '-') compact[j++] = acc.data[i];
    compact[j] = '\0';
    snprintf(url, sizeof(url), "https://www.sec.gov/Archives/edgar/data/%llu/%s/%.*s",
             (unsigned long long)cik, compact, (int)doc.len, doc.data);
    s = fg_sec_get(c, FG_STR(url), &r, e);
    if (s == FG_OK) {
        fg_buffer_free(out); *out = r.body; memset(&r.body, 0, sizeof(r.body));
    }
    fg_response_free(&r);
    return s;
}

typedef struct filings_state {
    uint64_t cik;
    int64_t since;
    const fg_sec_filings_options *options;
    fg_sec_filing_callback callback;
    void *user;
    fg_json *seen;
} filings_state;

static fg_status filing_page(filings_state *state, const fg_json *recent, fg_error *e)
{
    size_t i, j, n;
    fg_status s = recent_shape(recent, e);
    if (s != FG_OK) return s;
    n = fg_json_size(get(recent, "accessionNumber"));
    for (i = 0; i < n; ++i) {
        fg_sec_filing f = {0};
        int wanted = 0;
        int64_t filed;
        fg_json *mark = NULL;
        const char *keys[] = {"accessionNumber", "form", "filingDate", "primaryDocument",
                              "reportDate", "acceptanceDateTime"};
        fg_str *fields[] = {&f.accession, &f.form, &f.filing_date, &f.primary_document,
                            &f.report_date, &f.acceptance_datetime};
        f.cik = state->cik;
        for (j = 0; j < 3; ++j) {
            const fg_json *array = get(recent, keys[j]), *v;
            if (fg_json_type(array) != FG_JSON_ARRAY || fg_json_size(array) != n ||
                fg_json_type(v = fg_json_at(array, i)) != FG_JSON_STRING)
                return fg_fail(e, FG_PARSE, "Invalid SEC filing array entry");
            *fields[j] = fg_json_string(v);
        }
        if (!f.form.len)
            return fg_fail(e, FG_PARSE, "Malformed SEC filing record");
        if ((s = fg_sec_date(f.filing_date, &filed, e)) != FG_OK) return s;
        if (filed < state->since) continue;
        if (!state->options || !state->options->forms)
            wanted = fg_str_equal(f.form, FG_STR("10-K")) || fg_str_equal(f.form, FG_STR("10-Q"));
        else if (!state->options->form_count) wanted = 1;
        else for (j = 0; j < state->options->form_count; ++j)
            if (fg_str_equal(f.form, state->options->forms[j])) wanted = 1;
        if (!wanted) continue;
        for (j = 3; j < 6; ++j) {
            const fg_json *array = get(recent, keys[j]), *v;
            if (!array && j >= 4) continue;
            if (fg_json_type(array) != FG_JSON_ARRAY || fg_json_size(array) != n ||
                fg_json_type(v = fg_json_at(array, i)) != FG_JSON_STRING)
                return fg_fail(e, FG_PARSE, "Invalid SEC filing array entry");
            *fields[j] = fg_json_string(v);
        }
        if (!accession_valid(f.accession) ||
            (f.primary_document.len && !document_valid(f.primary_document)))
            return fg_fail(e, FG_PARSE, "Malformed SEC filing record");
        if (!f.primary_document.len || fg_json_get(state->seen, f.accession)) continue;
        s = fg_json_new_bool(1, &mark, e);
        if (s == FG_OK) s = fg_json_set(state->seen, f.accession, mark, e);
        if (s != FG_OK) { fg_json_free(mark); return s; }
        if ((s = state->callback(state->user, &f, e)) != FG_OK) return s;
    }
    return FG_OK;
}

fg_status fg_sec_filings(fg_client *c, uint64_t cik, const fg_sec_filings_options *options,
                        fg_sec_filing_callback callback, void *user, fg_error *e)
{
    fg_json *root = NULL, *page = NULL, *seen_files = NULL;
    const fg_json *files;
    filings_state st = {0};
    fg_status s;
    size_t i;
    char prefix[32];
    fg_str since = options && options->since.len ? options->since : FG_STR("2022-01-01");
    if (!callback || (options && !options->forms && options->form_count))
        return fg_fail(e, FG_INVALID, "Invalid SEC filing callback/options");
    st.cik = cik; st.options = options; st.callback = callback; st.user = user;
    if (fg_sec_date(since, &st.since, e) != FG_OK)
        return fg_fail(e, FG_INVALID, "Invalid SEC since date");
    if ((s = fg_sec_submissions(c, cik, &root, e)) != FG_OK) return s;
    if ((s = fg_json_new(FG_JSON_OBJECT, &st.seen, e)) != FG_OK) goto done;
    s = filing_page(&st, get(get(root, "filings"), "recent"), e);
    if (s != FG_OK || (options && options->recent_only)) goto done;
    files = get(get(root, "filings"), "files");
    if ((s = fg_json_new(FG_JSON_OBJECT, &seen_files, e)) != FG_OK) goto done;
    snprintf(prefix, sizeof(prefix), "CIK%010llu-submissions-", (unsigned long long)cik);
    for (i = 0; i < fg_json_size(files); ++i) {
        const fg_json *file = fg_json_at(files, i);
        fg_str name, end;
        int64_t to;
        char url[340];
        fg_json *mark = NULL;
        if ((s = required_string(file, "name", 1, &name, e)) != FG_OK) goto done;
        if (!filename_valid(name) || name.len <= strlen(prefix)+5 ||
            memcmp(name.data, prefix, strlen(prefix)) ||
            memcmp(name.data+name.len-5, ".json", 5)) {
            s = fg_fail(e, FG_PARSE, "Invalid SEC older submissions filename"); goto done;
        }
        if ((s = optional_string(file, "filingTo", &end, e)) != FG_OK) goto done;
        if (end.len) {
            if ((s = fg_sec_date(end, &to, e)) != FG_OK) goto done;
            if (to < st.since) continue;
        }
        if (fg_json_get(seen_files, name)) continue;
        if ((s = fg_json_new_bool(1, &mark, e)) != FG_OK) goto done;
        s = fg_json_set(seen_files, name, mark, e);
        if (s != FG_OK) { fg_json_free(mark); goto done; }
        snprintf(url, sizeof(url), "https://data.sec.gov/submissions/%.*s", (int)name.len, name.data);
        if ((s = fetch_json(c, url, &page, e)) != FG_OK) goto done;
        s = filing_page(&st, page, e);
        fg_json_free(page); page = NULL;
        if (s != FG_OK) goto done;
    }
done:
    fg_json_free(root); fg_json_free(page); fg_json_free(st.seen); fg_json_free(seen_files);
    return s;
}

static fg_status company_shape(const fg_json *root, uint64_t *cik, fg_error *e)
{
    fg_str name;
    fg_status s = cik_value(get(root, "cik"), cik, e);
    if (s == FG_OK) s = required_string(root, "entityName", 0, &name, e);
    if (s == FG_OK && fg_json_type(get(root, "facts")) != FG_JSON_OBJECT)
        s = fg_fail(e, FG_PARSE, "SEC companyfacts facts must be an object");
    return s;
}

fg_status fg_sec_companyfacts(fg_client *c, uint64_t cik, fg_json **out, fg_error *e)
{
    char url[120];
    uint64_t actual;
    fg_status s;
    if (!out) return fg_fail(e, FG_INVALID, "Missing companyfacts output");
    *out = NULL;
    if (!cik_valid(cik)) return fg_fail(e, FG_INVALID, "Invalid CIK");
    snprintf(url, sizeof(url), "https://data.sec.gov/api/xbrl/companyfacts/CIK%010llu.json",
             (unsigned long long)cik);
    if ((s = fetch_json(c, url, out, e)) != FG_OK) return s;
    s = company_shape(*out, &actual, e);
    if (s == FG_OK && actual != cik) s = fg_fail(e, FG_PARSE, "SEC companyfacts CIK mismatch");
    if (s != FG_OK) { fg_json_free(*out); *out = NULL; }
    return s;
}

fg_status fg_sec_companyfacts_extract(const fg_json *root, fg_sec_companyfact_callback callback,
                                     void *user, fg_error *e)
{
    const fg_json *facts = get(root, "facts");
    fg_sec_companyfact f = {0};
    size_t ti, ni, ui, ri;
    fg_status s;
    if (!callback) return fg_fail(e, FG_INVALID, "Missing companyfact callback");
    if ((s = company_shape(root, &f.cik, e)) != FG_OK) return s;
    f.entity_name = fg_json_string(get(root, "entityName"));
    for (ti = 0; ti < fg_json_size(facts); ++ti) {
        const fg_json *tags;
        f.taxonomy = fg_json_key(facts, ti);
        tags = fg_json_get(facts, f.taxonomy);
        if (fg_json_type(tags) != FG_JSON_OBJECT)
            return fg_fail(e, FG_PARSE, "SEC taxonomy must be an object");
        for (ni = 0; ni < fg_json_size(tags); ++ni) {
            const fg_json *payload, *units;
            f.name = fg_json_key(tags, ni); payload = fg_json_get(tags, f.name);
            if ((s = optional_string(payload, "label", &f.label, e)) != FG_OK ||
                (s = optional_string(payload, "description", &f.description, e)) != FG_OK) return s;
            units = get(payload, "units");
            if (fg_json_type(units) != FG_JSON_OBJECT)
                return fg_fail(e, FG_PARSE, "SEC fact units must be an object");
            for (ui = 0; ui < fg_json_size(units); ++ui) {
                const fg_json *entries;
                f.unit = fg_json_key(units, ui); entries = fg_json_get(units, f.unit);
                if (fg_json_type(entries) != FG_JSON_ARRAY)
                    return fg_fail(e, FG_PARSE, "SEC unit entries must be an array");
                for (ri = 0; ri < fg_json_size(entries); ++ri) {
                    const fg_json *r = fg_json_at(entries, ri);
                    f.value = get(r, "val"); f.fiscal_year = get(r, "fy");
                    if ((fg_json_type(f.value) != FG_JSON_NUMBER && fg_json_type(f.value) != FG_JSON_STRING) ||
                        (f.fiscal_year && fg_json_type(f.fiscal_year) != FG_JSON_NUMBER))
                        return fg_fail(e, FG_PARSE, "Invalid SEC companyfact value/year");
                    if ((s = required_string(r, "end", 1, &f.end, e)) != FG_OK ||
                        (s = required_string(r, "filed", 1, &f.filed, e)) != FG_OK ||
                        (s = optional_string(r, "start", &f.start, e)) != FG_OK ||
                        (s = optional_string(r, "accn", &f.accession, e)) != FG_OK ||
                        (s = optional_string(r, "form", &f.form, e)) != FG_OK ||
                        (s = optional_string(r, "fp", &f.fiscal_period, e)) != FG_OK ||
                        (s = optional_string(r, "frame", &f.frame, e)) != FG_OK) return s;
                    f.has_start = f.start.len != 0; f.period_start = 0;
                    if ((s = fg_sec_date(f.end, &f.period_end, e)) != FG_OK ||
                        (s = fg_sec_date(f.filed, &f.knowledge_time, e)) != FG_OK ||
                        (f.has_start && (s = fg_sec_date(f.start, &f.period_start, e)) != FG_OK)) return s;
                    if (f.has_start && f.period_start > f.period_end)
                        return fg_fail(e, FG_PARSE, "SEC fact period starts after end");
                    if ((s = callback(user, &f, e)) != FG_OK) return s;
                }
            }
        }
    }
    return FG_OK;
}

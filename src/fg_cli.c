#include "fg.h"
#include "fg_data.h"
#include "fg_sec.h"
#include "fg_sources.h"
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kept here until the embedding application needs a public CLI header. */
int fg_cli_run(int argc, char **argv, const fg_transport *transport);
int fg_cli_run_streams(int argc, char **argv, const fg_transport *transport,
                      FILE *output, FILE *errors);

static const char *const help[] = {
    "libcfindata - C11 acquisition examples (no analysis or storage)\n"
    "Usage: cfindata COMMAND [ACTION] OPTIONS\n"
    "  cfindata --help | help\n"
    "  cfindata COMMAND [ACTION] --help\n"
    "\n"
    "SEC (requires --user-agent TEXT or SEC_USER_AGENT):\n"
    "  sec tickers                         JSON ticker map\n"
    "  sec map --ticker SYMBOL             JSON {ticker,cik}\n"
    "  sec companyfacts --cik CIK           Full provider JSON, all vintages\n"
    "  sec filings --cik CIK --since YYYY-MM-DD\n"
    "      [--form FORM | --all-forms] [--recent-only]     JSONL\n"
    "      Default forms: 10-K and 10-Q; follows older pages by default.\n"
    "  sec document --cik CIK --accession NNNNNNNNNN-NN-NNNNNN --document NAME\n"
    "      JSON {cik,accession,document,html}; downloads the actual document.\n"
    "  sec markdown --cik CIK --accession ACCESSION --document NAME\n"
    "      Downloads and converts to Markdown on stdout.\n"
    "  sec xbrl --cik CIK --accession ACCESSION --document NAME\n"
    "      Downloads and extracts inline XBRL as a JSON array.\n"
    "\n"
    "FRED (requires --api-key KEY or FRED_API_KEY):\n"
    "  fred observations --series ID --start YYYY-MM-DD --end YYYY-MM-DD\n"
    "      [--latest]                      JSONL\n"
    "      Default: first release (ALFRED output_type=4), NEVER falls back.\n"
    "      --latest explicitly selects revised data, not release-time history.\n",
    "\n"
    "Positioning (explicit uncompressed public HTTPS CSV/TSV or local file):\n"
    "  cftc (--url HTTPS-URL | --file PATH) [--delimiter csv|tsv]\n"
    "      [--known-date-column NAME] [--known-at SECONDS] [--market-contains TEXT]\n"
    "  finra (--url HTTPS-URL | --file PATH) [--delimiter csv|tsv]\n"
    "      [--known-date-column NAME] [--known-at SECONDS]\n"
    "      JSONL; CSV default. CFTC uses TFF Futures Only named columns;\n"
    "      FINRA uses Month,Debit balances as its first two columns.\n"
    "      No inferred publication lag; missing values/knowledge are null.\n",
    "\n"
    "Local (no client, network, or credentials):\n"
    "  convert --file PATH                 HTML to Markdown on stdout\n"
    "  xbrl --file PATH                    Inline XBRL JSON array\n"
    "\n"
    "Network options: [--user-agent TEXT] [--env-dir ABSOLUTE-DIRECTORY]\n"
    "  --env-dir explicitly calls fg_env_load_upward (up to eight levels).\n"
    "  No implicit .env reads. Precedence: flag > process environment > .env.\n"
    "  --env-dir / --user-agent are not accepted in local file modes.\n"
    "Output: compact sorted-key JSON or one record per line (JSONL), except\n"
    "  explicit Markdown commands. Unix timestamps are integer UTC seconds.\n"
    "  Integer quantities and decimal lexemes stay exact. FINRA debit_balances\n"
    "  are approximate; FINRA also retains debit_text.\n"
    "  Streamed records already written are not rolled back on later errors.\n"
    "  Local input limit: 256 MiB. Redirect stdout to save output.\n"
    "Exit: 0 success; otherwise fg_status (1 invalid, 2 memory, 3 parse, 4 I/O,\n"
    "  5 HTTP, 6 limit, 7 not found, 8 checksum). Errors go to stderr and\n"
    "  omit provider/transport details to avoid disclosing credentials.\n"
    "All options require separate values; duplicates, extra arguments and\n"
    "  unrecognized options fail. Help must be used without other options.\n"
};

typedef enum cli_option {
    USER_AGENT, ENV_DIR, API_KEY, CIK, TICKER, SINCE,
    FORM, ALL_FORMS, RECENT_ONLY, ACCESSION, DOCUMENT, SERIES, START, END,
    LATEST, KNOWN_AT, URL, FILE_PATH, DELIMITER, KNOWN_COLUMN, MARKET_CONTAINS,
    OPTION_COUNT
} cli_option;

#define BIT(o) (UINT64_C(1) << (o))
#define NETWORK (BIT(USER_AGENT) | BIT(ENV_DIR))
#define DOCUMENT_OPTIONS (BIT(CIK) | BIT(ACCESSION) | BIT(DOCUMENT))
#define WINDOW (BIT(START) | BIT(END))
#define POSITIONING (BIT(URL) | BIT(FILE_PATH) | BIT(DELIMITER) | BIT(KNOWN_COLUMN) | BIT(KNOWN_AT))

static const char *const option_names[OPTION_COUNT] = {
    "--user-agent", "--env-dir", "--api-key",
    "--cik", "--ticker", "--since", "--form", "--all-forms", "--recent-only",
    "--accession", "--document", "--series", "--start", "--end", "--latest",
    "--known-at", "--url", "--file", "--delimiter",
    "--known-date-column", "--market-contains"
};

typedef enum cli_command {
    SEC_TICKERS, SEC_MAP, SEC_FACTS, SEC_FILINGS, SEC_DOCUMENT, SEC_MARKDOWN,
    SEC_XBRL, FRED, CFTC, FINRA, CONVERT, XBRL
} cli_command;

typedef struct command_spec {
    const char *command, *action;
    cli_command id;
    uint64_t required, optional;
} command_spec;

static const command_spec commands[] = {
    {"sec", "tickers", SEC_TICKERS, 0, NETWORK},
    {"sec", "map", SEC_MAP, BIT(TICKER), NETWORK},
    {"sec", "companyfacts", SEC_FACTS, BIT(CIK), NETWORK},
    {"sec", "filings", SEC_FILINGS, BIT(CIK) | BIT(SINCE),
        NETWORK | BIT(FORM) | BIT(ALL_FORMS) | BIT(RECENT_ONLY)},
    {"sec", "document", SEC_DOCUMENT, DOCUMENT_OPTIONS, NETWORK},
    {"sec", "markdown", SEC_MARKDOWN, DOCUMENT_OPTIONS, NETWORK},
    {"sec", "xbrl", SEC_XBRL, DOCUMENT_OPTIONS, NETWORK},
    {"fred", "observations", FRED, BIT(SERIES) | WINDOW, NETWORK | BIT(API_KEY) | BIT(LATEST)},
    {"cftc", NULL, CFTC, 0, NETWORK | POSITIONING | BIT(MARKET_CONTAINS)},
    {"finra", NULL, FINRA, 0, NETWORK | POSITIONING},
    {"convert", NULL, CONVERT, BIT(FILE_PATH), 0},
    {"xbrl", NULL, XBRL, BIT(FILE_PATH), 0}
};

typedef struct cli_args {
    const command_spec *spec;
    const char *values[OPTION_COUNT];
    uint64_t seen;
    int help, local;
    uint64_t cik;
    int64_t known_at;
} cli_args;

static fg_str arg(const cli_args *a, cli_option key)
{
    return FG_STR(a->values[key]);
}

static int has(const cli_args *a, cli_option key)
{
    return (a->seen & BIT(key)) != 0;
}

static int absolute_directory(const char *s)
{
    size_t n = strlen(s);
    if (s[0] == '/') return 1;
    if (n >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':' && (s[2] == '\\' || s[2] == '/')) return 1;
    return n >= 3 && s[0] == '\\' && s[1] == '\\' && s[2] != '\\';
}

static fg_status integer_arg(fg_str text, int64_t *out, fg_error *e)
{
    fg_json *number = NULL;
    size_t i = text.len && text.data[0] == '-' ? 1 : 0;
    fg_status s;
    if (i == text.len) return fg_fail(e, FG_INVALID, "Expected integer seconds");
    for (; i < text.len; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
            return fg_fail(e, FG_INVALID, "Expected integer seconds");
    s = fg_json_new_number(text, &number, e);
    if (s == FG_OK) s = fg_json_i64(number, out, e);
    fg_json_free(number);
    return s == FG_OK ? FG_OK : fg_fail(e, FG_INVALID, "Invalid integer seconds");
}

static fg_status parse_args(int argc, char **argv, cli_args *a, fg_error *e)
{
    size_t j;
    int i, first = 2;
    if (argc < 1 || !argv) return fg_fail(e, FG_INVALID, "Invalid arguments");
    for (i = 0; i < argc; ++i)
        if (!argv[i]) return fg_fail(e, FG_INVALID, "Invalid arguments");
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "help")))) {
        a->help = 1;
        return FG_OK;
    }
    for (j = 0; j < sizeof(commands) / sizeof(*commands); ++j) {
        if (strcmp(argv[1], commands[j].command)) continue;
        if (argc == 3 && !strcmp(argv[2], "--help")) {
            a->help = 1;
            return FG_OK;
        }
        if (commands[j].action) {
            if (argc < 3 || strcmp(argv[2], commands[j].action)) continue;
            first = 3;
        }
        a->spec = commands + j;
        break;
    }
    if (!a->spec) return fg_fail(e, FG_INVALID, "Unknown command");
    if (argc == first + 1 && !strcmp(argv[first], "--help")) {
        a->help = 1;
        return FG_OK;
    }
    for (i = first; i < argc; ++i) {
        for (j = 0; j < OPTION_COUNT; ++j)
            if (!strcmp(argv[i], option_names[j])) break;
        if (j == OPTION_COUNT || (a->seen & BIT(j)) ||
            !((a->spec->required | a->spec->optional) & BIT(j)))
            return fg_fail(e, FG_INVALID, "Unknown, duplicate, or inapplicable option");
        a->seen |= BIT(j);
        if (j == ALL_FORMS || j == RECENT_ONLY || j == LATEST) continue;
        if (++i == argc || !argv[i][0] || !strncmp(argv[i], "--", 2))
            return fg_fail(e, FG_INVALID, "Missing option value");
        a->values[j] = argv[i];
    }
    if ((a->seen & a->spec->required) != a->spec->required ||
        (has(a, FORM) && has(a, ALL_FORMS)))
        return fg_fail(e, FG_INVALID, "Missing or conflicting options");
    a->local = a->spec->id == CONVERT || a->spec->id == XBRL ||
        ((a->spec->id == CFTC || a->spec->id == FINRA) && has(a, FILE_PATH));
    if (a->local && (a->seen & NETWORK))
        return fg_fail(e, FG_INVALID, "Network options in local mode");
    if (has(a, ENV_DIR) && !absolute_directory(a->values[ENV_DIR]))
        return fg_fail(e, FG_INVALID, "Expected absolute environment directory");
    if (has(a, CIK)) {
        fg_str text = arg(a, CIK);
        if (!text.len || text.len > 10) return fg_fail(e, FG_INVALID, "Invalid CIK");
        for (j = 0; j < text.len; ++j) {
            if (text.data[j] < '0' || text.data[j] > '9')
                return fg_fail(e, FG_INVALID, "Invalid CIK");
            a->cik = a->cik * 10 + (unsigned)(text.data[j] - '0');
        }
        if (!a->cik) return fg_fail(e, FG_INVALID, "Invalid CIK");
    }
    if (has(a, KNOWN_AT) && integer_arg(arg(a, KNOWN_AT), &a->known_at, e) != FG_OK)
        return FG_INVALID;
    if (a->spec->id == CFTC || a->spec->id == FINRA) {
        if (has(a, URL) == has(a, FILE_PATH))
            return fg_fail(e, FG_INVALID, "Choose one URL or file");
        if (has(a, DELIMITER) && strcmp(a->values[DELIMITER], "csv") &&
            strcmp(a->values[DELIMITER], "tsv"))
            return fg_fail(e, FG_INVALID, "Invalid delimiter");
    }
    return FG_OK;
}

static fg_status credential(const cli_args *a, cli_option key, const char *name,
                            const fg_env *env, fg_str *out, fg_error *e)
{
    const char *process;
    fg_status s;
    if (has(a, key)) { *out = arg(a, key); return FG_OK; }
    process = getenv(name);
    if (process && *process) { *out = FG_STR(process); return FG_OK; }
    if (env) {
        s = fg_env_get(env, FG_STR(name), out, e);
        if (s != FG_NOT_FOUND) return s;
    }
    *out = FG_STR("");
    return FG_OK;
}

static fg_status write_text(FILE *out, fg_str text, int newline, fg_error *e)
{
    if ((text.len && fwrite(text.data, 1, text.len, out) != text.len) ||
        (newline && fputc('\n', out) == EOF) || fflush(out) == EOF || ferror(out))
        return fg_fail(e, FG_IO, "Output write failed");
    return FG_OK;
}

static fg_status write_json(FILE *out, const fg_json *tree, fg_error *e)
{
    fg_buffer text = {0};
    fg_status s = fg_json_serialize(tree, &text, e);
    if (s == FG_OK) s = write_text(out, (fg_str){text.data, text.len}, 1, e);
    fg_buffer_free(&text);
    return s;
}

typedef struct record_writer {
    fg_json *object;
    fg_status status;
    fg_error *error;
} record_writer;

static record_writer new_record(fg_error *e)
{
    record_writer w = {NULL, FG_OK, e};
    w.status = fg_json_new(FG_JSON_OBJECT, &w.object, e);
    return w;
}

/* Constructors/setters transfer ownership only on success. Preserve the first
   failure and free unattached nodes as well as the partial record. */
static void member(record_writer *w, const char *key, fg_json *value, fg_status s)
{
    if (w->status == FG_OK) w->status = s;
    if (w->status == FG_OK)
        w->status = fg_json_set(w->object, FG_STR(key), value, w->error);
    if (w->status != FG_OK) fg_json_free(value);
}

#define FIELD_FUNCTION(name, type, constructor) \
    static void name(record_writer *w, const char *key, type value) \
    { \
        fg_json *node = NULL; \
        fg_status s; \
        if (w->status != FG_OK) return; \
        s = constructor(value, &node, w->error); \
        member(w, key, node, s); \
    }
FIELD_FUNCTION(string_field, fg_str, fg_json_new_string)
FIELD_FUNCTION(decimal_field, fg_str, fg_json_new_number)
FIELD_FUNCTION(i64_field, int64_t, fg_json_new_i64)
FIELD_FUNCTION(u64_field, uint64_t, fg_json_new_u64)
FIELD_FUNCTION(bool_field, int, fg_json_new_bool)
#undef FIELD_FUNCTION

static void null_field(record_writer *w, const char *key)
{
    fg_json *node = NULL;
    fg_status s;
    if (w->status != FG_OK) return;
    s = fg_json_new(FG_JSON_NULL, &node, w->error);
    member(w, key, node, s);
}

static void optional_time(record_writer *w, const char *key, int present, int64_t value)
{
    if (present) i64_field(w, key, value);
    else null_field(w, key);
}

static void double_field(record_writer *w, const char *key, double value)
{
    char number[128], *point;
    const char *decimal;
    int n;
    if (w->status != FG_OK) return;
    if (isnan(value)) { null_field(w, key); return; }
    if (!isfinite(value)) {
        w->status = fg_fail(w->error, FG_LIMIT, "Nonfinite output number");
        return;
    }
    n = snprintf(number, sizeof(number), "%.*g", DBL_DECIMAL_DIG, value);
    if (n < 0 || (size_t)n >= sizeof(number)) {
        w->status = fg_fail(w->error, FG_LIMIT, "Output number too long");
        return;
    }
    /* snprintf follows the caller's locale. Normalize its decimal separator
       without changing global LC_NUMERIC or approximating integer fields. */
    decimal = localeconv()->decimal_point;
    if (decimal && *decimal && strcmp(decimal, ".") && (point = strstr(number, decimal))) {
        size_t len = strlen(decimal);
        *point = '.';
        memmove(point + 1, point + len, strlen(point + len) + 1);
    }
    decimal_field(w, key, FG_STR(number));
}

static fg_status finish_record(record_writer *w, FILE *out)
{
    fg_status s = w->status;
    if (s == FG_OK) s = write_json(out, w->object, w->error);
    fg_json_free(w->object);
    return s;
}

static fg_status filing_record(void *user, const fg_sec_filing *r, fg_error *e)
{
    record_writer w = new_record(e);
    u64_field(&w, "cik", r->cik);
    string_field(&w, "accession", r->accession);
    string_field(&w, "form", r->form);
    string_field(&w, "filing_date", r->filing_date);
    string_field(&w, "primary_document", r->primary_document);
    string_field(&w, "report_date", r->report_date);
    string_field(&w, "acceptance_datetime", r->acceptance_datetime);
    return finish_record(&w, user);
}

static fg_status fred_record(void *user, const fg_fred_observation *r, fg_error *e)
{
    record_writer w = new_record(e);
    string_field(&w, "series_id", r->series_id);
    string_field(&w, "value_text", r->value_text);
    if (r->has_value) decimal_field(&w, "value", r->value_text);
    else null_field(&w, "value");
    bool_field(&w, "has_value", r->has_value);
    i64_field(&w, "observation_date", r->observation_date);
    i64_field(&w, "realtime_start", r->realtime_start);
    i64_field(&w, "realtime_end", r->realtime_end);
    bool_field(&w, "is_first_release", r->is_first_release);
    optional_time(&w, "knowledge_time", r->has_knowledge_time, r->knowledge_time);
    return finish_record(&w, user);
}

static fg_status cftc_record(void *user, const fg_cftc_record *r, fg_error *e)
{
    static const char *const keys[] = {"dealer_long", "dealer_short", "asset_mgr_long",
        "asset_mgr_short", "lev_money_long", "lev_money_short"};
    uint64_t values[] = {r->dealer_long, r->dealer_short, r->asset_mgr_long,
        r->asset_mgr_short, r->lev_money_long, r->lev_money_short};
    record_writer w = new_record(e);
    size_t i;
    string_field(&w, "market", r->market);
    i64_field(&w, "report_date", r->report_date);
    optional_time(&w, "knowledge_time", r->has_knowledge_time, r->knowledge_time);
    u64_field(&w, "open_interest", r->open_interest);
    u64_field(&w, "present", r->present);
    for (i = 0; i < 6; ++i) {
        if (r->present & (1u << i)) u64_field(&w, keys[i], values[i]);
        else null_field(&w, keys[i]);
    }
    return finish_record(&w, user);
}

static fg_status finra_record(void *user, const fg_finra_record *r, fg_error *e)
{
    record_writer w = new_record(e);
    i64_field(&w, "month_date", r->month_date);
    optional_time(&w, "knowledge_time", r->has_knowledge_time, r->knowledge_time);
    bool_field(&w, "has_debit_balances", r->has_debit_balances);
    if (r->has_debit_balances) double_field(&w, "debit_balances", r->debit_balances);
    else null_field(&w, "debit_balances");
    string_field(&w, "debit_text", r->debit_text);
    return finish_record(&w, user);
}

static fg_status execute(const cli_args *a, fg_client *client, fg_str fred_key,
                         FILE *out, fg_error *e)
{
    fg_json *tree = NULL;
    fg_buffer input = {0}, markdown = {0};
    fg_status s = FG_OK;
    cli_command command = a->spec->id;
#define TRY(x) do { s = (x); if (s != FG_OK) goto done; } while (0)
    switch (command) {
    case SEC_TICKERS:
    case SEC_MAP:
        TRY(fg_sec_tickers(client, &tree, e));
        if (command == SEC_MAP) {
            uint64_t cik;
            record_writer w;
            TRY(fg_sec_ticker_cik(tree, arg(a, TICKER), &cik, e));
            w = new_record(e);
            string_field(&w, "ticker", arg(a, TICKER));
            u64_field(&w, "cik", cik);
            TRY(finish_record(&w, out));
            goto done;
        }
        break;
    case SEC_FACTS:
        TRY(fg_sec_companyfacts(client, a->cik, &tree, e));
        break;
    case SEC_FILINGS: {
        fg_str form = arg(a, FORM);
        fg_sec_filings_options o = {0};
        o.since = arg(a, SINCE);
        o.recent_only = has(a, RECENT_ONLY);
        if (has(a, FORM) || has(a, ALL_FORMS)) o.forms = &form;
        o.form_count = has(a, FORM) ? 1 : 0;
        TRY(fg_sec_filings(client, a->cik, &o, filing_record, out, e));
        goto done;
    }
    case SEC_DOCUMENT:
    case SEC_MARKDOWN:
    case SEC_XBRL:
    case CONVERT:
    case XBRL: {
        fg_str text;
        if (a->local) TRY(fg_read_file(arg(a, FILE_PATH), (size_t)256 * 1024 * 1024, &input, e));
        else TRY(fg_sec_filing_fetch(client, a->cik, arg(a, ACCESSION), arg(a, DOCUMENT), &input, e));
        text = (fg_str){input.data, input.len};
        if (command == SEC_DOCUMENT) {
            record_writer w = new_record(e);
            u64_field(&w, "cik", a->cik);
            string_field(&w, "accession", arg(a, ACCESSION));
            string_field(&w, "document", arg(a, DOCUMENT));
            string_field(&w, "html", text);
            TRY(finish_record(&w, out));
            goto done;
        }
        if (command == CONVERT || command == SEC_MARKDOWN) {
            TRY(fg_html_to_markdown(text, &markdown, e));
            TRY(write_text(out, (fg_str){markdown.data, markdown.len}, 0, e));
            goto done;
        }
        TRY(fg_xbrl_extract(text, &tree, e));
        break;
    }
    case FRED: {
        fg_fred_options o = {0};
        o.series_id = arg(a, SERIES);
        o.api_key = fred_key;
        o.observation_start = arg(a, START);
        o.observation_end = arg(a, END);
        o.vintage = has(a, LATEST) ? FG_FRED_LATEST : FG_FRED_FIRST_RELEASE;
        TRY(fg_fred_observations(client, &o, fred_record, out, e));
        goto done;
    }
    case CFTC:
    case FINRA: {
        fg_positioning_options o = {0};
        o.delimiter = has(a, DELIMITER) && !strcmp(a->values[DELIMITER], "tsv") ? '\t' : ',';
        o.known_date_column = arg(a, KNOWN_COLUMN);
        o.has_known_at = has(a, KNOWN_AT);
        o.known_at = a->known_at;
        o.market_contains = arg(a, MARKET_CONTAINS);
        if (a->local) {
            fg_str text;
            TRY(fg_read_file(arg(a, FILE_PATH), (size_t)256 * 1024 * 1024, &input, e));
            text = (fg_str){input.data, input.len};
            if (command == CFTC) TRY(fg_cftc_parse(text, &o, cftc_record, out, e));
            else TRY(fg_finra_parse(text, &o, finra_record, out, e));
        } else if (command == CFTC) TRY(fg_cftc_fetch(client, arg(a, URL), &o, cftc_record, out, e));
        else TRY(fg_finra_fetch(client, arg(a, URL), &o, finra_record, out, e));
        goto done;
    }
    }
    TRY(write_json(out, tree, e));
done:
    fg_json_free(tree);
    fg_buffer_free(&input);
    fg_buffer_free(&markdown);
    return s;
#undef TRY
}

int fg_cli_run_streams(int argc, char **argv, const fg_transport *transport,
                      FILE *output, FILE *errors)
{
    cli_args a = {0};
    fg_client_options options = {0};
    fg_str fred_key = {0};
    fg_client *client = NULL;
    fg_env *env = NULL;
    fg_error e = {0};
    fg_status s;
    if (!output || !errors) return FG_INVALID;
    s = parse_args(argc, argv, &a, &e);
    if (s != FG_OK) goto done;
    if (a.help) {
        size_t i;
        for (i = 0; i < sizeof(help)/sizeof(help[0]); ++i) {
            s = write_text(output, FG_STR(help[i]), 0, &e);
            if (s != FG_OK) goto done;
        }
        goto done;
    }
#define TRY(x) do { s = (x); if (s != FG_OK) goto done; } while (0)
    if (!a.local) {
        if (has(&a, ENV_DIR)) TRY(fg_env_load_upward(arg(&a, ENV_DIR), 8, &env, &e));
        options.user_agent = arg(&a, USER_AGENT);
        if (a.spec->id <= SEC_XBRL) {
            TRY(credential(&a, USER_AGENT, "SEC_USER_AGENT", env, &options.user_agent, &e));
            if (!options.user_agent.len) {
                s = fg_fail(&e, FG_INVALID, "SEC requires User-Agent");
                goto done;
            }
        }
        if (a.spec->id == FRED) {
            TRY(credential(&a, API_KEY, "FRED_API_KEY", env, &fred_key, &e));
            if (!fred_key.len) { s = fg_fail(&e, FG_INVALID, "FRED requires API key"); goto done; }
        }
        TRY(fg_client_create(transport, &options, &client, &e));
    }
    TRY(execute(&a, client, fred_key, output, &e));
    if (fflush(output) == EOF || ferror(output)) s = fg_fail(&e, FG_IO, "Output write failed");
done:
    fg_client_free(client);
    fg_env_free(env);
    /* Even injected transports may echo credentials in errors. Never print
       their messages, request URLs, response bodies, or argument values. */
    if (s != FG_OK) {
        if (fprintf(errors, "libcfindata: %s; see --help for syntax and requirements\n",
                    fg_status_name(s)) < 0 || fflush(errors) == EOF || ferror(errors))
            return FG_IO;
    }
    return (int)s;
#undef TRY
}

int fg_cli_run(int argc, char **argv, const fg_transport *transport)
{
    return fg_cli_run_streams(argc, argv, transport, stdout, stderr);
}

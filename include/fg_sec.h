#ifndef FG_SEC_H
#define FG_SEC_H

#include "fg_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SEC network calls always use fg_sec_get (configured User-Agent and throttle).
   CIKs are positive, at most ten decimal digits. JSON outputs own their entire
   tree; *out is NULL on failure. Free them with fg_json_free. */
fg_status fg_sec_tickers(fg_client *client, fg_json **out, fg_error *error);
/* Case insensitive; exact match precedes share-class . / - aliases.
   The supplied tree is the object returned by fg_sec_tickers. */
fg_status fg_sec_ticker_cik(const fg_json *tickers, fg_str ticker,
                          uint64_t *out, fg_error *error);
fg_status fg_sec_submissions(fg_client *client, uint64_t cik,
                           fg_json **out, fg_error *error);

typedef struct fg_sec_filing {
    uint64_t cik;
    fg_str accession, form, filing_date, primary_document;
    fg_str report_date, acceptance_datetime; /* Optional; empty when absent. */
} fg_sec_filing;
typedef fg_status (*fg_sec_filing_callback)(void *user,
    const fg_sec_filing *filing, fg_error *error);

typedef struct fg_sec_filings_options {
    fg_str since; /* Inclusive YYYY-MM-DD; empty defaults to 2022-01-01. */
    const fg_str *forms; /* NULL: 10-K, 10-Q; non-NULL + zero count: all forms. */
    size_t form_count;
    int recent_only; /* Zero follows older submissions files overlapping since. */
} fg_sec_filings_options;
/* Records borrow bytes only until the callback returns. Failures stop
   immediately, including HTTP/parse/callback failures in older pages; earlier
   callbacks are not rolled back. Accession duplicates across pages are skipped.
   Form/since exclusions precede download-path validation. Included nonempty
   primary_document paths follow fg_sec_filing_fetch's rules; empty ones skip. */
fg_status fg_sec_filings(fg_client *client, uint64_t cik,
    const fg_sec_filings_options *options, fg_sec_filing_callback callback,
    void *user, fg_error *error);
/* Builds only https://www.sec.gov/Archives/edgar/data/<cik>/<accession>/<doc>.
   Accession must be NNNNNNNNNN-NN-NNNNNN; document is a relative archive path
   of at most 255 bytes, with slash-separated ASCII letter/digit/_/./- components.
   No empty, "." or ".." components, leading slash, backslash, URL, percent
   escape, query or fragment is allowed. Paths stay within that accession.
   Older submissions JSON filenames remain single basenames, not paths.
   out is initialized to {0}; replaces its contents only on success. */
fg_status fg_sec_filing_fetch(fg_client *client, uint64_t cik, fg_str accession,
                            fg_str document, fg_buffer *out, fg_error *error);

fg_status fg_sec_companyfacts(fg_client *client, uint64_t cik,
                            fg_json **out, fg_error *error);
typedef struct fg_sec_companyfact {
    uint64_t cik;
    fg_str entity_name, taxonomy, name, label, description, unit;
    fg_str start, end, filed, accession, form, fiscal_period, frame;
    const fg_json *value; /* Borrowed number or string. number_text is exact. */
    const fg_json *fiscal_year; /* Optional JSON number; no lossy conversion. */
    int64_t period_start, period_end, knowledge_time; /* UTC Unix seconds. */
    int has_start; /* Instant records have no start; period_start is zero. */
} fg_sec_companyfact;
typedef fg_status (*fg_sec_companyfact_callback)(void *user,
    const fg_sec_companyfact *fact, fg_error *error);
/* Pure extraction, all taxonomies/tags/units/vintages retained. No metric
   catalog, cadence filtering or storage. knowledge_time is ALWAYS filed, not
   period end. Invalid dates/shapes fail rather than silently discarding facts.
   Callback views live as long as the input tree; the record itself is borrowed.
   Earlier callbacks are not rolled back on a later failure. */
fg_status fg_sec_companyfacts_extract(const fg_json *tree,
    fg_sec_companyfact_callback callback, void *user, fg_error *error);

/* Five-pass legacy behavior: metadata/hidden filtering, structural Markdown
   and padded tables, Unicode entities, SEC heading/cover/footer detection
   (a cover is dropped only when a PART or table-of-contents line ends it),
   whitespace normalization. Inline ix values stay visible, ix metadata does
   not. Links retain text, images are omitted, nested tables flatten in cells.
   Comments and colspan are read as browsers read them: "<!-->" and "<!--->"
   are empty comments and "--!>" also ends a comment; an invalid or zero
   colspan is 1 and a larger one is clamped to 1000; a row may still span at
   most 1024 columns (FG_LIMIT).
   Buffer replacement is transactional. Input must be UTF-8 without NUL. */
fg_status fg_html_to_markdown(fg_str html, fg_buffer *out, fg_error *error);
/* Pure inline-XBRL extraction, distinct from companyfacts. Owned JSON array:
   name, context, unit, decimals, scale, sign, format, text, numeric, nil, value,
   plus start/end/instant date strings and period_start/period_end when present.
   Numbers use exact decimal lexemes and exponent scaling (never double).
   decimals is precision metadata, not rounding; integer scale, precision and
   combined exponent magnitudes are limited to 100000. Unsupported transforms and
   missing/dash/nil numeric text yield value:null. Malformed numbers in supported
   transforms fail. Missing context definitions retain the context id alone.
   Continuation chains and ix:exclude are supported; cycles/missing links fail.
   No filing date is inferred: join with submissions for filing knowledge time. */
fg_status fg_xbrl_extract(fg_str html, fg_json **out, fg_error *error);

#ifdef __cplusplus
}
#endif
#endif

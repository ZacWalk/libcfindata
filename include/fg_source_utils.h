#ifndef FG_SOURCE_UTILS_H
#define FG_SOURCE_UTILS_H
#include "fg_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Provider-neutral helpers for application-owned sources. JSON/string/buffer
   views are borrowed. Initialize owned buffers and token lists to zero.
   String inputs must be valid fg_str views; no helper reads credentials. */
const fg_json *fg_s_get(const fg_json *, const char *);
fg_str fg_s_view(const fg_buffer *);
/* Reject control bytes, embedded NUL and missing data; optionally require text. */
int fg_s_valid(fg_str, int required);
int fg_s_equal(fg_str, const char *);
int fg_s_iequal(fg_str, fg_str);
fg_str fg_s_trim(fg_str);
/* Strict Gregorian YYYY-MM-DD and RFC3339, including nanoseconds/UTC offsets.
   format_time writes to a buffer of at least 21 bytes, years 0001..9999. */
fg_status fg_s_date(fg_str, int64_t *, fg_error *);
fg_status fg_s_timestamp(fg_str, int64_t *, uint32_t *, fg_error *);
fg_status fg_s_format_time(int64_t, char *, int date_only, fg_error *);
/* Optional missing/null strings become an empty view; populated values and
   finite numbers remain validated. Decimal input uses the shared JSON parser. */
fg_status fg_s_string(const fg_json *, const char *, int optional, fg_str *, fg_error *);
fg_status fg_s_double(const fg_json *, double *, fg_error *);
fg_status fg_s_decimal(fg_str, double *, fg_error *);
fg_status fg_s_encode(fg_buffer *, fg_str, fg_error *);
/* Append &name=encoded-value to an existing query; name is a trusted literal. */
fg_status fg_s_query(fg_buffer *, const char *, fg_str, fg_error *);
fg_status fg_s_uquery(fg_buffer *, const char *, uint64_t, fg_error *);
/* Owned JSON on success, NULL on failure. Transport/parser errors are redacted;
   the supplied client's response limits and redirect/cookie policy apply. */
fg_status fg_s_fetch(fg_client *, fg_str, const fg_header *, size_t, fg_json **, fg_error *);
fg_status fg_s_redact(fg_error *, fg_status, const char *);
/* Required next_page_token: string or null. Reject cycles, tokens over 64 KiB,
   and more than 5000 tokens. The returned view lives until tokens_free. */
typedef struct fg_s_tokens { fg_buffer *items; size_t count; } fg_s_tokens;
void fg_s_tokens_free(fg_s_tokens *);
fg_status fg_s_next(const fg_json *, fg_s_tokens *, fg_str *, fg_error *);

#ifdef __cplusplus
}
#endif
#endif

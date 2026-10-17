#ifndef FG_DATA_H
#define FG_DATA_H

#include "fg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FG_JSON_MAX_DEPTH 128

typedef struct fg_json fg_json;
typedef enum fg_json_kind {
    FG_JSON_NULL, FG_JSON_BOOL, FG_JSON_NUMBER, FG_JSON_STRING,
    FG_JSON_ARRAY, FG_JSON_OBJECT
} fg_json_kind;

/* Parse copies all data. *out is NULL on failure; free only the root.
   Duplicate keys, invalid UTF-8, and more than MAX_DEPTH node levels fail. */
fg_status fg_json_parse(fg_str text, fg_json **out, fg_error *error);
void fg_json_free(fg_json *value);
fg_json_kind fg_json_type(const fg_json *value); /* NULL pointer: FG_JSON_NULL. */
/* Lookups borrow from the tree; NULL means missing/wrong container type. */
const fg_json *fg_json_get(const fg_json *object, fg_str key);
const fg_json *fg_json_at(const fg_json *array, size_t index);
size_t fg_json_size(const fg_json *value); /* Array/object count, otherwise zero. */
fg_str fg_json_key(const fg_json *object, size_t index); /* Sorted UTF-8 byte order. */
fg_str fg_json_string(const fg_json *value); /* Wrong type: {NULL, 0}. */
fg_str fg_json_number_text(const fg_json *value); /* Original, exact lexeme. */
fg_status fg_json_bool(const fg_json *value, int *out, fg_error *error);
/* Integer conversions are exact, including integral decimal/exponent forms.
   Fractions return FG_INVALID; overflow returns FG_LIMIT; no output on error. */
fg_status fg_json_i64(const fg_json *value, int64_t *out, fg_error *error);
fg_status fg_json_u64(const fg_json *value, uint64_t *out, fg_error *error);
/* Explicitly approximate floating conversion. Use i64/u64 for timestamps and
   counts, or number_text to retain decimal precision. Range errors: FG_LIMIT. */
fg_status fg_json_double(const fg_json *value, double *out, fg_error *error);
/* Compact deterministic output; sorted keys, original number lexemes.
   Replaces an initialized buffer only on success. */
fg_status fg_json_serialize(const fg_json *value, fg_buffer *out, fg_error *error);

/* Constructors copy input; number text must be exactly one JSON number.
   new accepts NULL/BOOL/ARRAY/OBJECT (BOOL initially false).
   set replaces existing keys. set/append take ownership only on success;
   value must be an unattached root, not the target or one of its ancestors. */
fg_status fg_json_new(fg_json_kind kind, fg_json **out, fg_error *error);
fg_status fg_json_new_string(fg_str text, fg_json **out, fg_error *error);
fg_status fg_json_new_number(fg_str text, fg_json **out, fg_error *error);
fg_status fg_json_new_i64(int64_t value, fg_json **out, fg_error *error);
fg_status fg_json_new_u64(uint64_t value, fg_json **out, fg_error *error);
fg_status fg_json_new_bool(int value, fg_json **out, fg_error *error);
fg_status fg_json_set(fg_json *object, fg_str key, fg_json *value, fg_error *error);
fg_status fg_json_append(fg_json *array, fg_json *value, fg_error *error);

/* Full-buffer RFC4180 quoting, with LF record endings also accepted.
   Bare CR outside quotes and malformed quoting fail. Blank physical records
   are skipped (quoted empty records are not); a trailing comma adds a field.
   UTF-8 BOM is stripped. NUL and invalid UTF-8 fail.
   Row/field views last only for the callback; row_index is zero based.
   A failing callback stops parsing and its status/error are propagated.
   Earlier callbacks are not rolled back if a later record is malformed. */
typedef fg_status (*fg_csv_callback)(void *user, const fg_str *fields,
                                     size_t count, size_t row_index, fg_error *error);
fg_status fg_csv_parse(fg_str text, fg_csv_callback callback, void *user, fg_error *error);
typedef struct fg_csv_header fg_csv_header;
fg_status fg_csv_header_create(const fg_str *fields, size_t count,
                               fg_csv_header **out, fg_error *error);
void fg_csv_header_free(fg_csv_header *header);
fg_status fg_csv_column(const fg_csv_header *header, fg_str name,
                        size_t *out, fg_error *error);
fg_status fg_csv_field(const fg_str *fields, size_t count, size_t column,
                       fg_str *out, fg_error *error);

typedef struct fg_env fg_env;
/* UTF-8 BOM, CRLF/LF, blank lines, # comment lines, KEY = VALUE and single/
   double-quoted values. Keys: [A-Za-z_][A-Za-z0-9_]*. Values are literal:
   no interpolation/escape expansion; # inside values is preserved.
   A quoted value may be followed by whitespace and a # comment.
   Duplicate keys use the last assignment, matching the previous reader.
   NUL, invalid UTF-8, missing '=', and unclosed/stray quotes fail. */
fg_status fg_env_parse(fg_str text, fg_env **out, fg_error *error);
void fg_env_free(fg_env *env);
size_t fg_env_size(const fg_env *env);
fg_status fg_env_get(const fg_env *env, fg_str key, fg_str *out, fg_error *error);
/* Explicit opt-in only: never changes process environment. Empty directory
   starts at cwd; otherwise supply an absolute UTF-8 directory. Tries .env in
   that directory and parents, at most max_levels (zero means eight).
   Stops at the first existing file; parse/I/O errors do not fall through.
   Uses fg_read_file, with a 4 MiB per-file limit. FG_NOT_FOUND if absent. */
fg_status fg_env_load_upward(fg_str directory, size_t max_levels,
                             fg_env **out, fg_error *error);

#ifdef __cplusplus
}
#endif
#endif

#include "fg_data.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif

typedef struct fg_json_entry {
    fg_buffer key;
    fg_json *value;
} fg_json_entry;

struct fg_json {
    fg_json_kind kind;
    int boolean;
    fg_buffer text;
    fg_json_entry *entries;
    size_t count, capacity, height;
    fg_json *parent;
};

struct fg_csv_header {
    fg_buffer *fields;
    size_t count;
};

struct fg_env {
    fg_json *values;
};

static fg_str fg_view(const fg_buffer *buffer)
{
    fg_str result = {buffer->data, buffer->len};
    return result;
}

static fg_status fg_data_error(fg_error *error, fg_status status,
                               const char *message, size_t position)
{
    char text[256];
    (void)snprintf(text, sizeof(text), "%s at byte %zu", message, position);
    return fg_fail(error, status, text);
}

static fg_status fg_reserve(void *items, size_t *capacity, size_t needed,
                            size_t item_size, void **out, fg_error *error)
{
    size_t next;
    void *memory;
    if (needed <= *capacity) { *out = items; return FG_OK; }
    if (needed > SIZE_MAX / item_size)
        return fg_fail(error, FG_LIMIT, "Data allocation size overflow");
    next = *capacity ? *capacity : 8;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    if (next > SIZE_MAX / item_size) next = needed;
    memory = realloc(items, next * item_size);
    if (!memory) return fg_fail(error, FG_NOMEM, "Cannot allocate data entries");
    *out = memory;
    *capacity = next;
    return FG_OK;
}

static int fg_digit(char c) { return c >= '0' && c <= '9'; }
static int fg_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int fg_horizontal(char c) { return c == ' ' || c == '\t'; }

static int fg_compare(fg_str a, fg_str b)
{
    size_t common = a.len < b.len ? a.len : b.len;
    int result = common ? memcmp(a.data, b.data, common) : 0;
    if (result) return result;
    return (a.len > b.len) - (a.len < b.len);
}

static fg_status fg_utf8_validate(fg_str text, int allow_nul, fg_error *error)
{
    size_t i = 0;
    if (!text.data && text.len)
        return fg_fail(error, FG_INVALID, "NULL input with nonzero length");
    while (i < text.len) {
        size_t start = i, n, j;
        unsigned char c = (unsigned char)text.data[i++];
        uint32_t cp, minimum;
        if (c < 0x80) {
            if (!c && !allow_nul)
                return fg_data_error(error, FG_PARSE, "Unexpected NUL", start);
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31u; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15u; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7u; minimum = 0x10000; }
        else return fg_data_error(error, FG_PARSE, "Invalid UTF-8 lead byte", start);
        if (n > text.len - i)
            return fg_data_error(error, FG_PARSE, "Truncated UTF-8", start);
        for (j = 0; j < n; ++j) {
            c = (unsigned char)text.data[i++];
            if ((c & 0xc0u) != 0x80u)
                return fg_data_error(error, FG_PARSE, "Invalid UTF-8 continuation", i - 1);
            cp = (cp << 6) | (c & 63u);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return fg_data_error(error, FG_PARSE, "Invalid UTF-8 code point", start);
    }
    return FG_OK;
}

static fg_status fg_utf8_append(fg_buffer *out, uint32_t cp, fg_error *error)
{
    char bytes[4];
    size_t n;
    if (cp < 0x80) { bytes[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        bytes[0] = (char)(0xc0u | (cp >> 6));
        bytes[1] = (char)(0x80u | (cp & 63u)); n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char)(0xe0u | (cp >> 12));
        bytes[1] = (char)(0x80u | ((cp >> 6) & 63u));
        bytes[2] = (char)(0x80u | (cp & 63u)); n = 3;
    } else {
        bytes[0] = (char)(0xf0u | (cp >> 18));
        bytes[1] = (char)(0x80u | ((cp >> 12) & 63u));
        bytes[2] = (char)(0x80u | ((cp >> 6) & 63u));
        bytes[3] = (char)(0x80u | (cp & 63u)); n = 4;
    }
    return fg_buffer_append(out, bytes, n, error);
}

static fg_status fg_json_allocate(fg_json_kind kind, fg_json **out, fg_error *error)
{
    fg_json *value = (fg_json *)calloc(1, sizeof(*value));
    if (!value) return fg_fail(error, FG_NOMEM, "Cannot allocate JSON value");
    value->kind = kind;
    value->height = 1;
    *out = value;
    return FG_OK;
}

void fg_json_free(fg_json *value)
{
    size_t i;
    if (!value) return;
    for (i = 0; i < value->count; ++i) {
        fg_buffer_free(&value->entries[i].key);
        fg_json_free(value->entries[i].value);
    }
    free(value->entries);
    fg_buffer_free(&value->text);
    free(value);
}

fg_json_kind fg_json_type(const fg_json *value)
{
    return value ? value->kind : FG_JSON_NULL;
}

static size_t fg_json_find(const fg_json *object, fg_str key, int *found)
{
    size_t lo = 0, hi = object->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (fg_compare(fg_view(&object->entries[mid].key), key) < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < object->count && fg_compare(fg_view(&object->entries[lo].key), key) == 0;
    return lo;
}

const fg_json *fg_json_get(const fg_json *object, fg_str key)
{
    int found;
    size_t index;
    if (!object || object->kind != FG_JSON_OBJECT || (!key.data && key.len)) return NULL;
    index = fg_json_find(object, key, &found);
    return found ? object->entries[index].value : NULL;
}

const fg_json *fg_json_at(const fg_json *array, size_t index)
{
    if (!array || array->kind != FG_JSON_ARRAY || index >= array->count) return NULL;
    return array->entries[index].value;
}

size_t fg_json_size(const fg_json *value) { return value ? value->count : 0; }

fg_str fg_json_key(const fg_json *object, size_t index)
{
    fg_str empty = {NULL, 0};
    if (!object || object->kind != FG_JSON_OBJECT || index >= object->count) return empty;
    return fg_view(&object->entries[index].key);
}

fg_str fg_json_string(const fg_json *value)
{
    fg_str empty = {NULL, 0};
    return value && value->kind == FG_JSON_STRING ? fg_view(&value->text) : empty;
}

fg_str fg_json_number_text(const fg_json *value)
{
    fg_str empty = {NULL, 0};
    return value && value->kind == FG_JSON_NUMBER ? fg_view(&value->text) : empty;
}

fg_status fg_json_bool(const fg_json *value, int *out, fg_error *error)
{
    if (!out || !value || value->kind != FG_JSON_BOOL)
        return fg_fail(error, FG_INVALID, "Expected JSON boolean and output");
    *out = value->boolean;
    return FG_OK;
}

static fg_status fg_number_end(fg_str text, size_t *position, fg_error *error)
{
    size_t p = *position;
    if (p < text.len && text.data[p] == '-') ++p;
    if (p == text.len || !fg_digit(text.data[p]))
        return fg_data_error(error, FG_PARSE, "Expected JSON number digit", p);
    if (text.data[p] == '0') ++p;
    else while (p < text.len && fg_digit(text.data[p])) ++p;
    if (p < text.len && text.data[p] == '.') {
        ++p;
        if (p == text.len || !fg_digit(text.data[p]))
            return fg_data_error(error, FG_PARSE, "Expected fraction digit", p);
        while (p < text.len && fg_digit(text.data[p])) ++p;
    }
    if (p < text.len && (text.data[p] == 'e' || text.data[p] == 'E')) {
        ++p;
        if (p < text.len && (text.data[p] == '+' || text.data[p] == '-')) ++p;
        if (p == text.len || !fg_digit(text.data[p]))
            return fg_data_error(error, FG_PARSE, "Expected exponent digit", p);
        while (p < text.len && fg_digit(text.data[p])) ++p;
    }
    *position = p;
    return FG_OK;
}

typedef struct fg_json_parser {
    fg_str text;
    size_t position;
    fg_error *error;
} fg_json_parser;

static void fg_json_ws(fg_json_parser *parser)
{
    while (parser->position < parser->text.len && fg_space(parser->text.data[parser->position]))
        ++parser->position;
}

static fg_status fg_json_hex(fg_json_parser *parser, uint32_t *out)
{
    uint32_t cp = 0;
    size_t i;
    for (i = 0; i < 4; ++i) {
        char c;
        unsigned digit;
        if (parser->position == parser->text.len)
            return fg_data_error(parser->error, FG_PARSE, "Truncated Unicode escape", parser->position);
        c = parser->text.data[parser->position++];
        if (fg_digit(c)) digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else return fg_data_error(parser->error, FG_PARSE, "Invalid Unicode hex digit", parser->position - 1);
        cp = (cp << 4) | digit;
    }
    *out = cp;
    return FG_OK;
}

static fg_status fg_json_parse_string(fg_json_parser *parser, fg_buffer *out)
{
    fg_status status;
    if (parser->position == parser->text.len || parser->text.data[parser->position] != '"')
        return fg_data_error(parser->error, FG_PARSE, "Expected JSON string", parser->position);
    ++parser->position;
    while (parser->position < parser->text.len) {
        char c = parser->text.data[parser->position++];
        if (c == '"') {
            /* Even an empty string has a stable, non-NULL borrowed view. */
            return fg_buffer_append(out, "", 0, parser->error);
        }
        if ((unsigned char)c < 0x20)
            return fg_data_error(parser->error, FG_PARSE, "Unescaped JSON control character", parser->position - 1);
        if (c == '\\') {
            if (parser->position == parser->text.len)
                return fg_data_error(parser->error, FG_PARSE, "Truncated JSON escape", parser->position);
            c = parser->text.data[parser->position++];
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                uint32_t cp, low;
                status = fg_json_hex(parser, &cp);
                if (status != FG_OK) return status;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (parser->text.len - parser->position < 2 ||
                        parser->text.data[parser->position] != '\\' ||
                        parser->text.data[parser->position + 1] != 'u')
                        return fg_data_error(parser->error, FG_PARSE, "Missing low surrogate", parser->position);
                    parser->position += 2;
                    status = fg_json_hex(parser, &low);
                    if (status != FG_OK) return status;
                    if (low < 0xdc00 || low > 0xdfff)
                        return fg_data_error(parser->error, FG_PARSE, "Invalid low surrogate", parser->position - 4);
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return fg_data_error(parser->error, FG_PARSE, "Lone low surrogate", parser->position - 4);
                }
                status = fg_utf8_append(out, cp, parser->error);
                if (status != FG_OK) return status;
                continue;
            }
            default:
                return fg_data_error(parser->error, FG_PARSE, "Invalid JSON escape", parser->position - 1);
            }
        }
        status = fg_buffer_append(out, &c, 1, parser->error);
        if (status != FG_OK) return status;
    }
    return fg_data_error(parser->error, FG_PARSE, "Unterminated JSON string", parser->position);
}

static fg_status fg_json_slot(fg_json *value, fg_error *error)
{
    void *memory = NULL;
    fg_status status;
    if (value->count == SIZE_MAX)
        return fg_fail(error, FG_LIMIT, "Too many JSON members");
    status = fg_reserve(value->entries, &value->capacity, value->count + 1,
                        sizeof(*value->entries), &memory, error);
    if (status == FG_OK) value->entries = (fg_json_entry *)memory;
    return status;
}

static int fg_entry_compare(const void *left, const void *right)
{
    const fg_json_entry *a = (const fg_json_entry *)left;
    const fg_json_entry *b = (const fg_json_entry *)right;
    return fg_compare(fg_view(&a->key), fg_view(&b->key));
}

static fg_status fg_json_parse_value(fg_json_parser *parser, size_t depth, fg_json **out)
{
    fg_json *value = NULL;
    fg_status status;
    fg_json_kind kind;
    char c;
    size_t start;
    fg_json_ws(parser);
    if (depth > FG_JSON_MAX_DEPTH)
        return fg_data_error(parser->error, FG_LIMIT, "JSON nesting limit exceeded", parser->position);
    if (parser->position == parser->text.len)
        return fg_data_error(parser->error, FG_PARSE, "Expected JSON value", parser->position);
    c = parser->text.data[parser->position];
    kind = c == '{' ? FG_JSON_OBJECT : c == '[' ? FG_JSON_ARRAY :
           c == '"' ? FG_JSON_STRING : c == 't' || c == 'f' ? FG_JSON_BOOL :
           c == 'n' ? FG_JSON_NULL : FG_JSON_NUMBER;
    status = fg_json_allocate(kind, &value, parser->error);
    if (status != FG_OK) return status;
    if (kind == FG_JSON_OBJECT || kind == FG_JSON_ARRAY) {
        char close = kind == FG_JSON_OBJECT ? '}' : ']';
        ++parser->position;
        fg_json_ws(parser);
        if (parser->position < parser->text.len && parser->text.data[parser->position] == close) {
            ++parser->position;
        } else for (;;) {
            fg_json_entry entry = {{0}, NULL};
            if (kind == FG_JSON_OBJECT) {
                status = fg_json_parse_string(parser, &entry.key);
                if (status != FG_OK) { fg_buffer_free(&entry.key); goto fail; }
                fg_json_ws(parser);
                if (parser->position == parser->text.len || parser->text.data[parser->position] != ':') {
                    fg_buffer_free(&entry.key);
                    status = fg_data_error(parser->error, FG_PARSE, "Expected ':'", parser->position);
                    goto fail;
                }
                ++parser->position;
            }
            status = fg_json_parse_value(parser, depth + 1, &entry.value);
            if (status != FG_OK) { fg_buffer_free(&entry.key); goto fail; }
            status = fg_json_slot(value, parser->error);
            if (status != FG_OK) {
                fg_buffer_free(&entry.key);
                fg_json_free(entry.value);
                goto fail;
            }
            entry.value->parent = value;
            if (entry.value->height + 1 > value->height) value->height = entry.value->height + 1;
            value->entries[value->count++] = entry;
            fg_json_ws(parser);
            if (parser->position == parser->text.len) {
                status = fg_data_error(parser->error, FG_PARSE, "Unterminated JSON container", parser->position);
                goto fail;
            }
            c = parser->text.data[parser->position++];
            if (c == close) break;
            if (c != ',') {
                status = fg_data_error(parser->error, FG_PARSE, "Expected comma or container end", parser->position - 1);
                goto fail;
            }
            fg_json_ws(parser);
        }
        if (kind == FG_JSON_OBJECT && value->count > 1) {
            size_t i;
            qsort(value->entries, value->count, sizeof(*value->entries), fg_entry_compare);
            for (i = 1; i < value->count; ++i) {
                if (!fg_compare(fg_view(&value->entries[i - 1].key), fg_view(&value->entries[i].key))) {
                    status = fg_data_error(parser->error, FG_PARSE, "Duplicate JSON object key", parser->position);
                    goto fail;
                }
            }
        }
    } else if (kind == FG_JSON_STRING) {
        status = fg_json_parse_string(parser, &value->text);
        if (status != FG_OK) goto fail;
    } else if (kind == FG_JSON_NUMBER) {
        fg_str number;
        start = parser->position;
        status = fg_number_end(parser->text, &parser->position, parser->error);
        if (status != FG_OK) goto fail;
        number.data = parser->text.data + start;
        number.len = parser->position - start;
        status = fg_buffer_set(&value->text, number, parser->error);
        if (status != FG_OK) goto fail;
    } else {
        const char *word = c == 't' ? "true" : c == 'f' ? "false" : "null";
        size_t len = c == 'f' ? 5 : 4;
        if (parser->text.len - parser->position < len ||
            memcmp(parser->text.data + parser->position, word, len)) {
            status = fg_data_error(parser->error, FG_PARSE, "Invalid JSON literal", parser->position);
            goto fail;
        }
        value->boolean = c == 't';
        parser->position += len;
    }
    *out = value;
    return FG_OK;
fail:
    fg_json_free(value);
    return status;
}

fg_status fg_json_parse(fg_str text, fg_json **out, fg_error *error)
{
    fg_json_parser parser = {text, 0, error};
    fg_json *value = NULL;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing JSON output");
    *out = NULL;
    status = fg_utf8_validate(text, 0, error);
    if (status != FG_OK) return status;
    status = fg_json_parse_value(&parser, 1, &value);
    if (status != FG_OK) return status;
    fg_json_ws(&parser);
    if (parser.position != text.len) {
        fg_json_free(value);
        return fg_data_error(error, FG_PARSE, "Trailing JSON data", parser.position);
    }
    *out = value;
    return FG_OK;
}

typedef struct fg_decimal {
    fg_str text;
    size_t start, end, first, last, digits, whole;
    int negative;
    int64_t exponent;
} fg_decimal;

static fg_status fg_decimal_parts(const fg_json *value, fg_decimal *out, fg_error *error)
{
    size_t i;
    int fraction = 0, exponent_negative = 0;
    memset(out, 0, sizeof(*out));
    if (!value || value->kind != FG_JSON_NUMBER)
        return fg_fail(error, FG_INVALID, "Expected JSON number");
    out->text = fg_view(&value->text);
    if (out->text.len > 1000000000u)
        return fg_fail(error, FG_LIMIT, "JSON number too long to convert");
    out->negative = out->text.data[0] == '-';
    out->start = out->negative ? 1 : 0;
    out->first = SIZE_MAX;
    for (i = out->start; i < out->text.len; ++i) {
        char c = out->text.data[i];
        if (c == 'e' || c == 'E') break;
        if (c == '.') { fraction = 1; continue; }
        if (c != '0') {
            if (out->first == SIZE_MAX) out->first = out->digits;
            out->last = out->digits;
        }
        ++out->digits;
        if (!fraction) ++out->whole;
    }
    out->end = i;
    if (i < out->text.len) {
        ++i;
        if (out->text.data[i] == '-' || out->text.data[i] == '+')
            exponent_negative = out->text.data[i++] == '-';
        for (; i < out->text.len; ++i) {
            /* Saturation retains range/fraction decisions without signed overflow. */
            if (out->exponent < INT64_C(2000000000))
                out->exponent = out->exponent * 10 + out->text.data[i] - '0';
        }
        if (exponent_negative) out->exponent = -out->exponent;
    }
    return FG_OK;
}

static fg_status fg_json_magnitude(const fg_json *value, uint64_t *out, int *negative,
                                   fg_error *error)
{
    fg_decimal decimal;
    fg_status status = fg_decimal_parts(value, &decimal, error);
    int64_t point;
    size_t i, digit_index = 0, used = 0, needed;
    uint64_t result = 0;
    if (status != FG_OK) return status;
    *negative = decimal.negative;
    if (decimal.first == SIZE_MAX) { *out = 0; return FG_OK; }
    point = (int64_t)decimal.whole + decimal.exponent;
    if (point <= (int64_t)decimal.last)
        return fg_fail(error, FG_INVALID, "JSON number is not an exact integer");
    if (point - (int64_t)decimal.first > 20)
        return fg_fail(error, FG_LIMIT, "JSON integer is outside uint64 range");
    needed = (size_t)(point - (int64_t)decimal.first);
    for (i = decimal.start; used < needed; ) {
        unsigned digit = 0;
        if (i < decimal.end) {
            char c = decimal.text.data[i++];
            if (c == '.') continue;
            if (digit_index++ < decimal.first) continue;
            digit = (unsigned)(c - '0');
        }
        if (result > (UINT64_MAX - digit) / 10)
            return fg_fail(error, FG_LIMIT, "JSON integer is outside uint64 range");
        result = result * 10 + digit;
        ++used;
    }
    *out = result;
    return FG_OK;
}

fg_status fg_json_u64(const fg_json *value, uint64_t *out, fg_error *error)
{
    uint64_t magnitude = 0;
    int negative = 0;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing uint64 output");
    status = fg_json_magnitude(value, &magnitude, &negative, error);
    if (status != FG_OK) return status;
    if (negative && magnitude)
        return fg_fail(error, FG_LIMIT, "Negative JSON number cannot be uint64");
    *out = magnitude;
    return FG_OK;
}

fg_status fg_json_i64(const fg_json *value, int64_t *out, fg_error *error)
{
    uint64_t magnitude = 0;
    int negative = 0;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing int64 output");
    status = fg_json_magnitude(value, &magnitude, &negative, error);
    if (status != FG_OK) return status;
    if (magnitude > (uint64_t)INT64_MAX + (negative ? 1u : 0u))
        return fg_fail(error, FG_LIMIT, "JSON integer is outside int64 range");
    if (negative && magnitude == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
    else *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return FG_OK;
}

/* Binary64 rounding boundaries are terminating decimals with at most 1075
   fractional places. Retaining 1100 significant digits plus a sticky bit
   distinguishes every boundary, even after arbitrarily long zero tails.
   160 limbs cover 10^1423 and the shifted numerator for subnormal rounding. */
typedef struct fg_big {
    uint32_t words[160];
    size_t count;
} fg_big;

static void fg_big_multiply(fg_big *value, uint32_t factor, uint32_t add)
{
    size_t i;
    uint64_t carry = add;
    for (i = 0; i < value->count; ++i) {
        carry += (uint64_t)value->words[i] * factor;
        value->words[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry) value->words[value->count++] = (uint32_t)carry;
}

static void fg_big_power10(fg_big *value, unsigned exponent)
{
    while (exponent >= 9) { fg_big_multiply(value, 1000000000u, 0); exponent -= 9; }
    while (exponent--) fg_big_multiply(value, 10, 0);
}

static size_t fg_big_bits(const fg_big *value)
{
    uint32_t top;
    size_t bits;
    if (!value->count) return 0;
    top = value->words[value->count - 1];
    bits = (value->count - 1) * 32;
    while (top) { ++bits; top >>= 1; }
    return bits;
}

static int fg_big_compare(const fg_big *a, const fg_big *b)
{
    size_t i;
    if (a->count != b->count) return (a->count > b->count) - (a->count < b->count);
    for (i = a->count; i; --i) {
        if (a->words[i - 1] != b->words[i - 1])
            return (a->words[i - 1] > b->words[i - 1]) ? 1 : -1;
    }
    return 0;
}

static void fg_big_shift(fg_big *out, const fg_big *in, unsigned bits)
{
    size_t offset = bits / 32, i;
    unsigned partial = bits % 32;
    uint64_t carry = 0;
    memset(out, 0, sizeof(*out));
    if (!in->count) return;
    for (i = 0; i < in->count; ++i) {
        carry |= (uint64_t)in->words[i] << partial;
        out->words[i + offset] = (uint32_t)carry;
        carry >>= 32;
    }
    out->count = in->count + offset;
    if (carry) out->words[out->count++] = (uint32_t)carry;
}

static void fg_big_subtract(fg_big *a, const fg_big *b)
{
    size_t i;
    uint64_t borrow = 0;
    for (i = 0; i < a->count; ++i) {
        uint64_t left = a->words[i];
        uint64_t right = (i < b->count ? b->words[i] : 0u) + borrow;
        a->words[i] = (uint32_t)(left - right);
        borrow = left < right;
    }
    while (a->count && !a->words[a->count - 1]) --a->count;
}

fg_status fg_json_double(const fg_json *value, double *out, fg_error *error)
{
    fg_decimal decimal;
    fg_status status;
    fg_big numerator = {{0}, 0}, denominator = {{1}, 1}, shifted;
    size_t i, digit_index = 0, kept = 0;
    int64_t adjusted;
    int scale, exponent, quantum, bit, comparison, sticky = 0;
    uint64_t mantissa = 0;
    double number;
    if (!out) return fg_fail(error, FG_INVALID, "Missing double output");
#if FLT_RADIX != 2 || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024 || DBL_MIN_EXP != -1021
    return fg_fail(error, FG_INVALID, "JSON floating conversion requires binary64 double");
#endif
    status = fg_decimal_parts(value, &decimal, error);
    if (status != FG_OK) return status;
    if (decimal.first == SIZE_MAX) { *out = decimal.negative ? -0.0 : 0.0; return FG_OK; }
    adjusted = (int64_t)decimal.whole + decimal.exponent - (int64_t)decimal.first - 1;
    if (adjusted < -324 || adjusted > 308)
        return fg_fail(error, FG_LIMIT, "JSON number outside double range");
    for (i = decimal.start; i < decimal.end; ++i) {
        char c = decimal.text.data[i];
        if (c == '.') continue;
        if (digit_index++ < decimal.first) continue;
        if (kept < 1100) {
            fg_big_multiply(&numerator, 10, (uint32_t)(c - '0'));
            ++kept;
        } else if (c != '0') sticky = 1;
    }
    scale = (int)adjusted + 1 - (int)kept;
    if (scale >= 0) fg_big_power10(&numerator, (unsigned)scale);
    else fg_big_power10(&denominator, (unsigned)-scale);
    exponent = (int)fg_big_bits(&numerator) - (int)fg_big_bits(&denominator);
    if (exponent >= 0) {
        fg_big_shift(&shifted, &denominator, (unsigned)exponent);
        if (fg_big_compare(&numerator, &shifted) < 0) --exponent;
    } else {
        fg_big_shift(&shifted, &numerator, (unsigned)-exponent);
        if (fg_big_compare(&shifted, &denominator) < 0) --exponent;
    }
    quantum = exponent - 52;
    if (quantum < -1074) quantum = -1074;
    if (quantum < 0) {
        fg_big_shift(&shifted, &numerator, (unsigned)-quantum);
        numerator = shifted;
    } else {
        fg_big_shift(&shifted, &denominator, (unsigned)quantum);
        denominator = shifted;
    }
    bit = (int)fg_big_bits(&numerator) - (int)fg_big_bits(&denominator);
    for (; bit >= 0; --bit) {
        fg_big_shift(&shifted, &denominator, (unsigned)bit);
        if (fg_big_compare(&numerator, &shifted) >= 0) {
            fg_big_subtract(&numerator, &shifted);
            mantissa |= UINT64_C(1) << bit;
        }
    }
    fg_big_shift(&shifted, &numerator, 1);
    comparison = fg_big_compare(&shifted, &denominator);
    if (comparison > 0 || (comparison == 0 && (sticky || (mantissa & 1u)))) ++mantissa;
    if (!mantissa) return fg_fail(error, FG_LIMIT, "JSON number underflows double");
    number = ldexp((double)mantissa, quantum);
    if (!isfinite(number)) return fg_fail(error, FG_LIMIT, "JSON number overflows double");
    *out = decimal.negative ? -number : number;
    return FG_OK;
}

fg_status fg_json_new(fg_json_kind kind, fg_json **out, fg_error *error)
{
    if (!out) return fg_fail(error, FG_INVALID, "Missing JSON output");
    *out = NULL;
    if (kind != FG_JSON_NULL && kind != FG_JSON_BOOL && kind != FG_JSON_ARRAY && kind != FG_JSON_OBJECT)
        return fg_fail(error, FG_INVALID, "Use typed constructor for JSON string/number");
    return fg_json_allocate(kind, out, error);
}

fg_status fg_json_new_string(fg_str text, fg_json **out, fg_error *error)
{
    fg_json *value = NULL;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing JSON output");
    *out = NULL;
    status = fg_utf8_validate(text, 1, error);
    if (status != FG_OK) return status;
    status = fg_json_allocate(FG_JSON_STRING, &value, error);
    if (status == FG_OK) status = fg_buffer_set(&value->text, text, error);
    if (status != FG_OK) { fg_json_free(value); return status; }
    *out = value;
    return FG_OK;
}

fg_status fg_json_new_number(fg_str text, fg_json **out, fg_error *error)
{
    size_t position = 0;
    fg_json *value = NULL;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing JSON output");
    *out = NULL;
    if (!text.data && text.len) return fg_fail(error, FG_INVALID, "Invalid number input");
    status = fg_number_end(text, &position, error);
    if (status != FG_OK) return status;
    if (position != text.len)
        return fg_data_error(error, FG_PARSE, "Trailing JSON number data", position);
    status = fg_json_allocate(FG_JSON_NUMBER, &value, error);
    if (status == FG_OK) status = fg_buffer_set(&value->text, text, error);
    if (status != FG_OK) { fg_json_free(value); return status; }
    *out = value;
    return FG_OK;
}

static fg_status fg_json_new_integer(uint64_t magnitude, int negative, fg_json **out, fg_error *error)
{
    char text[21];
    size_t position = sizeof(text);
    fg_str number;
    do {
        text[--position] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (negative) text[--position] = '-';
    number.data = text + position;
    number.len = sizeof(text) - position;
    return fg_json_new_number(number, out, error);
}

fg_status fg_json_new_i64(int64_t value, fg_json **out, fg_error *error)
{
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
    return fg_json_new_integer(magnitude, value < 0, out, error);
}

fg_status fg_json_new_u64(uint64_t value, fg_json **out, fg_error *error)
{
    return fg_json_new_integer(value, 0, out, error);
}

fg_status fg_json_new_bool(int value, fg_json **out, fg_error *error)
{
    fg_status status = fg_json_new(FG_JSON_BOOL, out, error);
    if (status == FG_OK) (*out)->boolean = value != 0;
    return status;
}

static fg_status fg_json_can_attach(fg_json *target, fg_json *value, fg_error *error)
{
    fg_json *ancestor;
    size_t levels = 0;
    if (!target || !value)
        return fg_fail(error, FG_INVALID, "Missing JSON container or child");
    if (value->parent)
        return fg_fail(error, FG_INVALID, "JSON child already belongs to a container");
    for (ancestor = target; ancestor; ancestor = ancestor->parent) {
        if (ancestor == value) return fg_fail(error, FG_INVALID, "JSON ownership cycle");
        ++levels;
    }
    if (levels + value->height > FG_JSON_MAX_DEPTH)
        return fg_fail(error, FG_LIMIT, "JSON nesting limit exceeded");
    return FG_OK;
}

static void fg_json_update_height(fg_json *value)
{
    for (; value; value = value->parent) {
        size_t i, height = 1;
        for (i = 0; i < value->count; ++i) {
            size_t child_height = value->entries[i].value->height + 1;
            if (child_height > height) height = child_height;
        }
        if (height == value->height) break;
        value->height = height;
    }
}

static void fg_json_raise_height(fg_json *container, size_t child_height)
{
    for (; container && container->height < child_height + 1; container = container->parent) {
        container->height = ++child_height;
    }
}

fg_status fg_json_set(fg_json *object, fg_str key, fg_json *value, fg_error *error)
{
    fg_status status;
    fg_buffer copy = {0};
    size_t index, old_height = 0;
    int found;
    if (!object || object->kind != FG_JSON_OBJECT)
        return fg_fail(error, FG_INVALID, "Expected JSON object");
    status = fg_json_can_attach(object, value, error);
    if (status != FG_OK) return status;
    status = fg_utf8_validate(key, 1, error);
    if (status != FG_OK) return status;
    index = fg_json_find(object, key, &found);
    if (found) {
        old_height = object->entries[index].value->height;
        fg_json_free(object->entries[index].value);
        object->entries[index].value = value;
    } else {
        status = fg_buffer_set(&copy, key, error);
        if (status == FG_OK) status = fg_json_slot(object, error);
        if (status != FG_OK) { fg_buffer_free(&copy); return status; }
        memmove(object->entries + index + 1, object->entries + index,
                (object->count - index) * sizeof(*object->entries));
        object->entries[index].key = copy;
        object->entries[index].value = value;
        ++object->count;
    }
    value->parent = object;
    if (old_height > value->height) fg_json_update_height(object);
    else fg_json_raise_height(object, value->height);
    return FG_OK;
}

fg_status fg_json_append(fg_json *array, fg_json *value, fg_error *error)
{
    fg_status status;
    fg_json_entry entry = {{0}, value};
    if (!array || array->kind != FG_JSON_ARRAY)
        return fg_fail(error, FG_INVALID, "Expected JSON array");
    status = fg_json_can_attach(array, value, error);
    if (status != FG_OK) return status;
    status = fg_json_slot(array, error);
    if (status != FG_OK) return status;
    array->entries[array->count++] = entry;
    value->parent = array;
    fg_json_raise_height(array, value->height);
    return FG_OK;
}

static fg_status fg_json_write_string(fg_str text, fg_buffer *out, fg_error *error)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, start = 0;
    fg_status status = fg_buffer_append(out, "\"", 1, error);
    if (status != FG_OK) return status;
    for (i = 0; i < text.len; ++i) {
        unsigned char c = (unsigned char)text.data[i];
        char escape[6] = {'\\', 'u', '0', '0', '0', '0'};
        size_t count;
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        status = fg_buffer_append(out, text.data + start, i - start, error);
        if (status != FG_OK) return status;
        if (c == '"' || c == '\\') { escape[1] = (char)c; count = 2; }
        else {
            escape[4] = hex[c >> 4];
            escape[5] = hex[c & 15u];
            count = 6;
        }
        status = fg_buffer_append(out, escape, count, error);
        if (status != FG_OK) return status;
        start = i + 1;
    }
    if (start < text.len) {
        status = fg_buffer_append(out, text.data + start, text.len - start, error);
        if (status != FG_OK) return status;
    }
    return fg_buffer_append(out, "\"", 1, error);
}

static fg_status fg_json_write(const fg_json *value, fg_buffer *out, fg_error *error)
{
    size_t i;
    fg_status status;
    switch (value->kind) {
    case FG_JSON_NULL: return fg_buffer_append(out, "null", 4, error);
    case FG_JSON_BOOL:
        return fg_buffer_append(out, value->boolean ? "true" : "false", value->boolean ? 4 : 5, error);
    case FG_JSON_NUMBER: return fg_buffer_append(out, value->text.data, value->text.len, error);
    case FG_JSON_STRING: return fg_json_write_string(fg_view(&value->text), out, error);
    case FG_JSON_ARRAY: case FG_JSON_OBJECT:
        status = fg_buffer_append(out, value->kind == FG_JSON_ARRAY ? "[" : "{", 1, error);
        if (status != FG_OK) return status;
        for (i = 0; i < value->count; ++i) {
            if (i) {
                status = fg_buffer_append(out, ",", 1, error);
                if (status != FG_OK) return status;
            }
            if (value->kind == FG_JSON_OBJECT) {
                status = fg_json_write_string(fg_view(&value->entries[i].key), out, error);
                if (status != FG_OK) return status;
                status = fg_buffer_append(out, ":", 1, error);
                if (status != FG_OK) return status;
            }
            status = fg_json_write(value->entries[i].value, out, error);
            if (status != FG_OK) return status;
        }
        return fg_buffer_append(out, value->kind == FG_JSON_ARRAY ? "]" : "}", 1, error);
    }
    return fg_fail(error, FG_INVALID, "Unknown JSON value type");
}

fg_status fg_json_serialize(const fg_json *value, fg_buffer *out, fg_error *error)
{
    fg_buffer result = {0};
    fg_status status;
    if (!value || !out) return fg_fail(error, FG_INVALID, "Missing JSON value or output");
    status = fg_json_write(value, &result, error);
    if (status != FG_OK) { fg_buffer_free(&result); return status; }
    fg_buffer_free(out);
    *out = result;
    return FG_OK;
}

static size_t fg_skip_bom(fg_str text)
{
    return text.len >= 3 && (unsigned char)text.data[0] == 0xef &&
           (unsigned char)text.data[1] == 0xbb && (unsigned char)text.data[2] == 0xbf ? 3 : 0;
}

static fg_status fg_record_end(fg_str text, size_t *position, fg_error *error)
{
    if (text.data[*position] == '\r') {
        if (text.len - *position < 2 || text.data[*position + 1] != '\n')
            return fg_data_error(error, FG_PARSE, "Bare CR record ending", *position);
        ++*position;
    }
    ++*position;
    return FG_OK;
}

fg_status fg_csv_parse(fg_str text, fg_csv_callback callback, void *user, fg_error *error)
{
    fg_buffer *fields = NULL;
    fg_str *views = NULL;
    size_t position, field_capacity = 0, view_capacity = 0, count = 0, row = 0, i;
    fg_status status;
    if (!callback) return fg_fail(error, FG_INVALID, "Missing CSV callback");
    status = fg_utf8_validate(text, 0, error);
    if (status != FG_OK) return status;
    position = fg_skip_bom(text);
    while (position < text.len) {
        if (text.data[position] == '\r' || text.data[position] == '\n') {
            status = fg_record_end(text, &position, error);
            if (status != FG_OK) goto finish;
            continue;
        }
        for (;;) {
            fg_buffer field = {0};
            void *memory = NULL;
            size_t start = position;
            if (position < text.len && text.data[position] == '"') {
                int closed = 0;
                ++position;
                while (position < text.len) {
                    char c = text.data[position++];
                    if (c == '"') {
                        if (position < text.len && text.data[position] == '"') ++position;
                        else { closed = 1; break; }
                    }
                    status = fg_buffer_append(&field, &c, 1, error);
                    if (status != FG_OK) break;
                }
                if (status == FG_OK && !closed)
                    status = fg_data_error(error, FG_PARSE, "Unterminated quoted CSV field", start);
                if (status == FG_OK && position < text.len &&
                    text.data[position] != ',' && text.data[position] != '\r' && text.data[position] != '\n')
                    status = fg_data_error(error, FG_PARSE, "Unexpected data after closing CSV quote", position);
            } else {
                while (position < text.len && text.data[position] != ',' &&
                       text.data[position] != '\r' && text.data[position] != '\n') {
                    if (text.data[position] == '"') {
                        status = fg_data_error(error, FG_PARSE, "Quote inside unquoted CSV field", position);
                        break;
                    }
                    ++position;
                }
                if (status == FG_OK && position > start)
                    status = fg_buffer_append(&field, text.data + start, position - start, error);
            }
            if (status == FG_OK) status = fg_buffer_append(&field, "", 0, error);
            if (status == FG_OK && count == SIZE_MAX)
                status = fg_fail(error, FG_LIMIT, "Too many CSV fields");
            if (status == FG_OK) {
                status = fg_reserve(fields, &field_capacity, count + 1, sizeof(*fields), &memory, error);
                if (status == FG_OK) fields = (fg_buffer *)memory;
            }
            if (status != FG_OK) { fg_buffer_free(&field); goto finish; }
            fields[count++] = field;
            if (position == text.len || text.data[position] != ',') break;
            ++position;
        }
        if (position < text.len) {
            status = fg_record_end(text, &position, error);
            if (status != FG_OK) goto finish;
        }
        {
            void *memory = NULL;
            status = fg_reserve(views, &view_capacity, count, sizeof(*views), &memory, error);
            if (status == FG_OK) views = (fg_str *)memory;
        }
        if (status != FG_OK) goto finish;
        for (i = 0; i < count; ++i) views[i] = fg_view(&fields[i]);
        status = callback(user, views, count, row, error);
        if (status != FG_OK) goto finish;
        ++row;
        for (i = 0; i < count; ++i) fg_buffer_free(&fields[i]);
        count = 0;
    }
finish:
    for (i = 0; i < count; ++i) fg_buffer_free(&fields[i]);
    free(fields);
    free(views);
    return status;
}

void fg_csv_header_free(fg_csv_header *header)
{
    size_t i;
    if (!header) return;
    for (i = 0; i < header->count; ++i) fg_buffer_free(&header->fields[i]);
    free(header->fields);
    free(header);
}

fg_status fg_csv_header_create(const fg_str *fields, size_t count, fg_csv_header **out, fg_error *error)
{
    fg_csv_header *header;
    size_t i, j;
    fg_status status;
    if (!out) return fg_fail(error, FG_INVALID, "Missing CSV header output");
    *out = NULL;
    if (!fields || !count) return fg_fail(error, FG_INVALID, "Empty CSV header");
    if (count > SIZE_MAX / sizeof(fg_buffer))
        return fg_fail(error, FG_LIMIT, "Too many CSV columns");
    header = (fg_csv_header *)calloc(1, sizeof(*header));
    if (!header) return fg_fail(error, FG_NOMEM, "Cannot allocate CSV header");
    header->fields = (fg_buffer *)calloc(count, sizeof(*header->fields));
    if (!header->fields) { free(header); return fg_fail(error, FG_NOMEM, "Cannot allocate CSV columns"); }
    header->count = count;
    for (i = 0; i < count; ++i) {
        status = fg_utf8_validate(fields[i], 0, error);
        if (status != FG_OK) { fg_csv_header_free(header); return status; }
        for (j = 0; j < i; ++j) {
            if (!fg_compare(fields[i], fields[j])) {
                fg_csv_header_free(header);
                return fg_fail(error, FG_PARSE, "Duplicate CSV column name");
            }
        }
        status = fg_buffer_set(&header->fields[i], fields[i], error);
        if (status != FG_OK) { fg_csv_header_free(header); return status; }
    }
    *out = header;
    return FG_OK;
}

fg_status fg_csv_column(const fg_csv_header *header, fg_str name, size_t *out, fg_error *error)
{
    size_t i;
    if (!header || !out || (!name.data && name.len))
        return fg_fail(error, FG_INVALID, "Invalid CSV column lookup");
    for (i = 0; i < header->count; ++i) {
        if (!fg_compare(fg_view(&header->fields[i]), name)) { *out = i; return FG_OK; }
    }
    return fg_fail(error, FG_NOT_FOUND, "CSV column not found");
}

fg_status fg_csv_field(const fg_str *fields, size_t count, size_t column, fg_str *out, fg_error *error)
{
    if (!out || (!fields && count))
        return fg_fail(error, FG_INVALID, "Invalid CSV field lookup");
    if (column >= count) return fg_fail(error, FG_NOT_FOUND, "CSV row has no such column");
    *out = fields[column];
    return FG_OK;
}

static int fg_env_key_first(char c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

void fg_env_free(fg_env *env)
{
    if (!env) return;
    fg_json_free(env->values);
    free(env);
}

fg_status fg_env_parse(fg_str text, fg_env **out, fg_error *error)
{
    fg_env *env;
    fg_status status;
    size_t position;
    if (!out) return fg_fail(error, FG_INVALID, "Missing environment output");
    *out = NULL;
    status = fg_utf8_validate(text, 0, error);
    if (status != FG_OK) return status;
    env = (fg_env *)calloc(1, sizeof(*env));
    if (!env) return fg_fail(error, FG_NOMEM, "Cannot allocate environment table");
    status = fg_json_new(FG_JSON_OBJECT, &env->values, error);
    if (status != FG_OK) { fg_env_free(env); return status; }
    position = fg_skip_bom(text);
    while (position < text.len) {
        size_t start = position, end, key_end, value_start, value_end, i;
        fg_str key, val;
        fg_json *value = NULL;
        while (position < text.len && text.data[position] != '\r' && text.data[position] != '\n')
            ++position;
        end = position;
        if (position < text.len) {
            status = fg_record_end(text, &position, error);
            if (status != FG_OK) goto fail;
        }
        while (start < end && fg_horizontal(text.data[start])) ++start;
        while (end > start && fg_horizontal(text.data[end - 1])) --end;
        if (start == end || text.data[start] == '#') continue;
        if (!fg_env_key_first(text.data[start])) {
            status = fg_data_error(error, FG_PARSE, "Invalid environment key", start);
            goto fail;
        }
        key_end = start + 1;
        while (key_end < end && (fg_env_key_first(text.data[key_end]) || fg_digit(text.data[key_end])))
            ++key_end;
        i = key_end;
        while (i < end && fg_horizontal(text.data[i])) ++i;
        if (i == end || text.data[i] != '=') {
            status = fg_data_error(error, FG_PARSE, "Expected '=' after environment key", i);
            goto fail;
        }
        ++i;
        while (i < end && fg_horizontal(text.data[i])) ++i;
        value_start = i;
        value_end = end;
        if (i < end && (text.data[i] == '"' || text.data[i] == '\'')) {
            char quote = text.data[i++];
            value_start = i;
            while (i < end && text.data[i] != quote) ++i;
            if (i == end) {
                status = fg_data_error(error, FG_PARSE, "Unterminated quoted environment value", value_start - 1);
                goto fail;
            }
            value_end = i++;
            while (i < end && fg_horizontal(text.data[i])) ++i;
            if (i < end && text.data[i] != '#') {
                status = fg_data_error(error, FG_PARSE, "Unexpected data after environment quote", i);
                goto fail;
            }
        } else {
            for (; i < end; ++i) {
                if (text.data[i] == '"' || text.data[i] == '\'') {
                    status = fg_data_error(error, FG_PARSE, "Quote inside unquoted environment value", i);
                    goto fail;
                }
            }
        }
        for (i = value_start; i < value_end; ++i) {
            if ((unsigned char)text.data[i] < 0x20 && text.data[i] != '\t') {
                status = fg_data_error(error, FG_PARSE, "Control character in environment value", i);
                goto fail;
            }
        }
        key.data = text.data + start; key.len = key_end - start;
        val.data = text.data + value_start; val.len = value_end - value_start;
        status = fg_json_new_string(val, &value, error);
        if (status == FG_OK) status = fg_json_set(env->values, key, value, error);
        if (status != FG_OK) { fg_json_free(value); goto fail; }
    }
    *out = env;
    return FG_OK;
fail:
    fg_env_free(env);
    return status;
}

size_t fg_env_size(const fg_env *env) { return env ? fg_json_size(env->values) : 0; }

fg_status fg_env_get(const fg_env *env, fg_str key, fg_str *out, fg_error *error)
{
    const fg_json *value;
    if (!env || !out || (!key.data && key.len))
        return fg_fail(error, FG_INVALID, "Invalid environment lookup");
    value = fg_json_get(env->values, key);
    if (!value) return fg_fail(error, FG_NOT_FOUND, "Environment key not found");
    *out = fg_json_string(value);
    return FG_OK;
}

static fg_status fg_current_directory(fg_buffer *out, fg_error *error)
{
#ifdef _WIN32
    wchar_t *directory = _wgetcwd(NULL, 0);
    size_t i;
    fg_status status = FG_OK;
    if (!directory)
        return fg_fail(error, errno == ENOMEM ? FG_NOMEM : FG_IO, "Cannot get current directory");
    for (i = 0; directory[i]; ++i) {
        uint32_t cp = (uint32_t)directory[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            uint32_t low = (uint32_t)directory[++i];
            if (low < 0xdc00 || low > 0xdfff) {
                status = fg_fail(error, FG_IO, "Invalid Unicode in current directory");
                break;
            }
            cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            status = fg_fail(error, FG_IO, "Invalid Unicode in current directory");
            break;
        }
        status = fg_utf8_append(out, cp, error);
        if (status != FG_OK) break;
    }
    free(directory);
    return status;
#else
    size_t capacity = 256;
    for (;;) {
        char *directory = (char *)malloc(capacity);
        int saved_errno;
        fg_status status;
        if (!directory) return fg_fail(error, FG_NOMEM, "Cannot allocate current directory");
        if (getcwd(directory, capacity)) {
            status = fg_buffer_set(out, FG_STR(directory), error);
            free(directory);
            return status;
        }
        saved_errno = errno;
        free(directory);
        if (saved_errno != ERANGE)
            return fg_fail(error, FG_IO, "Cannot get current directory");
        if (capacity > SIZE_MAX / 2)
            return fg_fail(error, FG_LIMIT, "Current directory path too long");
        capacity *= 2;
    }
#endif
}

static int fg_path_separator(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

static fg_status fg_path_root(fg_str path, size_t *root, fg_error *error)
{
#ifdef _WIN32
    if (path.len >= 3 && ((path.data[0] >= 'A' && path.data[0] <= 'Z') ||
                         (path.data[0] >= 'a' && path.data[0] <= 'z')) &&
        path.data[1] == ':' && fg_path_separator(path.data[2])) {
        *root = 3;
        return FG_OK;
    }
    if (path.len >= 2 && fg_path_separator(path.data[0]) && fg_path_separator(path.data[1])) {
        size_t i = 2, start;
        while (i < path.len && !fg_path_separator(path.data[i])) ++i;
        if (i == 2 || i == path.len)
            return fg_fail(error, FG_INVALID, "UNC path requires server and share");
        start = ++i;
        while (i < path.len && !fg_path_separator(path.data[i])) ++i;
        if (i == start) return fg_fail(error, FG_INVALID, "UNC path requires share");
        *root = i;
        return FG_OK;
    }
#else
    if (path.len && path.data[0] == '/') { *root = 1; return FG_OK; }
#endif
    return fg_fail(error, FG_INVALID, "Environment search directory must be absolute");
}

fg_status fg_env_load_upward(fg_str directory, size_t max_levels, fg_env **out, fg_error *error)
{
    fg_buffer path = {0}, candidate = {0}, contents = {0};
    fg_status status;
    size_t root, level;
    if (!out) return fg_fail(error, FG_INVALID, "Missing environment output");
    *out = NULL;
    status = fg_utf8_validate(directory, 0, error);
    if (status != FG_OK) return status;
    if (directory.len) status = fg_buffer_set(&path, directory, error);
    else status = fg_current_directory(&path, error);
    if (status != FG_OK) goto finish;
    status = fg_path_root(fg_view(&path), &root, error);
    if (status != FG_OK) goto finish;
    while (path.len > root && fg_path_separator(path.data[path.len - 1])) --path.len;
    path.data[path.len] = 0;
    if (!max_levels) max_levels = 8;
    for (level = 0; level < max_levels; ++level) {
        status = fg_buffer_set(&candidate, fg_view(&path), error);
        if (status == FG_OK && !fg_path_separator(path.data[path.len - 1]))
#ifdef _WIN32
            status = fg_buffer_append(&candidate, "\\", 1, error);
#else
            status = fg_buffer_append(&candidate, "/", 1, error);
#endif
        if (status == FG_OK) status = fg_buffer_append(&candidate, ".env", 4, error);
        if (status != FG_OK) goto finish;
        status = fg_read_file(fg_view(&candidate), 4u * 1024u * 1024u, &contents, error);
        if (status == FG_OK) {
            status = fg_env_parse(fg_view(&contents), out, error);
            goto finish;
        }
        if (status != FG_NOT_FOUND) goto finish;
        fg_buffer_free(&contents);
        if (path.len == root) break;
        while (path.len > root && !fg_path_separator(path.data[path.len - 1])) --path.len;
        while (path.len > root && fg_path_separator(path.data[path.len - 1])) --path.len;
        path.data[path.len] = 0;
    }
    status = fg_fail(error, FG_NOT_FOUND, "No .env within environment search depth");
finish:
    fg_buffer_free(&contents);
    fg_buffer_free(&candidate);
    fg_buffer_free(&path);
    return status;
}

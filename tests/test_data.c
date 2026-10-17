#include "test.h"
#include "fg_data.h"

#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int test_json_strings(void)
{
    const char *source =
        "{\"z\":null,\"a\":[true,false,\"\\\"\\\\\\/\\b\\f\\n\\r\\t\","
        "\"\\u0041\\u00df\\u6771\\uD83D\\uDE00\",\"a\\u0000b\"],\"\":{},\"a\\u0000b\":7}";
    const char unicode[] = "A\xc3\x9f\xe6\x9d\xb1\xf0\x9f\x98\x80";
    const char nul_key[] = {'a', 0, 'b'};
    const fg_json *array, *value;
    fg_json *root = NULL, *again = NULL;
    fg_error error = {0};
    fg_buffer output = {0}, second = {0};
    fg_str string, key = {nul_key, sizeof(nul_key)};
    int boolean = -1;
    OK(fg_json_parse(FG_STR(source), &root, &error));
    CHECK(fg_json_type(root) == FG_JSON_OBJECT && fg_json_size(root) == 4);
    CHECK(fg_str_equal(fg_json_key(root, 0), FG_STR("")));
    CHECK(fg_str_equal(fg_json_key(root, 1), FG_STR("a")));
    CHECK(fg_str_equal(fg_json_key(root, 2), key));
    CHECK(fg_json_type(fg_json_get(root, FG_STR("z"))) == FG_JSON_NULL);
    CHECK(fg_json_get(root, FG_STR("missing")) == NULL);
    CHECK(fg_json_get(root, key) != NULL);
    array = fg_json_get(root, FG_STR("a"));
    CHECK(fg_json_size(array) == 5);
    OK(fg_json_bool(fg_json_at(array, 0), &boolean, &error));
    CHECK(boolean == 1);
    OK(fg_json_bool(fg_json_at(array, 1), &boolean, &error));
    CHECK(boolean == 0);
    CHECK(fg_str_equal(fg_json_string(fg_json_at(array, 2)), FG_STR("\"\\/\b\f\n\r\t")));
    CHECK(fg_str_equal(fg_json_string(fg_json_at(array, 3)), FG_STR(unicode)));
    value = fg_json_at(array, 4);
    string = fg_json_string(value);
    CHECK(string.len == 3 && !memcmp(string.data, nul_key, 3));
    CHECK(fg_json_at(array, 5) == NULL && fg_json_at(root, 0) == NULL);
    CHECK(fg_json_get(array, FG_STR("a")) == NULL);
    CHECK(fg_json_key(root, 99).data == NULL && fg_json_string(root).data == NULL);
    CHECK(fg_json_bool(value, &boolean, &error) == FG_INVALID);
    OK(fg_json_serialize(root, &output, &error));
    CHECK(strstr(output.data, "a\\u0000b") != NULL);
    OK(fg_json_parse((fg_str){output.data, output.len}, &again, &error));
    OK(fg_json_serialize(again, &second, &error));
    CHECK(fg_str_equal((fg_str){output.data, output.len}, (fg_str){second.data, second.len}));
    fg_json_free(root);
    fg_json_free(again);
    fg_buffer_free(&output);
    fg_buffer_free(&second);
    {
        char text[] = {'[', '"', 'x', '"', ']'};
        OK(fg_json_parse((fg_str){text, sizeof(text)}, &root, &error));
        memset(text, '?', sizeof(text));
        CHECK(fg_str_equal(fg_json_string(fg_json_at(root, 0)), FG_STR("x")));
        fg_json_free(root);
    }
    return 0;
}

static int test_json_invalid(void)
{
    static const char *invalid[] = {
        "", " ", "+1", "01", "-01", "-", ".1", "1.", "1e", "1e+", "1e-",
        "NaN", "Infinity", "truex", "tru", "False", "nul", "null null",
        "[1,]", "[,1]", "[1 2]", "{\"a\":1,}", "{a:1}", "{\"a\" 1}",
        "{\"a\":}", "{\"a\":1 \"b\":2}", "[", "{", "\"", "\"abc",
        "\"\\", "\"\\x\"", "\"\\u000\"", "\"\\u00gg\"", "\"\\uD800\"",
        "\"\\uDC00\"", "\"\\uD800\\u0041\"", "\"\\uD800\\uD800\"",
        "\"\\uDBFF\\uE000\"", "\"raw\nline\"", "\"raw\tline\"",
        "{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}",
        "{\"a\\u0000b\":1,\"a\\u0000b\":2}", "\vnull", "null\f", "\xef\xbb\xbf{}",
        "\"\xc0\xaf\"", "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"",
        "\"\xf0\x80\x80\x80\"", "\"\x80\"", "\"\xc2\"", "\"\xe2\x82\""
    };
    size_t i;
    fg_error error;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        fg_json *value = NULL;
        memset(&error, 0, sizeof(error));
        CHECK(fg_json_parse(FG_STR(invalid[i]), &value, &error) == FG_PARSE);
        CHECK(value == NULL && error.code == FG_PARSE && error.message[0]);
    }
    {
        const char embedded[] = {'"', 'x', 0, '"'};
        fg_json *value = NULL;
        CHECK(fg_json_parse((fg_str){embedded, sizeof(embedded)}, &value, NULL) == FG_PARSE);
        CHECK(fg_json_parse((fg_str){NULL, 5}, &value, &error) == FG_INVALID);
        CHECK(fg_json_parse(FG_STR("null"), NULL, &error) == FG_INVALID);
        CHECK(fg_json_parse((fg_str){NULL, 0}, &value, &error) == FG_PARSE);
    }
    return 0;
}

static int test_json_numbers(void)
{
    static const struct { const char *text; int64_t expected; } integers[] = {
        {"0", 0}, {"-0", 0}, {"42", 42}, {"-42", -42},
        {"1700000000123456", INT64_C(1700000000123456)},
        {"9007199254740993", INT64_C(9007199254740993)},
        {"9223372036854775807", INT64_MAX}, {"-9223372036854775808", INT64_MIN},
        {"9.223372036854775807e18", INT64_MAX},
        {"-92233720368547758080e-1", INT64_MIN},
        {"1.2300e2", 123}, {"100000000000000000000000000000000000000e-38", 1},
        {"0.000000000000000000000000000000001e33", 1},
        {"1000.000", 1000}, {"0e999999999999999999999", 0}
    };
    static const struct { const char *text; fg_status status; } invalid_i64[] = {
        {"9223372036854775808", FG_LIMIT}, {"-9223372036854775809", FG_LIMIT},
        {"18446744073709551616", FG_LIMIT}, {"1.1", FG_INVALID},
        {"1e-1", FG_INVALID}, {"1001e-3", FG_INVALID},
        {"1e99999999999999999999999999", FG_LIMIT},
        {"1e-99999999999999999999999999", FG_INVALID}
    };
    static const struct { const char *text; double expected; } doubles[] = {
        {"0", 0}, {"-0.0", -0.0}, {"0.1", 0.1}, {"-123.125", -123.125},
        {"1.23456789e12", 1.23456789e12}, {"1e-300", 1e-300}, {"1e300", 1e300},
        {"2.2250738585072014e-308", DBL_MIN},
        {"1.7976931348623157e308", DBL_MAX},
        {"9007199254740993", 9007199254740992.0},
        {"1.00000000000000011102230246251565404236316680908203125", 1.0},
        {"1.000000000000000111022302462515654042363166809082031251", 0x1.0000000000001p0}
    };
    fg_json *value = NULL;
    fg_error error = {0};
    size_t i;
    int64_t signed_number;
    uint64_t unsigned_number;
    double number;
    fg_buffer output = {0};
    for (i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        OK(fg_json_parse(FG_STR(integers[i].text), &value, &error));
        OK(fg_json_i64(value, &signed_number, &error));
        CHECK(signed_number == integers[i].expected);
        CHECK(fg_str_equal(fg_json_number_text(value), FG_STR(integers[i].text)));
        OK(fg_json_serialize(value, &output, &error));
        CHECK(fg_str_equal((fg_str){output.data, output.len}, FG_STR(integers[i].text)));
        fg_json_free(value);
    }
    for (i = 0; i < sizeof(invalid_i64) / sizeof(invalid_i64[0]); ++i) {
        OK(fg_json_parse(FG_STR(invalid_i64[i].text), &value, &error));
        signed_number = 17;
        CHECK(fg_json_i64(value, &signed_number, &error) == invalid_i64[i].status);
        CHECK(signed_number == 17 && error.message[0]);
        fg_json_free(value);
    }
    OK(fg_json_parse(FG_STR("18446744073709551615"), &value, &error));
    OK(fg_json_u64(value, &unsigned_number, &error));
    CHECK(unsigned_number == UINT64_MAX);
    fg_json_free(value);
    OK(fg_json_parse(FG_STR("-1"), &value, &error));
    CHECK(fg_json_u64(value, &unsigned_number, &error) == FG_LIMIT);
    fg_json_free(value);
    for (i = 0; i < sizeof(doubles) / sizeof(doubles[0]); ++i) {
        OK(fg_json_parse(FG_STR(doubles[i].text), &value, &error));
        OK(fg_json_double(value, &number, &error));
        CHECK(number == doubles[i].expected);
        fg_json_free(value);
    }
    OK(fg_json_parse(FG_STR("-0"), &value, &error));
    OK(fg_json_double(value, &number, &error));
    CHECK(signbit(number));
    fg_json_free(value);
    OK(fg_json_parse(FG_STR("5e-324"), &value, &error));
    OK(fg_json_double(value, &number, &error));
    CHECK(number > 0 && number < DBL_MIN);
    fg_json_free(value);
    OK(fg_json_parse(FG_STR("1e309"), &value, &error));
    CHECK(fg_json_double(value, &number, &error) == FG_LIMIT);
    fg_json_free(value);
    OK(fg_json_parse(FG_STR("1e-99999999"), &value, &error));
    CHECK(fg_json_double(value, &number, &error) == FG_LIMIT);
    fg_json_free(value);
    OK(fg_json_parse(FG_STR("\"123\""), &value, &error));
    CHECK(fg_json_i64(value, &signed_number, &error) == FG_INVALID);
    CHECK(fg_json_double(value, &number, &error) == FG_INVALID);
    CHECK(fg_json_number_text(value).data == NULL);
    fg_json_free(value);
    fg_buffer_free(&output);
    return 0;
}

static int test_json_float_rounding(void)
{
    uint32_t state = 17;
    size_t iteration;
    fg_error error = {0};
    /* The C runtime starts in the C locale. It is only an independent test
       oracle here; the implementation never uses its numeric parser. */
    for (iteration = 0; iteration < 2000; ++iteration) {
        char text[100];
        size_t i, digits = iteration % 45 + 1, length = 0;
        int exponent;
        double expected, actual = 0;
        fg_json *value = NULL;
        fg_status status;
        if (iteration & 1) text[length++] = '-';
        state = state * 1664525u + 1013904223u;
        text[length++] = (char)('1' + state % 9);
        text[length++] = '.';
        for (i = 0; i < digits; ++i) {
            state = state * 1664525u + 1013904223u;
            text[length++] = (char)('0' + state % 10);
        }
        exponent = (int)(state % 701) - 350;
        (void)snprintf(text + length, sizeof(text) - length, "e%d", exponent);
        expected = strtod(text, NULL);
        OK(fg_json_parse(FG_STR(text), &value, &error));
        status = fg_json_double(value, &actual, &error);
        if (!isfinite(expected) || expected == 0) CHECK(status == FG_LIMIT);
        else {
            CHECK(status == FG_OK);
            CHECK(actual == expected);
        }
        fg_json_free(value);
    }
    {
        const char *half = "1.00000000000000011102230246251565404236316680908203125";
        char text[1204];
        size_t length = strlen(half);
        double number;
        fg_json *value = NULL;
        memcpy(text, half, length);
        memset(text + length, '0', sizeof(text) - length - 2);
        text[sizeof(text) - 2] = '1';
        text[sizeof(text) - 1] = 0;
        OK(fg_json_parse(FG_STR(text), &value, &error));
        OK(fg_json_double(value, &number, &error));
        CHECK(number == 0x1.0000000000001p0);
        fg_json_free(value);
    }
    return 0;
}

static int test_json_construction(void)
{
    fg_json *object = NULL, *array = NULL, *value = NULL, *other = NULL;
    fg_buffer output = {0};
    fg_error error = {0};
    const char nul[] = {'x', 0, 'y'};
    OK(fg_json_new(FG_JSON_OBJECT, &object, &error));
    OK(fg_json_new(FG_JSON_ARRAY, &array, &error));
    OK(fg_json_new_i64(INT64_MIN, &value, &error));
    OK(fg_json_append(array, value, &error));
    CHECK(fg_json_append(array, value, &error) == FG_INVALID);
    OK(fg_json_new_u64(UINT64_MAX, &value, &error));
    OK(fg_json_append(array, value, &error));
    OK(fg_json_set(object, FG_STR("z"), array, &error));
    CHECK(fg_json_append(array, object, &error) == FG_INVALID);
    CHECK(fg_json_set(object, FG_STR("self"), object, &error) == FG_INVALID);
    OK(fg_json_new_bool(7, &value, &error));
    OK(fg_json_set(object, FG_STR("a"), value, &error));
    OK(fg_json_new_string((fg_str){nul, sizeof(nul)}, &value, &error));
    OK(fg_json_set(object, FG_STR("a"), value, &error));
    OK(fg_json_new(FG_JSON_NULL, &value, &error));
    OK(fg_json_set(object, FG_STR("empty"), value, &error));
    OK(fg_json_serialize(object, &output, &error));
    CHECK(!strcmp(output.data,
        "{\"a\":\"x\\u0000y\",\"empty\":null,\"z\":[-9223372036854775808,18446744073709551615]}"));
    CHECK(fg_json_new_number(FG_STR("01"), &other, &error) == FG_PARSE && other == NULL);
    CHECK(fg_json_new_number(FG_STR(" 1"), &other, &error) == FG_PARSE);
    CHECK(fg_json_new(FG_JSON_NUMBER, &other, &error) == FG_INVALID);
    CHECK(fg_json_new_string(FG_STR("\xed\xa0\x80"), &other, &error) == FG_PARSE);
    OK(fg_json_new_string(FG_STR(""), &other, &error));
    CHECK(fg_json_string(other).data != NULL);
    CHECK(fg_json_set(array, FG_STR("bad"), other, &error) == FG_INVALID);
    CHECK(fg_json_append(object, other, &error) == FG_INVALID);
    CHECK(fg_json_set(object, FG_STR("\x80"), other, &error) == FG_PARSE);
    fg_json_free(other);
    fg_json_free(object);
    fg_buffer_free(&output);
    return 0;
}

static int test_json_depth_and_prefixes(void)
{
    char text[FG_JSON_MAX_DEPTH * 2 + 2];
    fg_json *root = NULL, *value = NULL, *parent = NULL;
    fg_error error = {0};
    size_t i, n = FG_JSON_MAX_DEPTH - 1;
    memset(text, '[', n);
    text[n] = '0';
    memset(text + n + 1, ']', n);
    OK(fg_json_parse((fg_str){text, 2 * n + 1}, &root, &error));
    fg_json_free(root);
    n = FG_JSON_MAX_DEPTH;
    memset(text, '[', n);
    text[n] = '0';
    memset(text + n + 1, ']', n);
    CHECK(fg_json_parse((fg_str){text, 2 * n + 1}, &root, &error) == FG_LIMIT && !root);
    OK(fg_json_new(FG_JSON_ARRAY, &root, &error));
    parent = root;
    for (i = 1; i < FG_JSON_MAX_DEPTH; ++i) {
        OK(fg_json_new(FG_JSON_ARRAY, &value, &error));
        OK(fg_json_append(parent, value, &error));
        parent = value;
    }
    OK(fg_json_new(FG_JSON_NULL, &value, &error));
    CHECK(fg_json_append(parent, value, &error) == FG_LIMIT);
    fg_json_free(value);
    fg_json_free(root);
    {
        const char document[] = "{\"key\":[\"\\uD83D\\uDE00\",1.2e-3,true,false,null]}";
        for (i = 0; i < sizeof(document) - 2; ++i) {
            CHECK(fg_json_parse((fg_str){document, i}, &root, &error) == FG_PARSE);
            CHECK(root == NULL);
        }
    }
    return 0;
}

typedef struct csv_check {
    size_t rows;
    fg_csv_header *header;
} csv_check;

static fg_status csv_check_row(void *user, const fg_str *fields, size_t count,
                               size_t row_index, fg_error *error)
{
    csv_check *state = (csv_check *)user;
    fg_str value;
    size_t column;
    if (row_index != state->rows++) return fg_fail(error, FG_INVALID, "CSV row index mismatch");
    if (row_index == 0) return fg_csv_header_create(fields, count, &state->header, error);
    if (count != 3) return fg_fail(error, FG_INVALID, "Expected three CSV columns");
    if (fg_csv_column(state->header, FG_STR("description"), &column, error) != FG_OK)
        return FG_INVALID;
    if (fg_csv_field(fields, count, column, &value, error) != FG_OK) return FG_INVALID;
    if (row_index == 1) {
        if (!fg_str_equal(fields[0], FG_STR("A,B")) || !fg_str_equal(value, FG_STR("first\r\nsecond")) ||
            !fg_str_equal(fields[2], FG_STR("say \"hi\"")))
            return fg_fail(error, FG_INVALID, "Multiline CSV data mismatch");
    } else if (row_index == 2) {
        if (!fg_str_equal(fields[0], FG_STR("X")) || value.len || fields[2].len)
            return fg_fail(error, FG_INVALID, "Empty CSV fields mismatch");
    }
    return FG_OK;
}

static fg_status csv_count_row(void *user, const fg_str *fields, size_t count,
                               size_t row_index, fg_error *error)
{
    size_t *rows = (size_t *)user;
    (void)fields; (void)count; (void)row_index; (void)error;
    ++*rows;
    return FG_OK;
}

static fg_status csv_stop(void *user, const fg_str *fields, size_t count,
                          size_t row_index, fg_error *error)
{
    (void)user; (void)fields; (void)count; (void)row_index;
    return fg_fail(error, FG_LIMIT, "Intentional callback stop");
}

static int test_csv(void)
{
    const char text[] =
        "\xef\xbb\xbf" "symbol,description,note\r\n\r\n"
        "\"A,B\",\"first\r\nsecond\",\"say \"\"hi\"\"\"\r\nX,,";
    static const char *bad[] = {"\"open", "a\"b", "\"a\"x", "\"a\" ,b", "\"\"\"", "a\rb", "\"a\"\r"};
    fg_error error = {0};
    csv_check state = {0};
    size_t rows = 0, column = 99, i;
    fg_csv_header *duplicate = NULL;
    fg_str fields[] = {{"x", 1}, {"x", 1}};
    fg_str result;
    OK(fg_csv_parse((fg_str){text, sizeof(text) - 1}, csv_check_row, &state, &error));
    CHECK(state.rows == 3);
    OK(fg_csv_column(state.header, FG_STR("symbol"), &column, &error));
    CHECK(column == 0);
    CHECK(fg_csv_column(state.header, FG_STR("absent"), &column, &error) == FG_NOT_FOUND);
    CHECK(fg_csv_field(fields, 2, 2, &result, &error) == FG_NOT_FOUND);
    CHECK(fg_csv_header_create(fields, 2, &duplicate, &error) == FG_PARSE && !duplicate);
    fg_csv_header_free(state.header);
    OK(fg_csv_parse(FG_STR("\n\n\"\"\n,\na,b\n"), csv_count_row, &rows, &error));
    CHECK(rows == 3);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(fg_csv_parse(FG_STR(bad[i]), csv_count_row, &rows, &error) == FG_PARSE);
        CHECK(error.message[0] && strstr(error.message, "byte"));
    }
    CHECK(fg_csv_parse(FG_STR("a,b\nx,y"), csv_stop, NULL, &error) == FG_LIMIT);
    CHECK(!strcmp(error.message, "Intentional callback stop"));
    rows = 0;
    OK(fg_csv_parse((fg_str){NULL, 0}, csv_count_row, &rows, &error));
    CHECK(rows == 0);
    CHECK(fg_csv_parse(FG_STR("x"), NULL, NULL, &error) == FG_INVALID);
    CHECK(fg_csv_parse((fg_str){"a\0b", 3}, csv_count_row, &rows, &error) == FG_PARSE);
    CHECK(fg_csv_parse(FG_STR("\xc0\xaf"), csv_count_row, &rows, &error) == FG_PARSE);
    return 0;
}

static int test_env(void)
{
    const char text[] = "\xef\xbb\xbf"
        " # comment\r\n\r\nA=first\n B = '  two # = three  ' \r\n"
        "C=\"quoted\\n literal\" # comment\nEMPTY=\nA=last\n_9=raw#hash\n"
        "UNICODE=\xe6\x9d\xb1\nEQUAL=a=b\n";
    static const char *bad[] = {
        "NO_EQUALS", "=x", "9KEY=x", "A B=x", "A-B=x", "export A=x",
        "A=\"open", "A='open", "A=\"x\"junk", "A=x\"y", "A=x'y", "A=x\rB=y",
        "A=\x01", "A=\xed\xa0\x80"
    };
    fg_env *env = NULL;
    fg_error error = {0};
    fg_str value;
    size_t i;
    OK(fg_env_parse((fg_str){text, sizeof(text) - 1}, &env, &error));
    CHECK(fg_env_size(env) == 7);
    OK(fg_env_get(env, FG_STR("A"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("last")));
    OK(fg_env_get(env, FG_STR("B"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("  two # = three  ")));
    OK(fg_env_get(env, FG_STR("C"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("quoted\\n literal")));
    OK(fg_env_get(env, FG_STR("EMPTY"), &value, &error));
    CHECK(value.data && !value.len);
    OK(fg_env_get(env, FG_STR("_9"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("raw#hash")));
    OK(fg_env_get(env, FG_STR("EQUAL"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("a=b")));
    CHECK(fg_env_get(env, FG_STR("missing"), &value, &error) == FG_NOT_FOUND);
    fg_env_free(env);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(fg_env_parse(FG_STR(bad[i]), &env, &error) == FG_PARSE);
        CHECK(env == NULL && error.message[0]);
    }
    CHECK(fg_env_parse((fg_str){"A=x\0y", 5}, &env, &error) == FG_PARSE);
    OK(fg_env_parse((fg_str){NULL, 0}, &env, &error));
    CHECK(fg_env_size(env) == 0);
    fg_env_free(env);
    CHECK(fg_env_load_upward(FG_STR("relative-path"), 1, &env, &error) == FG_INVALID);
    return 0;
}

static int test_mutated_inputs(void)
{
    static const char seed[] =
        "{\"unicode\":\"\\uD83D\\uDE00\",\"values\":[null,true,false,-2.5e2],\"empty\":\"\"}";
    char input[sizeof(seed)];
    fg_error error = {0};
    size_t position, byte;
    for (position = 0; position < sizeof(seed) - 1; ++position) {
        for (byte = 0; byte < 256; ++byte) {
            fg_json *json = NULL, *again = NULL;
            fg_env *env = NULL;
            fg_buffer output = {0}, second = {0};
            size_t rows = 0;
            fg_status status;
            memcpy(input, seed, sizeof(seed));
            input[position] = (char)byte;
            status = fg_json_parse((fg_str){input, sizeof(seed) - 1}, &json, &error);
            if (status == FG_OK) {
                OK(fg_json_serialize(json, &output, &error));
                OK(fg_json_parse((fg_str){output.data, output.len}, &again, &error));
                OK(fg_json_serialize(again, &second, &error));
                CHECK(fg_str_equal((fg_str){output.data, output.len}, (fg_str){second.data, second.len}));
            } else CHECK(json == NULL && error.message[0]);
            fg_json_free(json);
            fg_json_free(again);
            fg_buffer_free(&output);
            fg_buffer_free(&second);
            (void)fg_csv_parse((fg_str){input, sizeof(seed) - 1}, csv_count_row, &rows, &error);
            (void)fg_env_parse((fg_str){input, sizeof(seed) - 1}, &env, &error);
            fg_env_free(env);
        }
    }
    return 0;
}

static int test_locale(void)
{
    static const char *candidates[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "German_Germany.1252"};
    const char *current = setlocale(LC_NUMERIC, NULL);
    char *saved = NULL;
    size_t i;
    fg_json *value = NULL;
    fg_buffer output = {0};
    fg_error error = {0};
    double number;
    if (current) {
        saved = (char *)malloc(strlen(current) + 1);
        CHECK(saved != NULL);
        memcpy(saved, current, strlen(current) + 1);
    }
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
        if (setlocale(LC_NUMERIC, candidates[i])) break;
    OK(fg_json_parse(FG_STR("1234.125"), &value, &error));
    OK(fg_json_double(value, &number, &error));
    CHECK(number == 1234.125);
    OK(fg_json_serialize(value, &output, &error));
    CHECK(!strcmp(output.data, "1234.125"));
    fg_json_free(value);
    fg_buffer_free(&output);
    if (saved) { CHECK(setlocale(LC_NUMERIC, saved) != NULL); free(saved); }
    return 0;
}

static int fixture_path(const char *directory, const char *relative, fg_buffer *out)
{
    fg_error error = {0};
    OK(fg_buffer_set(out, FG_STR(directory), &error));
#ifdef _WIN32
    OK(fg_buffer_append(out, "\\", 1, &error));
#else
    OK(fg_buffer_append(out, "/", 1, &error));
#endif
    OK(fg_buffer_append(out, relative, strlen(relative), &error));
    return 0;
}

static int test_fixtures(const char *directory)
{
    fg_buffer path = {0}, data = {0};
    fg_error error = {0};
    fg_json *json = NULL;
    fg_env *env = NULL;
    fg_str value;
    uint64_t integer;
    size_t rows = 0;
    CHECK(fixture_path(directory, "numbers.json", &path) == 0);
    OK(fg_read_file((fg_str){path.data, path.len}, 1024 * 1024, &data, &error));
    OK(fg_json_parse((fg_str){data.data, data.len}, &json, &error));
    OK(fg_json_u64(fg_json_get(json, FG_STR("trade_id")), &integer, &error));
    CHECK(integer == UINT64_MAX);
    fg_json_free(json);
    CHECK(fixture_path(directory, "records.csv", &path) == 0);
    OK(fg_read_file((fg_str){path.data, path.len}, 1024 * 1024, &data, &error));
    OK(fg_csv_parse((fg_str){data.data, data.len}, csv_count_row, &rows, &error));
    CHECK(rows == 3);
    CHECK(fixture_path(directory, "nested", &path) == 0);
    CHECK(fg_env_load_upward((fg_str){path.data, path.len}, 1, &env, &error) == FG_NOT_FOUND && !env);
    OK(fg_env_load_upward((fg_str){path.data, path.len}, 2, &env, &error));
    OK(fg_env_get(env, FG_STR("FG_SYNTHETIC_TEST"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("fixture-only")));
    fg_env_free(env);
    CHECK(fixture_path(directory, "malformed", &path) == 0);
    CHECK(fg_env_load_upward((fg_str){path.data, path.len}, 2, &env, &error) == FG_PARSE && !env);
    CHECK(fixture_path(directory, "utf8-\xc2\xb5", &path) == 0);
    OK(fg_env_load_upward((fg_str){path.data, path.len}, 1, &env, &error));
    OK(fg_env_get(env, FG_STR("FG_SYNTHETIC_UTF8"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("unicode-path")));
    fg_env_free(env);
    /* This test process alone changes cwd, into a directory containing only
       synthetic fixtures. The depth bound prevents consulting user files. */
#ifdef _WIN32
    CHECK(_chdir(directory) == 0);
#else
    CHECK(chdir(directory) == 0);
#endif
    OK(fg_env_load_upward((fg_str){NULL, 0}, 1, &env, &error));
    OK(fg_env_get(env, FG_STR("FG_SYNTHETIC_TEST"), &value, &error));
    CHECK(fg_str_equal(value, FG_STR("fixture-only")));
    fg_env_free(env);
    fg_buffer_free(&path);
    fg_buffer_free(&data);
    return 0;
}

int main(int argc, char **argv)
{
    (void)fg_fixture_get;
    if (argc != 2) {
        fputs("Usage: test_data <absolute tests/fixtures/core directory>\n", stderr);
        return 2;
    }
    CHECK(test_json_strings() == 0);
    CHECK(test_json_invalid() == 0);
    CHECK(test_json_numbers() == 0);
    CHECK(test_json_float_rounding() == 0);
    CHECK(test_json_construction() == 0);
    CHECK(test_json_depth_and_prefixes() == 0);
    CHECK(test_csv() == 0);
    CHECK(test_env() == 0);
    CHECK(test_mutated_inputs() == 0);
    CHECK(test_locale() == 0);
    CHECK(test_fixtures(argv[1]) == 0);
    puts("JSON, CSV, and environment tests passed");
    return 0;
}

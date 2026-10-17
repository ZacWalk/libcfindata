#ifndef FG_SEC_INTERNAL_H
#define FG_SEC_INTERNAL_H
#include "fg_sec.h"

typedef struct fg_html_tag {
    fg_str name, attrs;
    size_t start, end;
    int closing, self_closing, special;
} fg_html_tag;
int fg_sec_iequal(fg_str a, fg_str b);
fg_str fg_sec_slice(fg_str s, size_t begin, size_t end);
fg_str fg_sec_trim(fg_str s);
int fg_sec_space(unsigned char c);
fg_status fg_sec_utf8(fg_str s, fg_error *e);
/* pos points at '<'; end points just past '>'. Quote/comment aware. */
fg_status fg_html_tag_read(fg_str html, size_t pos, fg_html_tag *tag, fg_error *e);
fg_str fg_html_attr(const fg_html_tag *tag, const char *name, int *present);
int fg_html_is(const fg_html_tag *tag, const char *name);
int fg_html_local_is(const fg_html_tag *tag, const char *name);
fg_status fg_html_close(fg_str html, const fg_html_tag *tag,
                       size_t *content_end, size_t *after, fg_error *e);
fg_status fg_html_entities(fg_str text, fg_buffer *out, fg_error *e);
fg_status fg_html_text(fg_str text, fg_buffer *out, fg_error *e);
fg_status fg_sec_date(fg_str date, int64_t *out, fg_error *e);
fg_status fg_sec_set_string(fg_json *obj, const char *key, fg_str text, fg_error *e);
fg_status fg_sec_set_i64(fg_json *obj, const char *key, int64_t value, fg_error *e);
fg_status fg_sec_set_value(fg_json *obj, const char *key, fg_json *value, fg_error *e);
#endif

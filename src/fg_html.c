#include "fg_sec_internal.h"
#include <stdlib.h>
#include <string.h>

static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a'-'A')) : c;
}
int fg_sec_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
int fg_sec_iequal(fg_str a, fg_str b)
{
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; ++i)
        if (lower((unsigned char)a.data[i]) != lower((unsigned char)b.data[i])) return 0;
    return 1;
}
fg_str fg_sec_slice(fg_str s, size_t a, size_t b)
{
    fg_str r = {s.data ? s.data+a : NULL, b-a};
    return r;
}
fg_str fg_sec_trim(fg_str s)
{
    while (s.len && fg_sec_space((unsigned char)s.data[0])) { ++s.data; --s.len; }
    while (s.len && fg_sec_space((unsigned char)s.data[s.len-1])) --s.len;
    return s;
}
fg_status fg_sec_utf8(fg_str s, fg_error *e)
{
    size_t i = 0;
    if (!s.data && s.len) return fg_fail(e, FG_INVALID, "Missing HTML input");
    while (i < s.len) {
        unsigned c = (unsigned char)s.data[i++], cp, min;
        size_t n;
        if (!c) return fg_fail(e, FG_PARSE, "NUL in HTML input");
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; n = 1; min = 128; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; n = 2; min = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; n = 3; min = 65536; }
        else return fg_fail(e, FG_PARSE, "Invalid HTML UTF-8");
        if (n > s.len-i) return fg_fail(e, FG_PARSE, "Truncated HTML UTF-8");
        while (n--) {
            c = (unsigned char)s.data[i++];
            if ((c & 0xc0) != 0x80) return fg_fail(e, FG_PARSE, "Invalid HTML UTF-8 continuation");
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return fg_fail(e, FG_PARSE, "Invalid HTML Unicode scalar");
    }
    return FG_OK;
}
static int at(fg_str s, size_t i, const char *text)
{
    size_t n = strlen(text);
    return i <= s.len && n <= s.len-i && !memcmp(s.data+i, text, n);
}
static size_t find(fg_str s, size_t i, const char *text)
{
    for (; i < s.len; ++i) if (at(s, i, text)) return i;
    return s.len;
}
static int namechar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '-' || c == '.';
}
static int tag_start(fg_str s, size_t i)
{
    unsigned char c;
    if (i+1 >= s.len || s.data[i] != '<') return 0;
    c = (unsigned char)s.data[i+1];
    return (lower(c) >= 'a' && lower(c) <= 'z') || c == '!' || c == '?' || c == '/';
}
fg_status fg_html_tag_read(fg_str html, size_t pos, fg_html_tag *t, fg_error *e)
{
    size_t i = pos+1, start, attrs;
    char quote = 0;
    memset(t, 0, sizeof(*t)); t->start = pos;
    if (pos >= html.len || html.data[pos] != '<')
        return fg_fail(e, FG_PARSE, "Expected HTML tag");
    if (at(html, pos, "<!--")) {
        /* End where browsers end it: at the first "-->", whose dashes may be
           the opener's own ("<!-->", "<!--->": abrupt-closing-of-empty-comment),
           or at a "--!>" after the opener (incorrectly-closed-comment). */
        for (i = pos+2; i < html.len; ++i) {
            if (at(html, i, "-->")) { t->end = i+3; break; }
            if (i >= pos+4 && at(html, i, "--!>")) { t->end = i+4; break; }
        }
        if (i == html.len) return fg_fail(e, FG_PARSE, "Unterminated HTML comment");
        t->special = 1; return FG_OK;
    }
    if (i < html.len && (html.data[i] == '!' || html.data[i] == '?')) t->special = 1;
    if (i < html.len && html.data[i] == '/') { t->closing = 1; ++i; }
    start = i;
    while (i < html.len && namechar((unsigned char)html.data[i])) ++i;
    t->name = fg_sec_slice(html, start, i); attrs = i;
    if (!t->special && !t->name.len) return fg_fail(e, FG_PARSE, "Empty HTML tag name");
    for (; i < html.len; ++i) {
        char c = html.data[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '>') {
            size_t last = i;
            while (last > attrs && fg_sec_space((unsigned char)html.data[last-1])) --last;
            t->self_closing = last > attrs && html.data[last-1] == '/';
            t->attrs = fg_sec_slice(html, attrs, t->self_closing ? last-1 : i);
            t->end = i+1;
            return FG_OK;
        }
    }
    return fg_fail(e, FG_PARSE, "Unterminated HTML tag or attribute");
}
fg_str fg_html_attr(const fg_html_tag *t, const char *name, int *present)
{
    size_t i = 0;
    fg_str a = t->attrs, empty = {NULL,0};
    if (present) *present = 0;
    while (i < a.len) {
        size_t start, end, value, value_end;
        char quote;
        while (i < a.len && fg_sec_space((unsigned char)a.data[i])) ++i;
        start = i;
        while (i < a.len && namechar((unsigned char)a.data[i])) ++i;
        end = i;
        if (i == start) { ++i; continue; }
        while (i < a.len && fg_sec_space((unsigned char)a.data[i])) ++i;
        value = value_end = i;
        if (i < a.len && a.data[i] == '=') {
            ++i;
            while (i < a.len && fg_sec_space((unsigned char)a.data[i])) ++i;
            quote = i < a.len && (a.data[i] == '"' || a.data[i] == '\'') ? a.data[i++] : 0;
            value = i;
            while (i < a.len && (quote ? a.data[i] != quote : !fg_sec_space((unsigned char)a.data[i]))) ++i;
            value_end = i;
            if (quote && i < a.len) ++i;
        }
        if (fg_sec_iequal(fg_sec_slice(a, start, end), FG_STR(name))) {
            if (present) *present = 1;
            return fg_sec_slice(a, value, value_end);
        }
    }
    return empty;
}
int fg_html_is(const fg_html_tag *t, const char *name)
{
    return fg_sec_iequal(t->name, FG_STR(name));
}
int fg_html_local_is(const fg_html_tag *t, const char *name)
{
    size_t i;
    fg_str n = t->name;
    for (i = 0; i < n.len; ++i)
        if (n.data[i] == ':') return fg_sec_iequal(fg_sec_slice(n, i+1, n.len), FG_STR(name));
    return fg_sec_iequal(n, FG_STR(name));
}
fg_status fg_html_close(fg_str html, const fg_html_tag *t, size_t *end, size_t *after, fg_error *e)
{
    size_t i = t->end, depth = 1;
    int raw = fg_html_is(t, "script") || fg_html_is(t, "style");
    if (t->self_closing) { *end = *after = t->end; return FG_OK; }
    while (i < html.len) {
        fg_html_tag n;
        fg_status s;
        if (!tag_start(html, i)) { ++i; continue; }
        if (raw && !at(html, i, "</")) { ++i; continue; }
        if ((s = fg_html_tag_read(html, i, &n, e)) != FG_OK) {
            if (raw) { ++i; continue; }
            return s;
        }
        i = n.end;
        if (fg_sec_iequal(t->name, n.name)) {
            if (n.closing) {
                if (--depth == 0) { *end = n.start; *after = n.end; return FG_OK; }
            } else if (!n.self_closing && !raw) {
                if (++depth > 256) return fg_fail(e, FG_LIMIT, "HTML nesting limit");
            }
        } else if (!raw && !n.closing && (fg_html_is(&n, "script") || fg_html_is(&n, "style"))) {
            size_t ignored;
            if ((s = fg_html_close(html, &n, &ignored, &i, e)) != FG_OK) return s;
        }
    }
    return fg_fail(e, FG_PARSE, "Unterminated HTML element");
}
static fg_status codepoint(fg_buffer *out, unsigned cp, fg_error *e)
{
    char b[4];
    size_t n;
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp == 160) cp = ' ';
    if (cp < 128) { b[0] = (char)cp; n = 1; }
    else if (cp < 2048) { b[0]=(char)(0xc0|(cp>>6)); b[1]=(char)(0x80|(cp&63)); n=2; }
    else if (cp < 65536) {
        b[0]=(char)(0xe0|(cp>>12)); b[1]=(char)(0x80|((cp>>6)&63)); b[2]=(char)(0x80|(cp&63)); n=3;
    } else {
        b[0]=(char)(0xf0|(cp>>18)); b[1]=(char)(0x80|((cp>>12)&63));
        b[2]=(char)(0x80|((cp>>6)&63)); b[3]=(char)(0x80|(cp&63)); n=4;
    }
    return fg_buffer_append(out, b, n, e);
}
fg_status fg_html_entities(fg_str text, fg_buffer *out, fg_error *e)
{
    static const struct { const char *name; unsigned cp; } entities[] = {
        {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",160},
        {"mdash",8212},{"ndash",8211},{"bull",8226},{"trade",8482},{"reg",174},
        {"copy",169},{"sect",167},{"para",182},{"deg",176},{"lsquo",8216},
        {"rsquo",8217},{"ldquo",8220},{"rdquo",8221},{"hellip",8230},
        {"eacute",233},{"agrave",224},{"uuml",252},{"ouml",246},{"times",215},
        {"minus",8722},{"frac12",189},{"frac14",188},{"frac34",190},
        {"euro",8364},{"pound",163},{"yen",165},{"cent",162},{"plusmn",177},
        {"divide",247},{"middot",183},{"shy",173},{"thinsp",8201},{"ensp",8194},
        {"emsp",8195},{"laquo",171},{"raquo",187},{"micro",181},{"sup2",178},
        {"sup3",179},{"acute",180}
    };
    size_t i = 0;
    while (i < text.len) {
        size_t semi = i+1, j;
        unsigned cp = 0;
        int decoded = 0;
        fg_status s;
        if (text.data[i] == '&') {
            while (semi < text.len && semi-i < 40 && text.data[semi] != ';') ++semi;
            if (semi < text.len && text.data[semi] == ';') {
                fg_str name = fg_sec_slice(text, i+1, semi);
                if (name.len && name.data[0] == '#') {
                    unsigned base = 10;
                    int valid = 1, overflow = 0;
                    j = 1;
                    if (j < name.len && (name.data[j] == 'x' || name.data[j] == 'X')) { ++j; base=16; }
                    if (j == name.len) valid = 0;
                    for (; j < name.len; ++j) {
                        unsigned char c = lower((unsigned char)name.data[j]);
                        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c-'0') :
                                     c >= 'a' && c <= 'f' ? (unsigned)(c-'a'+10) : 99;
                        if (d >= base) { valid = 0; break; }
                        if (cp > (0x10ffff-d)/base) overflow = 1;
                        if (!overflow) cp = cp*base+d;
                    }
                    if (valid) { decoded = 1; if (overflow) cp = 0xfffd; }
                } else {
                    for (j = 0; j < sizeof(entities)/sizeof(entities[0]); ++j)
                        if (fg_str_equal(name, FG_STR(entities[j].name))) {
                            cp = entities[j].cp; decoded = 1; break;
                        }
                }
            }
        }
        if (decoded) { s = codepoint(out, cp, e); i = semi+1; }
        else if (at(text, i, "\xc2\xa0")) { s=fg_buffer_append(out," ",1,e); i+=2; }
        else { s = fg_buffer_append(out, text.data+i, 1, e); ++i; }
        if (s != FG_OK) return s;
    }
    return FG_OK;
}

static fg_status normalized(fg_str text, fg_buffer *out, int cell, fg_error *e)
{
    size_t i;
    int space = 0;
    for (i = 0; i < text.len; ++i) {
        fg_status s;
        if (fg_sec_space((unsigned char)text.data[i])) { space = 1; continue; }
        if (space && out->len && out->data[out->len-1] != ' ' &&
            out->data[out->len-1] != '\n') {
            if ((s = fg_buffer_append(out, " ", 1, e)) != FG_OK) return s;
        }
        space = 0;
        if (cell && text.data[i] == '|')
            if ((s = fg_buffer_append(out, "\\", 1, e)) != FG_OK) return s;
        if ((s = fg_buffer_append(out, text.data+i, 1, e)) != FG_OK) return s;
    }
    if (space && out->len && out->data[out->len-1] != ' ' && out->data[out->len-1] != '\n')
        return fg_buffer_append(out, " ", 1, e);
    return FG_OK;
}
fg_status fg_html_text(fg_str text, fg_buffer *out, fg_error *e)
{
    size_t i = 0;
    fg_buffer decoded = {0};
    fg_status s = FG_OK;
    while (i < text.len) {
        size_t start = i;
        fg_html_tag t;
        if (!tag_start(text, i)) {
            do { ++i; } while (i < text.len && !tag_start(text, i));
            s = fg_html_entities(fg_sec_slice(text, start, i), &decoded, e);
        } else {
            s = fg_html_tag_read(text, i, &t, e);
            if (s != FG_OK) break;
            i = t.end;
            if (!t.closing && (fg_html_is(&t, "ix:exclude") || fg_html_is(&t, "script") ||
                fg_html_is(&t, "style"))) {
                size_t end;
                s = fg_html_close(text, &t, &end, &i, e);
            } else if (fg_html_is(&t,"br") || fg_html_is(&t,"p") || fg_html_is(&t,"div"))
                s = fg_buffer_append(&decoded, " ", 1, e);
        }
        if (s != FG_OK) break;
    }
    if (s == FG_OK) s = normalized((fg_str){decoded.data,decoded.len}, out, 0, e);
    if (s == FG_OK && out->len && out->data[out->len-1] == ' ') out->data[--out->len] = 0;
    fg_buffer_free(&decoded);
    return s;
}

static int hidden(const fg_html_tag *t)
{
    fg_str style = fg_html_attr(t, "style", NULL);
    size_t i;
    int present;
    fg_html_attr(t, "hidden", &present);
    if (present) return 1;
    /* Compare CSS declarations after whitespace removal, not arbitrary substrings. */
    for (i = 0; i < style.len;) {
        char compact[128];
        size_t n = 0;
        while (i < style.len && style.data[i] != ';') {
            if (!fg_sec_space((unsigned char)style.data[i]) && n+1 < sizeof(compact))
                compact[n++] = (char)lower((unsigned char)style.data[i]);
            ++i;
        }
        if (i < style.len) ++i;
        compact[n] = 0;
        if (!strcmp(compact,"display:none") || !strcmp(compact,"display:none!important") ||
            !strcmp(compact,"visibility:hidden") || !strcmp(compact,"visibility:hidden!important")) return 1;
    }
    return 0;
}
static int void_tag(const fg_html_tag *t)
{
    static const char *names[] = {"area","base","br","col","embed","hr","img","input","link","meta","param","source","track","wbr"};
    size_t i;
    for (i=0; i<sizeof(names)/sizeof(names[0]); ++i) if (fg_html_is(t,names[i])) return 1;
    return 0;
}
static int remove_tag(const fg_html_tag *t)
{
    return fg_html_is(t,"head") || fg_html_is(t,"script") || fg_html_is(t,"style") ||
        fg_html_is(t,"ix:header") || fg_html_is(t,"ix:hidden") || fg_html_is(t,"ix:references") ||
        fg_html_is(t,"ix:resources") || fg_html_local_is(t,"context") ||
        fg_html_local_is(t,"unit") || hidden(t);
}
static fg_status strip(fg_str html, fg_buffer *out, fg_error *e)
{
    size_t i=0, start=0;
    fg_status s;
    while (i < html.len) {
        fg_html_tag t;
        if (!tag_start(html,i)) { ++i; continue; }
        if ((s=fg_buffer_append(out,html.data+start,i-start,e)) != FG_OK) return s;
        if ((s=fg_html_tag_read(html,i,&t,e)) != FG_OK) return s;
        i=t.end;
        if (!t.special && !t.closing && remove_tag(&t)) {
            size_t end;
            if (!void_tag(&t) && (s=fg_html_close(html,&t,&end,&i,e)) != FG_OK) return s;
        } else if (!t.special) {
            if ((s=fg_buffer_append(out,html.data+t.start,t.end-t.start,e)) != FG_OK) return s;
        }
        start=i;
    }
    return fg_buffer_append(out,html.data ? html.data+start : NULL,html.len-start,e);
}

typedef struct md_row { fg_buffer *cells; size_t count, cap; } md_row;
typedef struct md_table { md_row *rows, row; size_t count, cap; fg_buffer cell; int in_cell; size_t span; } md_table;

static fg_status grow(void **ptr, size_t *cap, size_t count, size_t size, fg_error *e)
{
    size_t n;
    void *p;
    if (count < *cap) return FG_OK;
    n=*cap ? *cap*2 : 8;
    if (n < *cap || n > SIZE_MAX/size) return fg_fail(e,FG_LIMIT,"HTML table too large");
    p=realloc(*ptr,n*size);
    if (!p) return fg_fail(e,FG_NOMEM,"Allocating HTML table");
    *ptr=p; *cap=n; return FG_OK;
}
static void row_free(md_row *r)
{
    size_t i;
    for(i=0;i<r->count;++i) fg_buffer_free(&r->cells[i]);
    free(r->cells); memset(r,0,sizeof(*r));
}
static void table_free(md_table *t)
{
    size_t i;
    row_free(&t->row); fg_buffer_free(&t->cell);
    for(i=0;i<t->count;++i) row_free(&t->rows[i]);
    free(t->rows); memset(t,0,sizeof(*t));
}
static fg_status end_cell(md_table *t, fg_error *e)
{
    fg_status s;
    size_t i, read, write=0;
    int space=0;
    if (!t->in_cell) return FG_OK;
    for(read=0;read<t->cell.len;++read) {
        if(fg_sec_space((unsigned char)t->cell.data[read])) {space=1;continue;}
        if(space&&write) t->cell.data[write++]=' ';
        space=0;
        t->cell.data[write++]=t->cell.data[read];
    }
    t->cell.len=write;
    if(t->cell.data) t->cell.data[write]=0;
    for(i=0;i<t->span;++i) {
        if(t->row.count >= 1024) return fg_fail(e,FG_LIMIT,"Too many HTML table columns");
        s=grow((void **)&t->row.cells,&t->row.cap,t->row.count,sizeof(*t->row.cells),e);
        if(s!=FG_OK) return s;
        memset(&t->row.cells[t->row.count],0,sizeof(fg_buffer));
        if(!i) { t->row.cells[t->row.count]=t->cell; memset(&t->cell,0,sizeof(t->cell)); }
        ++t->row.count;
    }
    t->in_cell=0; return FG_OK;
}
static fg_status end_row(md_table *t, fg_error *e)
{
    fg_status s=end_cell(t,e);
    size_t i;
    int content=0;
    if(s!=FG_OK) return s;
    for(i=0;i<t->row.count;++i) if(t->row.cells[i].len) content=1;
    if(content) {
        s=grow((void **)&t->rows,&t->cap,t->count,sizeof(*t->rows),e);
        if(s!=FG_OK) return s;
        t->rows[t->count++]=t->row; memset(&t->row,0,sizeof(t->row));
    } else row_free(&t->row);
    return FG_OK;
}
static fg_status repeat(fg_buffer *out, char c, size_t n, fg_error *e)
{
    char b[128];
    memset(b,c,sizeof(b));
    while(n) {
        size_t take=n < sizeof(b) ? n : sizeof(b);
        fg_status s=fg_buffer_append(out,b,take,e);
        if(s!=FG_OK) return s;
        n-=take;
    }
    return FG_OK;
}
/* Pad by code points, not bytes, so cells holding ’, — or € stay aligned. */
static size_t text_width(const fg_buffer *cell)
{
    size_t i, n=0;
    for(i=0;i<cell->len;++i) if(((unsigned char)cell->data[i]&0xc0)!=0x80) ++n;
    return n;
}
static fg_status emit_table(md_table *t, fg_buffer *out, fg_error *e)
{
    size_t widths[1024], cols=0, r, c, pass;
    fg_status s=end_row(t,e);
    if(s!=FG_OK || !t->count) return s;
    for(r=0;r<t->count;++r) if(t->rows[r].count>cols) cols=t->rows[r].count;
    for(c=0;c<cols;++c) widths[c]=3;
    for(r=0;r<t->count;++r) for(c=0;c<t->rows[r].count;++c)
        if(text_width(&t->rows[r].cells[c])>widths[c]) widths[c]=text_width(&t->rows[r].cells[c]);
    if((s=fg_buffer_append(out,"\n",1,e))!=FG_OK) return s;
    for(pass=0;pass<t->count+1;++pass) {
        r=pass > 1 ? pass-1 : 0;
        if((s=fg_buffer_append(out,"|",1,e))!=FG_OK) return s;
        for(c=0;c<cols;++c) {
            fg_buffer empty={0}, *cell=c<t->rows[r].count?&t->rows[r].cells[c]:&empty;
            if((s=fg_buffer_append(out," ",1,e))!=FG_OK) return s;
            if(pass==1) s=repeat(out,'-',widths[c],e);
            else {
                s=fg_buffer_append(out,cell->data,cell->len,e);
                if(s==FG_OK) s=repeat(out,' ',widths[c]-text_width(cell),e);
            }
            if(s!=FG_OK || (s=fg_buffer_append(out," |",2,e))!=FG_OK) return s;
        }
        if((s=fg_buffer_append(out,"\n",1,e))!=FG_OK) return s;
    }
    return fg_buffer_append(out,"\n",1,e);
}
static fg_status decode_normal(fg_str text, fg_buffer *out, int cell, fg_error *e)
{
    fg_buffer decoded={0};
    fg_status s=fg_html_entities(text,&decoded,e);
    if(s==FG_OK) s=normalized((fg_str){decoded.data,decoded.len},out,cell,e);
    fg_buffer_free(&decoded); return s;
}
/* HTML's rules for parsing non-negative integers, as browsers apply them:
   leading space and '+' are skipped and trailing text ignored; an invalid,
   negative or zero span is 1 and a larger one is clamped to 1000. The
   1024-column row limit in end_cell still bounds what spans may add. */
static size_t colspan(const fg_html_tag *t)
{
    fg_str v=fg_html_attr(t,"colspan",NULL);
    size_t i=0, n=0;
    while(i<v.len && fg_sec_space((unsigned char)v.data[i])) ++i;
    if(i<v.len && v.data[i]=='+') ++i;
    for(;i<v.len && v.data[i]>='0' && v.data[i]<='9';++i)
        if((n=n*10+(size_t)(v.data[i]-'0'))>1000) return 1000;
    return n?n:1;
}
static fg_status walk(fg_str html, fg_buffer *out, fg_error *e)
{
    md_table table={0};
    size_t i=0, depth=0;
    fg_status s=FG_OK;
    while(i<html.len) {
        fg_html_tag t;
        size_t start=i;
        const char *insert=NULL;
        if(!tag_start(html,i)) {
            do{++i;}while(i<html.len&&!tag_start(html,i));
            if(!depth || table.in_cell)
                s=decode_normal(fg_sec_slice(html,start,i),table.in_cell?&table.cell:out,table.in_cell,e);
            if(s!=FG_OK) break;
            continue;
        }
        if((s=fg_html_tag_read(html,i,&t,e))!=FG_OK) break;
        i=t.end;
        if(fg_html_is(&t,"table")) {
            if(!t.closing) {
                if(++depth>256) {s=fg_fail(e,FG_LIMIT,"HTML table nesting limit");break;}
                if(depth>1 && table.in_cell) s=fg_buffer_append(&table.cell," ",1,e);
            } else if(depth && --depth==0) {
                s=emit_table(&table,out,e); table_free(&table);
            }
        } else if(depth==1 && fg_html_is(&t,"tr")) s=end_row(&table,e);
        else if(depth==1 && (fg_html_is(&t,"td")||fg_html_is(&t,"th"))) {
            s=end_cell(&table,e);
            if(s==FG_OK && !t.closing) {table.span=colspan(&t); table.in_cell=1;}
        } else if(fg_html_is(&t,"br")) insert=depth?" ":"\n";
        else if(fg_html_is(&t,"hr")) insert=depth?" ":"\n---\n";
        else if(fg_html_is(&t,"p")||fg_html_is(&t,"div")||fg_html_is(&t,"section")||
                fg_html_is(&t,"article")||fg_html_is(&t,"blockquote"))
            insert=depth?" ":"\n";
        else if(fg_html_is(&t,"li")) insert=depth?" ":(t.closing?"\n":"\n- ");
        else if(fg_html_is(&t,"ul")||fg_html_is(&t,"ol")) insert=depth?" ":"\n";
        else if(t.name.len==2 && lower((unsigned char)t.name.data[0])=='h' &&
                t.name.data[1]>='1'&&t.name.data[1]<='6') {
            if(depth) insert=" ";
            else if(t.closing) insert="\n";
            else {
                s=fg_buffer_append(out,"\n",1,e);
                if(s==FG_OK) s=repeat(out,'#',(size_t)(t.name.data[1]-'0'),e);
                if(s==FG_OK) s=fg_buffer_append(out," ",1,e);
            }
        } else if(depth>1 && (fg_html_is(&t,"td")||fg_html_is(&t,"tr"))) insert=" ";
        if(s==FG_OK && insert && (!depth||table.in_cell)) {
            fg_buffer *target=table.in_cell?&table.cell:out;
            if(strcmp(insert,"\n") || !target->len || target->data[target->len-1]!='\n')
                s=fg_buffer_append(target,insert,strlen(insert),e);
        }
        if(s!=FG_OK) break;
    }
    if(s==FG_OK && depth) s=emit_table(&table,out,e);
    table_free(&table); return s;
}
static int contains(fg_str s,const char *needle) {return find(s,0,needle)!=s.len;}
static int starts(fg_str s,const char *prefix) {return at(s,0,prefix);}
static fg_str line_at(fg_str text,size_t *pos)
{
    size_t a=*pos,b=a;
    while(b<text.len&&text.data[b]!='\n') ++b;
    *pos=b<text.len?b+1:b;
    return fg_sec_trim(fg_sec_slice(text,a,b));
}
static fg_str bare_line(fg_str line)
{
    while(line.len && (line.data[0]=='#'||line.data[0]==' ')) {++line.data;--line.len;}
    return line;
}
static int cover_end(fg_str bare)
{
    return starts(bare,"PART ") || contains(bare,"TABLE OF CONTENTS") ||
        contains(bare,"Table of Contents");
}
/* A cover is only dropped up to a recognised end. Without one (8-K, or a
   10-Q using "Part I" and "INDEX") every line is kept rather than lost. */
static int cover_ends_later(fg_str text,size_t pos)
{
    while(pos<text.len) if(cover_end(bare_line(line_at(text,&pos)))) return 1;
    return 0;
}
static fg_status finish_lines(fg_str text,fg_buffer *out,fg_error *e)
{
    size_t pos=0;
    unsigned blanks=0;
    int cover=0,past_cover=0;
    while(pos<text.len) {
        fg_str line=line_at(text,&pos), bare=bare_line(line);
        const char *heading=NULL;
        fg_status s;
        size_t j,letters=0,upper=0;
        if(!past_cover && !cover && contains(bare,"UNITED STATES")) {
            size_t look=pos;
            fg_str next={0};
            while(look<text.len&&!next.len) next=line_at(text,&look);
            if(contains(next,"SECURITIES AND EXCHANGE COMMISSION")) {
                if(cover_end(bare) || cover_ends_later(text,pos)) cover=1;
                else past_cover=1;
            }
        }
        if(cover) {
            if(cover_end(bare)) {cover=0;past_cover=1;}
            else continue;
        }
        if(line.len<80 && line.len && line.data[0]!='|' &&
           contains(line,"Form 10-") && contains(line,"|")) continue;
        if(!line.len) {if(blanks<2) ++blanks;continue;}
        if(out->len) {
            if((s=repeat(out,'\n',blanks,e))!=FG_OK) return s;
        }
        blanks=0;
        if(line.data[0]!='|'&&line.data[0]!='#') {
            if(starts(line,"PART ")) heading="## ";
            else if(starts(line,"Item ")||starts(line,"ITEM ")) heading="### ";
            else if(line.len>15) {
                for(j=0;j<line.len;++j) {
                    unsigned char c=(unsigned char)line.data[j];
                    if(c>='A'&&c<='Z') {++letters;++upper;}
                    else if(c>='a'&&c<='z') ++letters;
                }
                if(letters>10 && (double)upper/(double)letters>0.70) heading="#### ";
            }
        }
        if(heading && (s=fg_buffer_append(out,heading,strlen(heading),e))!=FG_OK) return s;
        if((s=fg_buffer_append(out,line.data,line.len,e))!=FG_OK ||
           (s=fg_buffer_append(out,"\n",1,e))!=FG_OK) return s;
    }
    return FG_OK;
}
fg_status fg_html_to_markdown(fg_str html, fg_buffer *out, fg_error *e)
{
    fg_buffer stripped={0}, walked={0}, result={0};
    fg_status s;
    if(!out) return fg_fail(e,FG_INVALID,"Missing Markdown output");
    s=fg_sec_utf8(html,e);
    if(s==FG_OK) s=strip(html,&stripped,e);
    if(s==FG_OK) s=walk((fg_str){stripped.data,stripped.len},&walked,e);
    if(s==FG_OK) s=finish_lines((fg_str){walked.data,walked.len},&result,e);
    if(s==FG_OK) {
        /* Empty successful conversions still obey the owned buffer convention. */
        if(!result.data) s=fg_buffer_set(&result,FG_STR(""),e);
        if(s==FG_OK) {fg_buffer_free(out);*out=result;memset(&result,0,sizeof(result));}
    }
    fg_buffer_free(&stripped);fg_buffer_free(&walked);fg_buffer_free(&result);
    return s;
}

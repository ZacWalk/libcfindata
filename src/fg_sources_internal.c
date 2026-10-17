#include "fg_sources_internal.h"

const fg_json *fg_s_get(const fg_json *v, const char *k) { return fg_json_get(v, FG_STR(k)); }
fg_str fg_s_view(const fg_buffer *b) { fg_str s = {b->data, b->len}; return s; }
int fg_s_valid(fg_str s, int required)
{
    size_t i;
    if ((!s.data && s.len) || (required && !s.len)) return 0;
    for (i = 0; i < s.len; ++i)
        if ((unsigned char)s.data[i] < 32 || s.data[i] == 127) return 0;
    return 1;
}
int fg_s_equal(fg_str a, const char *b) { return fg_str_equal(a, FG_STR(b)); }
int fg_s_iequal(fg_str a, fg_str b)
{
    size_t i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; ++i) {
        unsigned char x = (unsigned char)a.data[i], y = (unsigned char)b.data[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
fg_str fg_s_trim(fg_str s)
{
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t' || s.data[0] == '\r' || s.data[0] == '\n'))
        { ++s.data; --s.len; }
    while (s.len && (s.data[s.len-1] == ' ' || s.data[s.len-1] == '\t' ||
                    s.data[s.len-1] == '\r' || s.data[s.len-1] == '\n')) --s.len;
    return s;
}
fg_status fg_s_date(fg_str s, int64_t *out, fg_error *e)
{
    if (fg_sec_date(s, out, NULL) != FG_OK) return fg_fail(e, FG_PARSE, "Invalid source calendar date");
    return FG_OK;
}
static int two(fg_str s, size_t i)
{
    if (s.data[i] < '0' || s.data[i] > '9' || s.data[i+1] < '0' || s.data[i+1] > '9') return -1;
    return (s.data[i]-'0')*10+s.data[i+1]-'0';
}
fg_status fg_s_timestamp(fg_str s, int64_t *out, uint32_t *ns, fg_error *e)
{
    int h, m, sec, oh = 0, om = 0, sign = 0;
    int64_t day;
    uint32_t nano = 0;
    size_t i = 19, n = 0;
    fg_str date = {s.data, 10};
    if (!s.data || s.len < 20 || s.data[10] != 'T' || s.data[13] != ':' || s.data[16] != ':' ||
        fg_s_date(date, &day, NULL) != FG_OK) goto bad;
    h = two(s,11); m = two(s,14); sec = two(s,17);
    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) goto bad;
    if (s.data[i] == '.') {
        ++i;
        while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9') {
            if (++n > 9) goto bad;
            nano = nano * 10 + (unsigned)(s.data[i++]-'0');
        }
        if (!n) goto bad;
        while (n++ < 9) nano *= 10;
    }
    if (i < s.len && s.data[i] == 'Z') ++i;
    else if (i + 6 == s.len && (s.data[i] == '+' || s.data[i] == '-') && s.data[i+3] == ':') {
        sign = s.data[i] == '+' ? 1 : -1;
        oh = two(s,i+1); om = two(s,i+4);
        if (oh < 0 || oh > 23 || om < 0 || om > 59) goto bad;
        i += 6;
    } else goto bad;
    if (i != s.len) goto bad;
    *out = day + h*3600 + m*60 + sec - sign*(oh*3600+om*60);
    *ns = nano;
    return FG_OK;
bad:
    return fg_fail(e, FG_PARSE, "Invalid source RFC3339 timestamp");
}
fg_status fg_s_format_time(int64_t t, char *out, int date_only, fg_error *e)
{
    int64_t z, era, doe, yoe, y, doy, mp, d, m, sec;
    if (t < INT64_C(-62135596800) || t > INT64_C(253402300799))
        return fg_fail(e, FG_INVALID, "Timestamp outside years 0001..9999");
    sec = t % 86400; z = t / 86400;
    if (sec < 0) { sec += 86400; --z; }
    z += 719468; era = z / 146097; doe = z - era*146097;
    yoe = (doe-doe/1460+doe/36524-doe/146096)/365; y = yoe + era*400;
    doy = doe-(365*yoe+yoe/4-yoe/100); mp = (5*doy+2)/153;
    d = doy-(153*mp+2)/5+1; m = mp+(mp < 10 ? 3 : -9); y += m <= 2;
    memcpy(out,"0000-00-00T00:00:00Z",21);
    out[0]=(char)('0'+y/1000); out[1]=(char)('0'+y/100%10);
    out[2]=(char)('0'+y/10%10); out[3]=(char)('0'+y%10);
    out[5]=(char)('0'+m/10); out[6]=(char)('0'+m%10);
    out[8]=(char)('0'+d/10); out[9]=(char)('0'+d%10);
    out[11]=(char)('0'+sec/3600/10); out[12]=(char)('0'+sec/3600%10);
    out[14]=(char)('0'+sec/60%60/10); out[15]=(char)('0'+sec/60%10);
    out[17]=(char)('0'+sec%60/10); out[18]=(char)('0'+sec%10);
    if (date_only) out[10]=0;
    return FG_OK;
}
fg_status fg_s_string(const fg_json *v, const char *k, int optional, fg_str *out, fg_error *e)
{
    const fg_json *n = fg_s_get(v,k);
    if (optional && (!n || fg_json_type(n) == FG_JSON_NULL)) { *out = (fg_str){0}; return FG_OK; }
    if (fg_json_type(n) != FG_JSON_STRING || !fg_s_valid(fg_json_string(n), !optional))
        return fg_fail(e, FG_PARSE, "Missing or invalid source string field");
    *out = fg_json_string(n);
    return FG_OK;
}
fg_status fg_s_double(const fg_json *v, double *out, fg_error *e)
{
    fg_status s = fg_json_double(v,out,NULL);
    if (s != FG_OK || !isfinite(*out)) return fg_fail(e, s == FG_LIMIT ? FG_LIMIT : FG_PARSE, "Invalid source number");
    return FG_OK;
}
fg_status fg_s_decimal(fg_str text, double *out, fg_error *e)
{
    fg_json *v = NULL;
    fg_status s = fg_json_new_number(text,&v,NULL);
    if (s == FG_OK) s = fg_s_double(v,out,e);
    else s = fg_fail(e,s == FG_NOMEM ? FG_NOMEM : FG_PARSE,"Invalid source decimal");
    fg_json_free(v);
    return s;
}
fg_status fg_s_encode(fg_buffer *out, fg_str value, fg_error *e)
{
    fg_buffer encoded = {0};
    fg_status s = fg_url_encode(&encoded,value,e);
    if (s == FG_OK) s = fg_buffer_append(out,encoded.data,encoded.len,e);
    fg_buffer_free(&encoded);
    return s;
}
fg_status fg_s_query(fg_buffer *url, const char *name, fg_str value, fg_error *e)
{
    fg_status s;
    if ((s = fg_buffer_append(url,"&",1,e)) != FG_OK ||
        (s = fg_buffer_append(url,name,strlen(name),e)) != FG_OK ||
        (s = fg_buffer_append(url,"=",1,e)) != FG_OK) return s;
    return fg_s_encode(url,value,e);
}
fg_status fg_s_uquery(fg_buffer *url, const char *name, uint64_t value, fg_error *e)
{
    char n[32];
    snprintf(n,sizeof(n),"%llu",(unsigned long long)value);
    return fg_s_query(url,name,FG_STR(n),e);
}
fg_status fg_s_redact(fg_error *e, fg_status s, const char *message)
{
    int status = e ? e->http_status : 0;
    fg_fail(e,s,message);
    if (e) e->http_status = status;
    return s;
}
fg_status fg_s_fetch(fg_client *c, fg_str url, const fg_header *h, size_t n, fg_json **out, fg_error *e)
{
    fg_response r = {0};
    fg_status s;
    *out = NULL;
    s = fg_get(c,url,h,n,&r,e);
    if (s == FG_OK) s = fg_json_parse(fg_s_view(&r.body),out,e);
    if (s != FG_OK) fg_s_redact(e,s,"Source request or JSON response failed");
    fg_response_free(&r);
    return s;
}
void fg_s_tokens_free(fg_s_tokens *t)
{
    size_t i;
    for (i=0;i<t->count;++i) fg_buffer_free(t->items+i);
    free(t->items); memset(t,0,sizeof(*t));
}
fg_status fg_s_next(const fg_json *tree, fg_s_tokens *t, fg_str *next, fg_error *e)
{
    fg_buffer *items;
    fg_str token;
    size_t i;
    fg_status s;
    if (!fg_s_get(tree,"next_page_token")) return fg_fail(e,FG_PARSE,"Missing pagination token field");
    s = fg_s_string(tree,"next_page_token",1,&token,e);
    if (s != FG_OK) return s;
    *next = (fg_str){0};
    if (!token.len) return FG_OK;
    for (i=0;i<t->count;++i)
        if (fg_str_equal(token,fg_s_view(t->items+i))) return fg_fail(e,FG_PARSE,"Pagination token cycle");
    if (token.len > 65536 || t->count >= 5000) return fg_fail(e,FG_LIMIT,"Pagination limit exceeded");
    items = (fg_buffer *)realloc(t->items,(t->count+1)*sizeof(*items));
    if (!items) return fg_fail(e,FG_NOMEM,"Pagination allocation failed");
    t->items = items; memset(items+t->count,0,sizeof(*items));
    if ((s = fg_buffer_set(items+t->count,token,e)) != FG_OK) return s;
    *next = fg_s_view(items+t->count++);
    return FG_OK;
}

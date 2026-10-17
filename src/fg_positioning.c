#include "fg_sources_internal.h"

typedef struct positioning_reader {
    const fg_positioning_options *options;
    fg_csv_header *header;
    size_t columns[9], known_column, width;
    fg_cftc_callback cot_callback;
    fg_finra_callback finra_callback;
    void *user;
    int cot;
} positioning_reader;

static fg_status grouped_number(fg_str text, fg_buffer *clean, fg_error *e)
{
    size_t i, integer_end, first_comma=SIZE_MAX, group=0;
    fg_status s;
    text=fg_s_trim(text);
    integer_end=0;
    while(integer_end<text.len && text.data[integer_end]!='.') ++integer_end;
    for(i=0;i<integer_end;++i) {
        if(text.data[i]==',') {
            if((first_comma==SIZE_MAX && (group<1 || group>3)) ||
                (first_comma!=SIZE_MAX && group!=3))
                return fg_fail(e,FG_PARSE,"Invalid grouped source number");
            first_comma=i; group=0;
        } else {
            if(text.data[i]<'0' || text.data[i]>'9')
                return fg_fail(e,FG_PARSE,"Invalid positioning number");
            ++group;
        }
    }
    if(first_comma!=SIZE_MAX && group!=3) return fg_fail(e,FG_PARSE,"Invalid grouped source number");
    if(!group) return fg_fail(e,FG_PARSE,"Missing positioning number");
    for(i=0;i<text.len;++i) {
        if(i>integer_end && (text.data[i]<'0' || text.data[i]>'9'))
            return fg_fail(e,FG_PARSE,"Invalid positioning decimal");
        if(text.data[i]!=',' && (s=fg_buffer_append(clean,text.data+i,1,e))!=FG_OK) return s;
    }
    return FG_OK;
}
static fg_status count_number(fg_str text, uint64_t *out, fg_error *e)
{
    fg_buffer clean={0};
    fg_json *v=NULL;
    fg_status s=grouped_number(text,&clean,e);
    if(s==FG_OK) s=fg_json_new_number(fg_s_view(&clean),&v,NULL);
    if(s==FG_OK) s=fg_json_u64(v,out,NULL);
    fg_buffer_free(&clean); fg_json_free(v);
    if(s!=FG_OK) return fg_fail(e,s==FG_NOMEM?FG_NOMEM:FG_PARSE,"Invalid positioning count");
    return FG_OK;
}
static int contains(fg_str text, fg_str needle)
{
    size_t i;
    if(!needle.len) return 1;
    for(i=0;i+needle.len<=text.len;++i)
        if(!memcmp(text.data+i,needle.data,needle.len)) return 1;
    return 0;
}
static fg_status known_time(positioning_reader *r, const fg_str *fields,
    int *present, int64_t *out, fg_error *e)
{
    fg_str date={0};
    if(r->known_column!=SIZE_MAX) date=fg_s_trim(fields[r->known_column]);
    if(date.len) { *present=1; return fg_s_date(date,out,e); }
    *present=r->options->has_known_at!=0;
    if(*present) *out=r->options->known_at;
    return FG_OK;
}
static fg_status positioning_row(void *user, const fg_str *fields, size_t count,
    size_t row, fg_error *e)
{
    static const char *names[]={"Market_and_Exchange_Names","Report_Date_as_YYYY-MM-DD",
        "Open_Interest_All","Dealer_Positions_Long_All","Dealer_Positions_Short_All",
        "Asset_Mgr_Positions_Long_All","Asset_Mgr_Positions_Short_All",
        "Lev_Money_Positions_Long_All","Lev_Money_Positions_Short_All"};
    positioning_reader *r=(positioning_reader *)user;
    size_t i;
    fg_status s;
    if(!row) {
        fg_str *trimmed;
        if(count<2) return fg_fail(e,FG_PARSE,"Positioning header has too few columns");
        trimmed=(fg_str *)malloc(count*sizeof(*trimmed));
        if(!trimmed) return fg_fail(e,FG_NOMEM,"Positioning header allocation failed");
        for(i=0;i<count;++i) trimmed[i]=fg_s_trim(fields[i]);
        s=fg_csv_header_create(trimmed,count,&r->header,e);
        free(trimmed);
        if(s!=FG_OK) return s;
        r->width=count;
        if(r->cot) for(i=0;i<9;++i) {
            r->columns[i]=SIZE_MAX;
            s=fg_csv_column(r->header,FG_STR(names[i]),r->columns+i,NULL);
            if(s!=FG_OK && i<3) return fg_fail(e,FG_PARSE,"Missing required CFTC column");
        }
        r->known_column=SIZE_MAX;
        if(r->options->known_date_column.len &&
            fg_csv_column(r->header,r->options->known_date_column,&r->known_column,NULL)!=FG_OK)
            return fg_fail(e,FG_PARSE,"Missing requested positioning known-date column");
        return FG_OK;
    }
    if(count!=r->width) return fg_fail(e,FG_PARSE,"Positioning row width differs from header");
    if(r->cot) {
        fg_cftc_record record={0};
        uint64_t *values[6]={&record.dealer_long,&record.dealer_short,&record.asset_mgr_long,
            &record.asset_mgr_short,&record.lev_money_long,&record.lev_money_short};
        record.market=fg_s_trim(fields[r->columns[0]]);
        if(!record.market.len) return fg_fail(e,FG_PARSE,"Missing CFTC market");
        if(!contains(record.market,r->options->market_contains)) return FG_OK;
        if((s=fg_s_date(fg_s_trim(fields[r->columns[1]]),&record.report_date,e))!=FG_OK ||
            (s=count_number(fields[r->columns[2]],&record.open_interest,e))!=FG_OK ||
            (s=known_time(r,fields,&record.has_knowledge_time,&record.knowledge_time,e))!=FG_OK) return s;
        if(record.has_knowledge_time && record.knowledge_time<record.report_date)
            return fg_fail(e,FG_PARSE,"CFTC knowledge time precedes report date");
        for(i=0;i<6;++i) if(r->columns[i+3]!=SIZE_MAX && fg_s_trim(fields[r->columns[i+3]]).len) {
            if((s=count_number(fields[r->columns[i+3]],values[i],e))!=FG_OK) return s;
            record.present |= 1u<<i;
        }
        return r->cot_callback(r->user,&record,e);
    } else {
        fg_finra_record record={0};
        fg_str month=fg_s_trim(fields[0]);
        char date[11];
        fg_buffer clean={0};
        if(month.len==7) {
            memcpy(date,month.data,7); memcpy(date+7,"-01",4); month=FG_STR(date);
        }
        if((s=fg_s_date(month,&record.month_date,e))!=FG_OK) return s;
        if(month.data[8]!='0' || month.data[9]!='1')
            return fg_fail(e,FG_PARSE,"FINRA month must start on day 01");
        record.debit_text=fields[1];
        record.has_debit_balances=fg_s_trim(fields[1]).len!=0;
        if(record.has_debit_balances) {
            s=grouped_number(fields[1],&clean,e);
            if(s==FG_OK) s=fg_s_decimal(fg_s_view(&clean),&record.debit_balances,e);
            fg_buffer_free(&clean);
            if(s!=FG_OK) return s;
        }
        if((s=known_time(r,fields,&record.has_knowledge_time,&record.knowledge_time,e))!=FG_OK) return s;
        if(record.has_knowledge_time && record.knowledge_time<record.month_date)
            return fg_fail(e,FG_PARSE,"FINRA knowledge time precedes month date");
        return r->finra_callback(r->user,&record,e);
    }
}

/* Only adapts delimiters/escaping; fg_csv_parse still owns UTF-8 validation,
   RFC4180 grammar, quoted field decoding, and row dispatch. */
static fg_status tsv_to_csv(fg_str input, fg_buffer *csv, fg_error *e)
{
    size_t i=0;
    int start=1, quoted=0, plain=0, closed=0;
    fg_status s;
    if(input.len>=3 && (unsigned char)input.data[0]==0xef &&
        (unsigned char)input.data[1]==0xbb && (unsigned char)input.data[2]==0xbf) i=3;
#define APP(p,n) do { s=fg_buffer_append(csv,p,n,e); if(s!=FG_OK) return s; } while(0)
    for(;i<input.len;++i) {
        char ch=input.data[i];
        if(start) {
            start=0; closed=0;
            if(ch=='"') { quoted=1; APP(&ch,1); continue; }
            /* Preserve truly blank physical rows for the CSV parser to skip. */
            if(ch=='\r' || ch=='\n') { APP(&ch,1); start=1; continue; }
            plain=1; APP("\"",1);
        }
        if(quoted) {
            APP(&ch,1);
            if(ch=='"') {
                if(i+1<input.len && input.data[i+1]=='"') { APP("\"",1); ++i; }
                else { quoted=0; closed=1; }
            }
        } else if(ch=='\t' || ch=='\r' || ch=='\n') {
            if(plain) { APP("\"",1); plain=0; }
            if(ch=='\t') APP(",",1); else APP(&ch,1);
            start=1; closed=0;
        } else {
            if(closed || ch=='"') return fg_fail(e,FG_PARSE,"Malformed TSV quoting");
            APP(&ch,1);
        }
    }
    if(plain) APP("\"",1);
    if(quoted) return fg_fail(e,FG_PARSE,"Unterminated TSV quote");
    return FG_OK;
#undef APP
}
static int valid_options(const fg_positioning_options *o)
{
    return !o || ((!o->delimiter || o->delimiter==',' || o->delimiter=='\t') &&
        fg_s_valid(o->known_date_column,0) && fg_s_valid(o->market_contains,0));
}
static fg_status positioning_parse(fg_str text, const fg_positioning_options *o,
    positioning_reader *reader, fg_error *e)
{
    fg_positioning_options defaults={0};
    fg_buffer csv={0};
    fg_str prefix;
    fg_status s;
    if(!o) o=&defaults;
    if((!text.data && text.len) || !valid_options(o))
        return fg_fail(e,FG_INVALID,"Invalid positioning options");
    prefix=text;
    if(prefix.len>=3 && !memcmp(prefix.data,"\xef\xbb\xbf",3)) { prefix.data+=3; prefix.len-=3; }
    prefix=fg_s_trim(prefix);
    if((prefix.len && prefix.data[0]=='<') ||
        (prefix.len>=4 && !memcmp(prefix.data,"PK\x03\x04",4)))
        return fg_fail(e,FG_PARSE,"Positioning input must be plain CSV or TSV, not HTML or ZIP");
    reader->options=o;
    if(o->delimiter=='\t') {
        s=tsv_to_csv(text,&csv,e);
        if(s!=FG_OK) { fg_buffer_free(&csv); return s; }
        text=fg_s_view(&csv);
    }
    s=fg_csv_parse(text,positioning_row,reader,e);
    if(s==FG_OK && !reader->header) s=fg_fail(e,FG_PARSE,"Missing positioning header");
    fg_csv_header_free(reader->header); fg_buffer_free(&csv);
    return s;
}
fg_status fg_cftc_parse(fg_str text, const fg_positioning_options *o, fg_cftc_callback cb, void *user, fg_error *e)
{
    positioning_reader r={0};
    if(!cb) return fg_fail(e,FG_INVALID,"Missing CFTC callback");
    r.cot=1; r.cot_callback=cb; r.user=user;
    return positioning_parse(text,o,&r,e);
}
fg_status fg_finra_parse(fg_str text, const fg_positioning_options *o, fg_finra_callback cb, void *user, fg_error *e)
{
    positioning_reader r={0};
    if(!cb) return fg_fail(e,FG_INVALID,"Missing FINRA callback");
    r.finra_callback=cb; r.user=user;
    return positioning_parse(text,o,&r,e);
}
static int public_url(fg_str url)
{
    size_t end=8;
    if(!fg_s_valid(url,1) || url.len<=8 || memcmp(url.data,"https://",8)) return 0;
    while(end<url.len && url.data[end]!='/' && url.data[end]!='?' && url.data[end]!='#') {
        if(url.data[end]=='@' || url.data[end]=='\\' || url.data[end]==' ' || url.data[end]=='%') return 0;
        ++end;
    }
    return end>8;
}
fg_status fg_cftc_fetch(fg_client *c, fg_str url, const fg_positioning_options *o,
    fg_cftc_callback cb, void *user, fg_error *e)
{
    fg_response r={0};
    fg_status s;
    if(!c || !cb || !public_url(url) || !valid_options(o)) return fg_fail(e,FG_INVALID,"Invalid CFTC fetch options");
    s=fg_get(c,url,NULL,0,&r,e);
    if(s==FG_OK) s=fg_cftc_parse(fg_s_view(&r.body),o,cb,user,e);
    else fg_s_redact(e,s,"CFTC public fetch failed");
    fg_response_free(&r); return s;
}
fg_status fg_finra_fetch(fg_client *c, fg_str url, const fg_positioning_options *o,
    fg_finra_callback cb, void *user, fg_error *e)
{
    fg_response r={0};
    fg_status s;
    if(!c || !cb || !public_url(url) || !valid_options(o)) return fg_fail(e,FG_INVALID,"Invalid FINRA fetch options");
    s=fg_get(c,url,NULL,0,&r,e);
    if(s==FG_OK) s=fg_finra_parse(fg_s_view(&r.body),o,cb,user,e);
    else fg_s_redact(e,s,"FINRA public fetch failed");
    fg_response_free(&r); return s;
}

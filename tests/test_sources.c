#include "fg_sources.h"
#include "fg_source_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static const char *fixture_directory;
static const int64_t jan1=INT64_C(1672531200);
typedef struct step {
    const char *url, *query, *query2, *query3, *forbidden;
    const char *fixture, *body, *headers;
    int status;
} step;
typedef struct mock { const step *steps; size_t count, cursor; } mock;

static fg_buffer fixture(const char *name)
{
    char path[1024];
    fg_buffer b={0};
    fg_error e={0};
#ifdef _WIN32
    snprintf(path,sizeof(path),"%s\\%s",fixture_directory,name);
#else
    snprintf(path,sizeof(path),"%s/%s",fixture_directory,name);
#endif
    CHECK(fg_read_file(FG_STR(path),1024*1024,&b,&e)==FG_OK);
    return b;
}
static fg_str view(const fg_buffer *b) { fg_str s={b->data,b->len}; return s; }
static fg_str header(const fg_request *r, const char *name)
{
    size_t i;
    for(i=0;i<r->header_count;++i)
        if(fg_str_equal(r->headers[i].name,FG_STR(name))) return r->headers[i].value;
    return (fg_str){0};
}
static int has(fg_str text, const char *needle)
{
    size_t i,n=strlen(needle);
    for(i=0;i+n<=text.len;++i) if(!memcmp(text.data+i,needle,n)) return 1;
    return 0;
}
static fg_status replay(void *user, const fg_request *r, fg_response *out, fg_error *e)
{
    mock *m=(mock *)user;
    const step *s;
    fg_buffer body={0},headers={0};
    CHECK(m->cursor<m->count);
    s=m->steps+m->cursor++;
    CHECK(has(r->url,s->url));
    if(s->query) CHECK(has(r->url,s->query));
    if(s->query2) CHECK(has(r->url,s->query2));
    if(s->query3) CHECK(has(r->url,s->query3));
    if(s->forbidden) CHECK(!has(r->url,s->forbidden));
    CHECK(!header(r,"Authorization").len);
    CHECK(!header(r,"Cookie").len);
    if(s->fixture) body=fixture(s->fixture);
    else CHECK(fg_buffer_set(&body,FG_STR(s->body?s->body:""),e)==FG_OK);
    CHECK(fg_buffer_set(&headers,FG_STR(s->headers?s->headers:""),e)==FG_OK);
    out->status=s->status?s->status:200; out->body=body; out->headers=headers;
    return FG_OK;
}
static fg_client *client(mock *m, fg_transport *transport)
{
    fg_client *c=NULL;
    fg_client_options o={0};
    o.user_agent=FG_STR("sources-tests offline");
    transport->user=m; transport->get=replay; transport->post=NULL;
    CHECK(fg_client_create(transport,&o,&c,NULL)==FG_OK);
    return c;
}
static void complete(mock *m, fg_client *c) { CHECK(m->cursor==m->count); fg_client_free(c); }
typedef struct fred_result { size_t n; fg_fred_observation records[8]; } fred_result;
static fg_status fred_record(void *user, const fg_fred_observation *r, fg_error *e)
{
    fred_result *out=(fred_result *)user;
    (void)e;
    CHECK(out->n<8);
    out->records[out->n++]=*r;
    if(r->has_value) CHECK(r->value_text.len>0);
    return FG_OK;
}
static fg_fred_options fred_options(void)
{
    fg_fred_options o={0};
    o.series_id=FG_STR("JTS JOL&"); o.api_key=FG_STR("k/+ &=\xc3\xa9");
    return o;
}
static void fred_tests(void)
{
    const step first[]={
        {.url="https://api.stlouisfed.org/fred/series/observations?",.query="output_type=4",
         .query2="api_key=k%2F%2B%20%26%3D%C3%A9",.query3="series_id=JTS%20JOL%26",
         .fixture="fred_first_1.json"},
        {.url="realtime_start=1776-07-04&realtime_end=9999-12-31",.query="offset=2",
         .query2="limit=100000",.fixture="fred_first_2.json"}
    };
    const step latest[]={{.url="output_type=1",.forbidden="output_type=4",.fixture="fred_latest.json"}};
    fg_transport t;
    mock m={first,2,0};
    fg_client *c=client(&m,&t);
    fg_fred_options o=fred_options();
    fred_result initial={0}, revised={0};
    fg_error e={0};
    CHECK(fg_fred_observations(c,&o,fred_record,&initial,&e)==FG_OK);
    CHECK(initial.n==3); CHECK(initial.records[0].value==10824);
    CHECK(initial.records[0].observation_date==jan1);
    CHECK(initial.records[0].knowledge_time==INT64_C(1678233600));
    CHECK(initial.records[0].has_knowledge_time && initial.records[0].is_first_release);
    CHECK(!initial.records[1].has_value);
    CHECK(initial.records[2].value==9590.25); complete(&m,c);
    m=(mock){latest,1,0}; c=client(&m,&t); o.vintage=FG_FRED_LATEST;
    CHECK(fg_fred_observations(c,&o,fred_record,&revised,&e)==FG_OK);
    CHECK(revised.n==1 && revised.records[0].value==10318);
    CHECK(revised.records[0].value!=initial.records[0].value);
    CHECK(!revised.records[0].has_knowledge_time && !revised.records[0].is_first_release);
    complete(&m,c);
    {
        const char *bad[]={
            "{}",
            "{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":[]}",
            "{\"count\":1,\"offset\":1,\"limit\":100000,\"observations\":[]}",
            "{\"count\":1,\"offset\":0,\"limit\":0,\"observations\":[]}",
            "{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":[{\"date\":\"2023-02-29\",\"realtime_start\":\"2023-03-08\",\"realtime_end\":\"9999-12-31\",\"value\":\"10824\"}]}",
            "{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":[{\"date\":\"2023-01-01\",\"realtime_end\":\"9999-12-31\",\"value\":\"10824\"}]}",
            "{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":[{\"date\":\"2023-01-01\",\"realtime_start\":\"2023-03-08\",\"realtime_end\":\"9999-12-31\",\"value\":\"10824oops\"}]}"
        };
        size_t i;
        for(i=0;i<sizeof(bad)/sizeof(*bad);++i) {
            step one={.url="output_type=4",.body=bad[i]};
            m=(mock){&one,1,0}; c=client(&m,&t); o=fred_options(); initial=(fred_result){0};
            CHECK(fg_fred_observations(c,&o,fred_record,&initial,&e)==FG_PARSE);
            complete(&m,c);
        }
    }
    {
        step steps[2]={first[0],{.url="offset=2",.status=500,.body="secret=xyz"}};
        m=(mock){steps,2,0}; c=client(&m,&t); o=fred_options(); initial=(fred_result){0};
        CHECK(fg_fred_observations(c,&o,fred_record,&initial,&e)==FG_HTTP);
        CHECK(initial.n==2); CHECK(!strstr(e.message,"xyz")); complete(&m,c);
    }
    m=(mock){NULL,0,0}; c=client(&m,&t); o=fred_options();
    o.api_key=(fg_str){"a\0secret",8};
    CHECK(fg_fred_observations(c,&o,fred_record,&initial,&e)==FG_INVALID);
    o=fred_options(); o.observation_start=FG_STR("2023-02-30");
    CHECK(fg_fred_observations(c,&o,fred_record,&initial,&e)==FG_INVALID);
    complete(&m,c);
}

typedef struct cot_result { size_t n; fg_cftc_record records[4]; } cot_result;
typedef struct finra_result { size_t n; fg_finra_record records[4]; } finra_result;
static fg_status cot_record(void *user, const fg_cftc_record *r, fg_error *e)
{
    cot_result *out=(cot_result *)user; (void)e;
    CHECK(out->n<4); out->records[out->n++]=*r;
    return FG_OK;
}
static fg_status finra_record(void *user, const fg_finra_record *r, fg_error *e)
{
    finra_result *out=(finra_result *)user; (void)e;
    CHECK(out->n<4); out->records[out->n++]=*r;
    return FG_OK;
}
static void positioning_tests(void)
{
    fg_buffer cot=fixture("cftc.csv"), finra=fixture("finra.csv"), tsv=fixture("finra.tsv");
    fg_positioning_options o={0};
    cot_result cr={0};
    finra_result fr={0};
    fg_error e={0};
    o.known_date_column=FG_STR("Published"); o.market_contains=FG_STR("NASDAQ-100");
    CHECK(fg_cftc_parse(view(&cot),&o,cot_record,&cr,&e)==FG_OK);
    CHECK(cr.n==2); CHECK(cr.records[0].open_interest==UINT64_C(9007199254740993));
    CHECK(cr.records[0].present==63 && cr.records[1].present==62);
    CHECK(cr.records[0].report_date==jan1+2*86400);
    CHECK(cr.records[0].knowledge_time==jan1+5*86400);
    CHECK(cr.records[0].has_knowledge_time && !cr.records[1].has_knowledge_time);
    CHECK(fg_finra_parse(view(&finra),&o,finra_record,&fr,&e)==FG_OK);
    CHECK(fr.n==3); CHECK(fr.records[0].debit_balances==641236.5 && fr.records[0].month_date==jan1);
    CHECK(fr.records[0].has_knowledge_time && !fr.records[1].has_debit_balances);
    CHECK(!fr.records[2].has_knowledge_time);
    fr=(finra_result){0}; o.delimiter='\t';
    CHECK(fg_finra_parse(view(&tsv),&o,finra_record,&fr,&e)==FG_OK);
    CHECK(fr.n==2 && fr.records[0].debit_balances==641236.5 && fr.records[1].debit_balances==642000);
    o=(fg_positioning_options){0}; o.has_known_at=1; o.known_at=jan1+90*86400;
    cr=(cot_result){0};
    CHECK(fg_cftc_parse(view(&cot),&o,cot_record,&cr,&e)==FG_OK);
    CHECK(cr.n==3 && cr.records[0].knowledge_time==o.known_at);
    {
        const char *bad[]={"","Month,Debt\n2023-13,1\n","Month,Debt\n2023-01-31,1\n",
            "Month,Debt\n2023-01,\"12,34\"\n","Month,Debt\n2023-01,oops\n",
            "Month,Debt\n2023-01,1,extra\n","Month,Debt\n\"2023-01,1\n","Month,Debt\n2023-01,-1\n",
            "<html>Sorry, please enable cookies</html>","PK\x03\x04" "name,data"};
        size_t i;
        for(i=0;i<sizeof(bad)/sizeof(*bad);++i) {
            fr=(finra_result){0};
            CHECK(fg_finra_parse(FG_STR(bad[i]),NULL,finra_record,&fr,&e)==FG_PARSE);
        }
        cr=(cot_result){0};
        CHECK(fg_cftc_parse(FG_STR("Market_and_Exchange_Names,Report_Date_as_YYYY-MM-DD\nA,2023-01-01\n"),
            NULL,cot_record,&cr,&e)==FG_PARSE);
        CHECK(fg_cftc_parse(FG_STR("Market_and_Exchange_Names,Report_Date_as_YYYY-MM-DD,Open_Interest_All\nA,2023-01-01,3x\n"),
            NULL,cot_record,&cr,&e)==FG_PARSE);
        o=(fg_positioning_options){0}; o.delimiter='\t'; fr=(finra_result){0};
        CHECK(fg_finra_parse(FG_STR("Month\tDebt\n2023-01\t\"12\"oops\n"),&o,finra_record,&fr,&e)==FG_PARSE);
    }
    {
        const step fetches[]={
            {.url="https://public.example/cftc.csv",.fixture="cftc.csv"},
            {.url="https://public.example/finra.tsv",.fixture="finra.tsv"}
        };
        fg_transport t;
        mock m={fetches,2,0};
        fg_client *c=client(&m,&t);
        cr=(cot_result){0}; fr=(finra_result){0};
        CHECK(fg_cftc_fetch(c,FG_STR("https://public.example/cftc.csv"),NULL,cot_record,&cr,&e)==FG_OK);
        CHECK(cr.n==3 && !cr.records[0].has_knowledge_time);
        o=(fg_positioning_options){0}; o.delimiter='\t';
        CHECK(fg_finra_fetch(c,FG_STR("https://public.example/finra.tsv"),&o,finra_record,&fr,&e)==FG_OK);
        CHECK(fr.n==2); complete(&m,c);
        m=(mock){NULL,0,0}; c=client(&m,&t);
        CHECK(fg_cftc_fetch(c,FG_STR("https://secret@public.example/data"),NULL,cot_record,&cr,&e)==FG_INVALID);
        CHECK(fg_finra_fetch(c,FG_STR("http://public.example/data"),NULL,finra_record,&fr,&e)==FG_INVALID);
        complete(&m,c);
    }
    fg_buffer_free(&cot); fg_buffer_free(&finra); fg_buffer_free(&tsv);
}

static void edge_tests(void)
{
    fg_transport t;
    fg_error e={0};
    mock m={0};
    fg_client *c;
    {
        const step response={.url="/observations?",.query="output_type=4",.query2="observation_start=2023-01-01",
            .query3="observation_end=2023-01-01",
            .body="{\"count\":1,\"offset\":0,\"limit\":100000,\"observations\":[{\"date\":\"2023-01-01\",\"realtime_start\":\"2023-03-08\",\"realtime_end\":\"9999-12-31\",\"value\":\"-0.125\"}]}"};
        fg_fred_options o=fred_options();
        fred_result fr={0};
        o.observation_start=o.observation_end=FG_STR("2023-01-01");
        m=(mock){&response,1,0}; c=client(&m,&t);
        CHECK(fg_fred_observations(c,&o,fred_record,&fr,&e)==FG_OK);
        CHECK(fr.records[0].value==-0.125); complete(&m,c);
    }
    {
        const char *quoted_tsv="Month\tDebt\tExtra\r\n2023-01\t\"1,234.5\"\t\"line one,\nline \"\"two\"\"\"\r\n";
        fg_positioning_options o={0};
        finra_result fr={0};
        cot_result cr={0};
        o.delimiter='\t';
        CHECK(fg_finra_parse(FG_STR(quoted_tsv),&o,finra_record,&fr,&e)==FG_OK);
        CHECK(fr.n==1 && fr.records[0].debit_balances==1234.5);
        o=(fg_positioning_options){0}; o.known_date_column=FG_STR("Published");
        CHECK(fg_finra_parse(FG_STR("Month,Debt,Published\n2023-01,100,2022-12-31\n"),
            &o,finra_record,&fr,&e)==FG_PARSE);
        CHECK(fg_cftc_parse(FG_STR("Market_and_Exchange_Names,Report_Date_as_YYYY-MM-DD,Open_Interest_All,Published\nA,2023-01-03,1,2023-01-02\n"),
            &o,cot_record,&cr,&e)==FG_PARSE);
        {
            step failure={.url="https://public.example/data",.status=404,.body="Not found"};
            m=(mock){&failure,1,0}; c=client(&m,&t);
            CHECK(fg_cftc_fetch(c,FG_STR("https://public.example/data"),NULL,cot_record,&cr,&e)==FG_HTTP);
            CHECK(e.http_status==404); complete(&m,c);
            failure.status=302; failure.headers="Location: https://other.example/data\r\n";
            m=(mock){&failure,1,0}; c=client(&m,&t);
            CHECK(fg_finra_fetch(c,FG_STR("https://public.example/data"),NULL,finra_record,&fr,&e)==FG_HTTP);
            complete(&m,c);
        }
    }
}

static void source_utils_tests(void)
{
    fg_error e={0};
    fg_buffer url={0};
    fg_json *tree=NULL;
    fg_s_tokens tokens={0};
    fg_str next={0};
    int64_t timestamp;
    uint32_t ns;
    char formatted[21];
    CHECK(fg_s_timestamp(FG_STR("2024-02-29T01:30:00.123456789+01:30"),&timestamp,&ns,&e)==FG_OK);
    CHECK(timestamp==INT64_C(1709164800) && ns==123456789);
    CHECK(fg_s_format_time(timestamp,formatted,0,&e)==FG_OK);
    CHECK(!strcmp(formatted,"2024-02-29T00:00:00Z"));
    CHECK(fg_s_timestamp(FG_STR("2023-02-29T00:00:00Z"),&timestamp,&ns,&e)==FG_PARSE);
    CHECK(fg_s_timestamp(FG_STR("2024-02-29T00:00:00+24:00"),&timestamp,&ns,&e)==FG_PARSE);
    CHECK(fg_s_format_time(INT64_MAX,formatted,0,&e)==FG_INVALID);
    CHECK(!fg_s_valid((fg_str){"a\0b",3},1));
    CHECK(fg_s_iequal(FG_STR("Mixed"),FG_STR("mIXED")));
    CHECK(fg_buffer_set(&url,FG_STR("https://example.test/?fixed=1"),&e)==FG_OK);
    CHECK(fg_s_query(&url,"value",FG_STR("a/+&"),&e)==FG_OK);
    CHECK(fg_s_equal(fg_s_view(&url),"https://example.test/?fixed=1&value=a%2F%2B%26"));
    fg_buffer_free(&url);
    CHECK(fg_json_parse(FG_STR("{\"next_page_token\":\"a/+&\"}"),&tree,&e)==FG_OK);
    CHECK(fg_s_next(tree,&tokens,&next,&e)==FG_OK);
    fg_json_free(tree);
    CHECK(fg_s_equal(next,"a/+&"));
    CHECK(fg_json_parse(FG_STR("{\"next_page_token\":\"a/+&\"}"),&tree,&e)==FG_OK);
    CHECK(fg_s_next(tree,&tokens,&next,&e)==FG_PARSE);
    fg_json_free(tree);
    CHECK(fg_json_parse(FG_STR("{\"next_page_token\":null}"),&tree,&e)==FG_OK);
    CHECK(fg_s_next(tree,&tokens,&next,&e)==FG_OK && !next.len);
    fg_json_free(tree);
    CHECK(fg_json_parse(FG_STR("{}"),&tree,&e)==FG_OK);
    CHECK(fg_s_next(tree,&tokens,&next,&e)==FG_PARSE);
    fg_json_free(tree);
    fg_s_tokens_free(&tokens);
    CHECK(!tokens.items && !tokens.count);
}

int main(int argc, char **argv)
{
    if(argc!=2) { fprintf(stderr,"usage: test_sources FIXTURE_DIRECTORY\n"); return 2; }
    fixture_directory=argv[1];
    fred_tests(); positioning_tests(); edge_tests(); source_utils_tests();
    printf("sources: %d checks passed (offline fixtures)\n",checks);
    return 0;
}

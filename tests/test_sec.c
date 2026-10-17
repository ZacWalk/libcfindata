#include "test.h"
#include "fg_sec.h"
#include <stdlib.h>

static const fg_json *get(const fg_json *v,const char *key) {return fg_json_get(v,FG_STR(key));}
static int same(const fg_json *v,const char *text) {return fg_str_equal(fg_json_string(v),FG_STR(text));}
static int number_same(const fg_json *v,const char *text) {return fg_str_equal(fg_json_number_text(v),FG_STR(text));}
static fg_status load(const char *dir,const char *file,fg_buffer *out,fg_error *e)
{
    char path[2048];
#ifdef _WIN32
    snprintf(path,sizeof(path),"%s\\%s",dir,file);
#else
    snprintf(path,sizeof(path),"%s/%s",dir,file);
#endif
    return fg_read_file(FG_STR(path),1024*1024,out,e);
}

typedef struct clock_state {uint64_t now;unsigned sleeps;fg_transport transport;} clock_state;
static uint64_t now_ms(void *user) {return ((clock_state *)user)->now;}
static void sleep_ms(void *user,unsigned n) {clock_state *c=user;c->now+=n;++c->sleeps;}
static fg_status make_client(fg_fixture_transport *f,clock_state *clock,int ua,fg_client **out,fg_error *e)
{
    fg_client_options o={0};
    clock->transport.user=f;clock->transport.get=fg_fixture_get;
    o.user_agent=ua?FG_STR("offline-sec-test (configured by caller)"):FG_STR("");
    o.clock_user=clock;o.now_ms=now_ms;o.sleep_ms=sleep_ms;
    return fg_client_create(&clock->transport,&o,out,e);
}
typedef struct filing_state {unsigned count;char acc[4][32];int stop;} filing_state;
static fg_status on_filing(void *user,const fg_sec_filing *f,fg_error *e)
{
    filing_state *s=user;
    if(s->stop) return fg_fail(e,FG_IO,"callback stopped");
    if(s->count>=4||f->cik!=42||f->accession.len!=20)
        return fg_fail(e,FG_INVALID,"Unexpected filing");
    memcpy(s->acc[s->count],f->accession.data,20);
    s->acc[s->count][20]=0;++s->count;
    return FG_OK;
}
typedef struct facts_state {unsigned count;int exact,instant,text,usd;int64_t knowledge[2];int stop;} facts_state;
static fg_status on_fact(void *user,const fg_sec_companyfact *f,fg_error *e)
{
    facts_state *s=user;
    if(s->stop) return fg_fail(e,FG_IO,"fact callback stopped");
    if(f->cik!=42||f->knowledge_time<1714521600||f->period_end>f->knowledge_time)
        return fg_fail(e,FG_INVALID,"Incorrect companyfact times");
    ++s->count;
    if(number_same(f->value,"10000000000000001")) {
        s->exact=1;
        if(f->has_start||f->period_start!=0) return fg_fail(e,FG_INVALID,"Instant has start");
        s->instant=1;
    }
    if(fg_json_type(f->value)==FG_JSON_STRING) {
        if(f->period_end!=1709164800) return fg_fail(e,FG_INVALID,"Leap day conversion");
        s->text=1;
    }
    if(fg_str_equal(f->name,FG_STR("Revenues"))&&fg_str_equal(f->unit,FG_STR("USD"))) {
        if(!f->has_start||f->period_start!=1704067200||f->period_end!=1711843200||s->usd>=2)
            return fg_fail(e,FG_INVALID,"Duration dates");
        s->knowledge[s->usd++]=f->knowledge_time;
    }
    return FG_OK;
}
static int acquisition(const char *dir)
{
    fg_buffer tickers={0},sub={0},older={0},html={0},company={0},body={0},md={0};
    fg_json *map=NULL,*tree=NULL,*xbrl=NULL;
    fg_error e={0};
    fg_client *client=NULL;
    uint64_t cik=0;
    clock_state clock={0};
    filing_state filings={0};
    facts_state facts={0};
    fg_fixture items[5];
    fg_fixture_transport transport={items,5,0};
    OK(load(dir,"tickers.json",&tickers,&e));
    OK(load(dir,"submissions.json",&sub,&e));
    OK(load(dir,"older.json",&older,&e));
    OK(load(dir,"filing.htm",&html,&e));
    OK(load(dir,"companyfacts.json",&company,&e));
    items[0]=(fg_fixture){"https://www.sec.gov/files/company_tickers.json",tickers.data,200,"",
        "User-Agent","offline-sec-test (configured by caller)"};
    items[1]=(fg_fixture){"https://data.sec.gov/submissions/CIK0000000042.json",sub.data,200,"",
        "User-Agent","offline-sec-test (configured by caller)"};
    items[2]=(fg_fixture){"https://data.sec.gov/submissions/CIK0000000042-submissions-001.json",older.data,200,"",
        "User-Agent","offline-sec-test (configured by caller)"};
    items[3]=(fg_fixture){"https://www.sec.gov/Archives/edgar/data/42/000000004224000001/quarter.htm",html.data,200,"",
        "User-Agent","offline-sec-test (configured by caller)"};
    items[4]=(fg_fixture){"https://data.sec.gov/api/xbrl/companyfacts/CIK0000000042.json",company.data,200,"",
        "User-Agent","offline-sec-test (configured by caller)"};
    OK(make_client(&transport,&clock,1,&client,&e));
    OK(fg_sec_tickers(client,&map,&e));
    OK(fg_sec_ticker_cik(map,FG_STR("demo"),&cik,&e));CHECK(cik==42);
    OK(fg_sec_ticker_cik(map,FG_STR("DEMO.B"),&cik,&e));CHECK(cik==43);
    OK(fg_sec_ticker_cik(map,FG_STR("demo/b"),&cik,&e));CHECK(cik==43);
    CHECK(fg_sec_ticker_cik(map,FG_STR("missing"),&cik,&e)==FG_NOT_FOUND);CHECK(cik==43);
    OK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e));
    CHECK(filings.count==2&&!strcmp(filings.acc[1],"0000000042-22-000003"));
    OK(fg_sec_filing_fetch(client,42,FG_STR(filings.acc[0]),FG_STR("quarter.htm"),&body,&e));
    CHECK(body.len==html.len);
    OK(fg_html_to_markdown((fg_str){body.data,body.len},&md,&e));
    CHECK(strstr(md.data,"## PART I")&&strstr(md.data,"### Item 1."));
    CHECK(strstr(md.data,"#### CONDENSED CONSOLIDATED BALANCE SHEETS"));
    CHECK(!strstr(md.data,"REMOVE HEAD")&&!strstr(md.data,"REMOVE SCRIPT"));
    CHECK(!strstr(md.data,"REMOVE HIDDEN")&&!strstr(md.data,"REMOVE COVER"));
    CHECK(!strstr(md.data,"REMOVE COMMENT")&&!strstr(md.data,"xbrli:"));
    CHECK(strstr(md.data,"| Metric")&&strstr(md.data,"84,000")&&strstr(md.data,"\\| pipe"));
    CHECK(strstr(md.data,"\xc3\xa9")&&strstr(md.data,"\xe2\x82\xac")&&strstr(md.data,"\xf0\x9f\x9a\x80"));
    CHECK(strstr(md.data,"Words stay spaced.")&&strstr(md.data,"\nNext line."));
    CHECK(strstr(md.data,"&unknown;")&&!strstr(md.data,"Form 10-Q |"));
    CHECK(strstr(md.data,"- First item")&&strstr(md.data,"End of synthetic filing."));
    OK(fg_xbrl_extract((fg_str){body.data,body.len},&xbrl,&e));
    CHECK(fg_json_size(xbrl)==5);
    CHECK(number_same(get(fg_json_at(xbrl,0),"value"),"84000e6"));
    CHECK(same(get(fg_json_at(xbrl,0),"decimals"),"-6"));
    CHECK(same(get(fg_json_at(xbrl,0),"unit"),"USD"));
    CHECK(same(get(fg_json_at(xbrl,0),"start"),"2024-01-01"));
    CHECK(same(get(fg_json_at(xbrl,0),"end"),"2024-03-31"));
    CHECK(number_same(get(fg_json_at(xbrl,1),"value"),"1250e-2"));
    CHECK(same(get(fg_json_at(xbrl,2),"text"),"Synthetic & Co. Second part. Final."));
    CHECK(same(get(fg_json_at(xbrl,2),"instant"),"2024-02-29"));
    CHECK(fg_json_type(get(fg_json_at(xbrl,3),"value"))==FG_JSON_NULL);
    CHECK(number_same(get(fg_json_at(xbrl,4),"value"),"10000000000000001"));
    OK(fg_sec_companyfacts(client,42,&tree,&e));
    OK(fg_sec_companyfacts_extract(tree,on_fact,&facts,&e));
    CHECK(facts.count==5&&facts.exact&&facts.instant&&facts.text&&facts.usd==2);
    CHECK(facts.knowledge[0]==1714521600&&facts.knowledge[1]==1714608000);
    CHECK(transport.next==5&&clock.now==480&&clock.sleeps==4);
    fg_client_free(client);fg_json_free(map);fg_json_free(tree);fg_json_free(xbrl);
    fg_buffer_free(&tickers);fg_buffer_free(&sub);fg_buffer_free(&older);
    fg_buffer_free(&html);fg_buffer_free(&company);fg_buffer_free(&body);fg_buffer_free(&md);
    return 0;
}

static int html_cases(void)
{
    static const struct {const char *html,*expected;} cases[]={
        {"<div>Hello</div><div>World</div>","Hello\nWorld\n"},
        {"<p>A &amp; B &lt; C &gt; D &quot;x&quot; &apos;y&apos;.</p>","A & B < C > D \"x\" 'y'.\n"},
        {"<p>&#x1f600; &#128512; &#xD800; &#0; &#9999999999999999999999;</p>",
         "\xf0\x9f\x98\x80 \xf0\x9f\x98\x80 \xef\xbf\xbd \xef\xbf\xbd \xef\xbf\xbd\n"},
        {"<p>&#x; &bogus; &amp;lt;</p>","&#x; &bogus; &lt;\n"},
        {"<table><tr><td>A</td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table>",
         "| A   | B   |\n| --- | --- |\n| C   | D   |\n"},
        {"<table><tr><td>&nbsp;</td></tr></table><p>Kept</p>","Kept\n"},
        {"<table><tr><td colspan='2'>A</td></tr><tr><td>B</td><td>C</td></tr></table>",
         "| A   |     |\n| --- | --- |\n| B   | C   |\n"},
        {"<table><tr><td>A<table><tr><td>B</td><td>C</td></tr></table>D</td><td>E</td></tr></table>",
         "| A B C D | E   |\n| ------- | --- |\n"},
        {"<h2>Title</h2><p> spaced \t words </p><script>var a = '<';</script><p>After</p>",
         "## Title\nspaced words\nAfter\n"},
        {"<div hidden><div>hidden</div></div><p>visible</p><ix:header>metadata</ix:header>","visible\n"},
        {"<p>2 < 3 and 4 > 1</p>","2 < 3 and 4 > 1\n"},
        {"<p>Form 10-Q | Financial data</p><table><tr><td>Form 10-Q</td><td>123</td></tr></table>",
         "| Form 10-Q | 123 |\n| --------- | --- |\n"},
        {"<div>UNITED STATES</div><div>SECURITIES AND EXCHANGE COMMISSION</div><div>Cover</div>"
         "<div>PART II</div><div>Item 2. Other Information</div>",
         "## PART II\n### Item 2. Other Information\n"},
        {"<div>UNITED STATES</div><div>SECURITIES AND EXCHANGE COMMISSION</div><div>FORM 8-K</div>"
         "<div>Item 2.02 Results of Operations</div><p>Body</p>",
         "UNITED STATES\n#### SECURITIES AND EXCHANGE COMMISSION\nFORM 8-K\n"
         "### Item 2.02 Results of Operations\nBody\n"},
        {"<table><tr><td>\xe2\x80\x99</td><td>B</td></tr><tr><td>Caf\xc3\xa9s</td><td>D</td></tr></table>",
         "| \xe2\x80\x99     | B   |\n| ----- | --- |\n| Caf\xc3\xa9s | D   |\n"},
        {"<p>A</p><!--><p>B</p>","A\nB\n"},
        {"<p>A</p><!---><p>B</p>","A\nB\n"},
        {"<!---><p>Kept</p><!-- note --><p>End</p>","Kept\nEnd\n"},
        {"<p>A</p><!-- note --!><p>B</p>","A\nB\n"},
        {"<!-- a --!><p>Kept</p><!-- b --><p>End</p>","Kept\nEnd\n"},
        {"<!---!><p>Hidden</p>--><p>Shown</p>","Shown\n"},
        {"<table><tr><td colspan='abc'>A</td><td colspan='0'>B</td><td colspan='-2'>C</td></tr>"
         "<tr><td>1</td><td>2</td><td>3</td></tr></table>",
         "| A   | B   | C   |\n| --- | --- | --- |\n| 1   | 2   | 3   |\n"},
        {"<table><tr><td colspan=' +2px'>A</td><td>B</td></tr><tr><td>1</td><td>2</td><td>3</td></tr></table>",
         "| A   |     | B   |\n| --- | --- | --- |\n| 1   | 2   | 3   |\n"},
        {"<span>Unicode\xc2\xa0space</span>","Unicode space\n"}
    };
    static const char *clamped[]={
        "<table><td colspan='1001'>x</td></table>",
        "<table><td colspan='1000000'>x</td></table>",
        "<table><td colspan='99999999999999999999999999'>x</td></table>"
    };
    size_t i;
    fg_error e={0};
    fg_buffer b={0};
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        OK(fg_html_to_markdown(FG_STR(cases[i].html),&b,&e));
        if(strcmp(b.data,cases[i].expected)) fprintf(stderr,"HTML case %zu got: [%s]\n",i,b.data);
        CHECK(!strcmp(b.data,cases[i].expected));
    }
    OK(fg_buffer_set(&b,FG_STR("unchanged"),&e));
    CHECK(fg_html_to_markdown(FG_STR("<div a='oops>"),&b,&e)==FG_PARSE);
    CHECK(!strcmp(b.data,"unchanged"));
    CHECK(fg_html_to_markdown(FG_STR("<!--"),&b,&e)==FG_PARSE);
    CHECK(fg_html_to_markdown(FG_STR("<head>not closed"),&b,&e)==FG_PARSE);
    CHECK(fg_html_to_markdown(FG_STR("\xc0\xaf"),&b,&e)==FG_PARSE);
    CHECK(fg_html_to_markdown((fg_str){"a\0b",3},&b,&e)==FG_PARSE);
    /* Browsers clamp colspan at 1000; the 1024-column row limit still holds. */
    for(i=0;i<sizeof(clamped)/sizeof(clamped[0]);++i) {
        size_t k, bars=0;
        OK(fg_html_to_markdown(FG_STR(clamped[i]),&b,&e));
        for(k=0;k<b.len&&b.data[k]!='\n';++k) bars+=b.data[k]=='|';
        CHECK(bars==1001);
    }
    CHECK(fg_html_to_markdown(FG_STR("<table><td colspan='1000'>x</td><td colspan='25'>y</td></table>"),
        &b,&e)==FG_LIMIT);
    OK(fg_html_to_markdown((fg_str){NULL,0},&b,&e));CHECK(b.data&&b.len==0);
    fg_buffer_free(&b);return 0;
}

static int xbrl_cases(void)
{
    static const struct {const char *attrs,*text,*value;} cases[]={
        {"","1,234.500","1234.500"},
        {"sign='-' scale='3' decimals='-3'","1,234","-1234e3"},
        {"","($123)","-123"},
        {"scale='-2'","000.125","0.125e-2"},
        {"","&#x2212;42","-42"},
        {"","12&#160;345","12345"},
        {"format='ixt:num-comma-decimal'","1.234,56","1234.56"},
        {"format='ixt:numspacecomma'","1 234,56","1234.56"},
        {"format='ixt:zerodash'","&mdash;","0"},
        {"format='ixt:fixed-zero'","any","0"},
        {"",".50","0.50"},
        {"","12.","12.0"},
        {"scale='3'","1.25e2","1.25e5"},
        {"scale='-3'","1.25E+2","1.25e-1"},
        {"","-",""},
        {"","N/A",""},
        {"xsi:nil='1'","",""},
        {"format='ixt:unsupported-transform'","123",""}
    };
    static const char *bad[]={
        "<ix:nonFraction name='x'>12oops</ix:nonFraction>",
        "<ix:nonFraction name='x' scale='oops'>12</ix:nonFraction>",
        "<ix:nonFraction name='x' decimals='3.5'>12</ix:nonFraction>",
        "<ix:nonFraction name='x' sign='oops'>12</ix:nonFraction>",
        "<ix:nonFraction name='x' xsi:nil='maybe'>12</ix:nonFraction>",
        "<ix:nonFraction name='x'>1.2.3</ix:nonFraction>",
        "<ix:nonFraction name='x'>1e</ix:nonFraction>",
        "<ix:nonFraction name='x'>1e2oops</ix:nonFraction>",
        "<ix:nonFraction name='x'>1",
        "<ix:nonNumeric name='x' continuedAt='missing'>A</ix:nonNumeric>",
        "<ix:nonNumeric name='x' continuedAt='c'>A</ix:nonNumeric><ix:continuation id='c' continuedAt='c'>B</ix:continuation>",
        "<ix:nonNumeric name='x' continuedAt='c'>A</ix:nonNumeric><ix:continuation id='c'>B</ix:continuation><ix:continuation id='c'>C</ix:continuation>",
        "<xbrli:context id='c'><xbrli:period><xbrli:instant>2023-02-29</xbrli:instant></xbrli:period></xbrli:context>",
        "<context id='c'><period><startDate>2024-03-31</startDate><endDate>2024-01-01</endDate></period></context>",
        "<context id='c'><period><instant>2024-01-01</instant><endDate>2024-01-02</endDate></period></context>",
        "<ix:nonNumeric>no name</ix:nonNumeric>"
    };
    size_t i;
    fg_error e={0};
    fg_json *v=NULL;
    char html[1024];
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        snprintf(html,sizeof(html),"<ix:nonFraction name='demo:Value' contextRef='unresolved' unitRef='USD' %s>%s</ix:nonFraction>",
                 cases[i].attrs,cases[i].text);
        OK(fg_xbrl_extract(FG_STR(html),&v,&e));
        CHECK(fg_json_size(v)==1);
        if(cases[i].value[0]) CHECK(number_same(get(fg_json_at(v,0),"value"),cases[i].value));
        else CHECK(fg_json_type(get(fg_json_at(v,0),"value"))==FG_JSON_NULL);
        fg_json_free(v);v=NULL;
    }
    for(i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        CHECK(fg_xbrl_extract(FG_STR(bad[i]),&v,&e)==FG_PARSE);CHECK(v==NULL);
    }
    OK(fg_xbrl_extract(FG_STR("<ix:nonNumeric name='a' contextRef='c'>A<ix:nonNumeric name='b'>B</ix:nonNumeric>C</ix:nonNumeric>"
        "<context id='c'><period><instant>2024-01-01</instant></period></context>"),&v,&e));
    CHECK(fg_json_size(v)==2&&same(get(fg_json_at(v,0),"text"),"ABC"));
    CHECK(same(get(fg_json_at(v,0),"instant"),"2024-01-01"));fg_json_free(v);v=NULL;
    CHECK(fg_xbrl_extract(FG_STR("<ix:nonFraction name='x' scale='99999999999999999999'>1</ix:nonFraction>"),&v,&e)==FG_LIMIT);
    CHECK(v==NULL);
    OK(fg_xbrl_extract(FG_STR("<html>No facts</html>"),&v,&e));CHECK(fg_json_size(v)==0);fg_json_free(v);
    return 0;
}

static int failures(const char *dir)
{
    static const char *bad_tickers[]={
        "[]","{","{\"0\":{\"cik_str\":1.5,\"ticker\":\"X\",\"title\":\"X\"}}",
        "{\"0\":{\"cik_str\":10000000000,\"ticker\":\"X\",\"title\":\"X\"}}",
        "{\"0\":{\"cik_str\":\"42oops\",\"ticker\":\"X\",\"title\":\"X\"}}",
        "{\"0\":{\"cik_str\":42,\"ticker\":\"X\"}}"
    };
    static const char *bad_facts[]={
        "{\"cik\":42,\"entityName\":\"X\",\"facts\":[]}",
        "{\"cik\":42,\"entityName\":\"X\",\"facts\":{\"demo\":{\"x\":{\"units\":{\"USD\":{}}}}}}",
        "{\"cik\":42,\"entityName\":\"X\",\"facts\":{\"demo\":{\"x\":{\"units\":{\"USD\":[{\"val\":null,\"end\":\"2024-01-01\",\"filed\":\"2024-02-01\"}]}}}}}",
        "{\"cik\":42,\"entityName\":\"X\",\"facts\":{\"demo\":{\"x\":{\"units\":{\"USD\":[{\"val\":1,\"end\":\"2024-02-30\",\"filed\":\"2024-03-01\"}]}}}}}",
        "{\"cik\":42,\"entityName\":\"X\",\"facts\":{\"demo\":{\"x\":{\"units\":{\"USD\":[{\"val\":1,\"end\":\"2024-02-01\"}]}}}}}"
    };
    fg_fixture item={"https://www.sec.gov/files/company_tickers.json","{}",200,"",NULL,NULL};
    fg_fixture_transport f={&item,1,0};
    clock_state clock={0};
    fg_client *client=NULL;
    fg_json *v=NULL;
    fg_buffer body={0},sub={0},older={0},company={0};
    fg_error e={0};
    size_t i;
    filing_state filings={0};
    facts_state facts={0};
    OK(make_client(&f,&clock,0,&client,&e));
    CHECK(fg_sec_tickers(client,&v,&e)==FG_INVALID&&f.next==0&&v==NULL);
    fg_client_free(client);client=NULL;
    OK(make_client(&f,&clock,1,&client,&e));
    for(i=0;i<sizeof(bad_tickers)/sizeof(bad_tickers[0]);++i) {
        item.body=bad_tickers[i];f.next=0;
        CHECK(fg_sec_tickers(client,&v,&e)==FG_PARSE&&v==NULL);
    }
    item.status=429;item.body="rate limit";f.next=0;
    CHECK(fg_sec_tickers(client,&v,&e)==FG_HTTP&&e.http_status==429&&v==NULL);
    item.status=302;item.headers="Location: https://example.invalid/";f.next=0;
    CHECK(fg_sec_tickers(client,&v,&e)==FG_HTTP&&f.next==1);
    CHECK(fg_sec_submissions(client,0,&v,&e)==FG_INVALID);
    CHECK(fg_sec_companyfacts(client,UINT64_MAX,&v,&e)==FG_INVALID);
    OK(fg_buffer_set(&body,FG_STR("untouched"),&e));
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR("../evil"),&body,&e)==FG_INVALID);
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR("https://example.invalid/a"),&body,&e)==FG_INVALID);
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR("a%2f..htm"),&body,&e)==FG_INVALID);
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("000000004224000001"),FG_STR("a.htm"),&body,&e)==FG_INVALID);
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),(fg_str){"a\0.htm",6},&body,&e)==FG_INVALID);
    CHECK(!strcmp(body.data,"untouched"));
    item=(fg_fixture){"https://www.sec.gov/Archives/edgar/data/42/000000004224000001/a.htm","gone",404,"",NULL,NULL};f.next=0;
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR("a.htm"),&body,&e)==FG_HTTP);
    CHECK(!strcmp(body.data,"untouched")&&e.http_status==404);
    for(i=0;i<sizeof(bad_facts)/sizeof(bad_facts[0]);++i) {
        OK(fg_json_parse(FG_STR(bad_facts[i]),&v,&e));
        CHECK(fg_sec_companyfacts_extract(v,on_fact,&facts,&e)==FG_PARSE);
        fg_json_free(v);v=NULL;
    }
    OK(load(dir,"submissions.json",&sub,&e));OK(load(dir,"older.json",&older,&e));OK(load(dir,"companyfacts.json",&company,&e));
    {
        fg_fixture pages[]={
            {"https://data.sec.gov/submissions/CIK0000000042.json",sub.data,200,"",NULL,NULL},
            {"https://data.sec.gov/submissions/CIK0000000042-submissions-001.json","older unavailable",503,"",NULL,NULL}
        };
        fg_sec_filings_options options={0};
        fg_str all[]={FG_STR("")};
        f.items=pages;f.count=2;f.next=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_HTTP);
        CHECK(filings.count==1&&e.http_status==503);
        pages[1].status=200;pages[1].body="{bad";f.next=0;filings.count=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_PARSE);
        options.recent_only=1;f.next=0;filings.count=0;
        OK(fg_sec_filings(client,42,&options,on_filing,&filings,&e));CHECK(filings.count==1&&f.next==1);
        options.forms=all;options.form_count=0;f.next=0;filings.count=0;
        OK(fg_sec_filings(client,42,&options,on_filing,&filings,&e));CHECK(filings.count==2);
        options.since=FG_STR("2024-02-30");f.next=0;
        CHECK(fg_sec_filings(client,42,&options,on_filing,&filings,&e)==FG_INVALID&&f.next==0);
        filings.stop=1;f.next=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_IO&&f.next==1);
        filings.stop=0;
        pages[0].body="{\"filings\":{\"recent\":{\"accessionNumber\":[],\"form\":[],\"filingDate\":[\"x\"],\"primaryDocument\":[]}}}";f.next=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_PARSE);
        pages[0].body="{\"filings\":{\"recent\":{\"accessionNumber\":[],\"form\":[],\"filingDate\":[],\"primaryDocument\":[]},\"files\":[{\"name\":\"https://evil.invalid/a\"}]}}";f.next=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_PARSE&&f.next==1);
        pages[0].body="{\"filings\":{\"recent\":{\"accessionNumber\":[],\"form\":[],\"filingDate\":[],\"primaryDocument\":[]},\"files\":[{\"name\":\"CIK0000000042-submissions-dir/page.json\"}]}}";f.next=0;
        CHECK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e)==FG_PARSE&&f.next==1);
    }
    f.items=&item;f.count=1;f.next=0;
    item=(fg_fixture){"https://data.sec.gov/api/xbrl/companyfacts/CIK0000000043.json",company.data,200,"",NULL,NULL};
    CHECK(fg_sec_companyfacts(client,43,&v,&e)==FG_PARSE&&v==NULL);
    OK(fg_json_parse((fg_str){company.data,company.len},&v,&e));
    facts.stop=1;CHECK(fg_sec_companyfacts_extract(v,on_fact,&facts,&e)==FG_IO);fg_json_free(v);
    fg_client_free(client);fg_buffer_free(&body);fg_buffer_free(&sub);fg_buffer_free(&older);fg_buffer_free(&company);
    return 0;
}

typedef struct document_state {
    fg_client *client;
    fg_str expected;
    unsigned count;
} document_state;
static fg_status download_filing(void *user,const fg_sec_filing *f,fg_error *e)
{
    document_state *state=user;
    fg_buffer body={0};
    fg_status s;
    if(!fg_str_equal(f->primary_document,state->expected))
        return fg_fail(e,FG_INVALID,"Unexpected archive document path");
    s=fg_sec_filing_fetch(state->client,f->cik,f->accession,f->primary_document,&body,e);
    if(s==FG_OK && !fg_str_equal((fg_str){body.data,body.len},FG_STR("document")))
        s=fg_fail(e,FG_PARSE,"Unexpected archive document body");
    fg_buffer_free(&body);
    if(s==FG_OK) ++state->count;
    return s;
}
static int document_paths(void)
{
    char too_long[257], payload[4096];
    fg_str paths[]={
        FG_STR("xslF345X03/edgardoc.xml"),
        FG_STR("nested/ASCII_09-1/a..b.htm"),
        FG_STR("../evil"),FG_STR("x/../evil"),FG_STR("./a"),FG_STR("x/./a"),
        FG_STR("."),FG_STR(".."),FG_STR("x/."),FG_STR("x/.."),
        FG_STR("/a"),FG_STR("//evil/a"),FG_STR("x//a"),FG_STR("x/"),
        FG_STR("\\evil"),FG_STR("x\\a"),FG_STR("https://evil.invalid/a"),
        FG_STR("x/a?query"),FG_STR("x/a#fragment"),FG_STR("x/%2e%2e/a"),
        FG_STR("x%2fa"),FG_STR("x/a b"),FG_STR("x/\xc3\xa9"),
        (fg_str){"x/a\0b",5},FG_STR("x/a\nb"),(fg_str){too_long,256}
    };
    fg_fixture items[2]={
        {"https://data.sec.gov/submissions/CIK0000000042.json",payload,200,"",NULL,NULL},
        {NULL,"document",200,"",NULL,NULL}
    };
    fg_fixture_transport transport={items,2,0};
    clock_state clock={0};
    fg_client *client=NULL;
    fg_str form=FG_STR("4");
    fg_sec_filings_options options={0};
    fg_buffer encoded={0},body={0};
    fg_error e={0};
    size_t i;
    memset(too_long,'a',sizeof(too_long));too_long[256]=0;
    OK(make_client(&transport,&clock,1,&client,&e));
    options.forms=&form;options.form_count=1;
    OK(fg_buffer_set(&body,FG_STR("untouched"),&e));
    for(i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
        fg_json *text=NULL;
        filing_state filings={0};
        document_state download={client,paths[i],0};
        char url[400];
        int length;
        OK(fg_json_new_string(paths[i],&text,&e));
        OK(fg_json_serialize(text,&encoded,&e));fg_json_free(text);
        length=snprintf(payload,sizeof(payload),
            "{\"filings\":{\"recent\":{\"accessionNumber\":[\"0000000042-24-000001\","
            "\"0000000042-24-000002\"],\"form\":[\"4\",\"10-Q\"],"
            "\"filingDate\":[\"2024-05-01\",\"2024-05-02\"],"
            "\"primaryDocument\":[%s,\"quarter.htm\"]}}}",encoded.data);
        CHECK(length>0 && (size_t)length<sizeof(payload));
        transport.next=0;
        OK(fg_sec_filings(client,42,NULL,on_filing,&filings,&e));
        CHECK(filings.count==1 && !strcmp(filings.acc[0],"0000000042-24-000002"));
        CHECK(transport.next==1);
        transport.next=0;options.since=FG_STR("");
        if(i<2) {
            snprintf(url,sizeof(url),"https://www.sec.gov/Archives/edgar/data/42/000000004224000001/%.*s",
                     (int)paths[i].len,paths[i].data);
            items[1].url=url;
            OK(fg_sec_filings(client,42,&options,download_filing,&download,&e));
            CHECK(download.count==1 && transport.next==2);
        } else {
            CHECK(fg_sec_filings(client,42,&options,download_filing,&download,&e)==FG_PARSE);
            CHECK(download.count==0 && transport.next==1);
            CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),paths[i],&body,&e)==FG_INVALID);
            CHECK(!strcmp(body.data,"untouched") && transport.next==1);
        }
        transport.next=0;filings.count=0;options.since=FG_STR("2025-01-01");
        OK(fg_sec_filings(client,42,&options,on_filing,&filings,&e));
        CHECK(filings.count==0 && transport.next==1);
    }
    CHECK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR(""),&body,&e)==FG_INVALID);
    /* The accepted bound must fit without truncating the accession URL. */
    {
        char url[400];
        too_long[255]=0;
        snprintf(url,sizeof(url),"https://www.sec.gov/Archives/edgar/data/42/000000004224000001/%s",too_long);
        items[1].url=url;transport.next=1;
        OK(fg_sec_filing_fetch(client,42,FG_STR("0000000042-24-000001"),FG_STR(too_long),&body,&e));
        CHECK(transport.next==2 && !strcmp(body.data,"document"));
    }
    fg_client_free(client);fg_buffer_free(&encoded);fg_buffer_free(&body);
    return 0;
}
static int bounded_inputs(const char *dir)
{
    fg_buffer html={0},md={0};
    fg_json *v=NULL;
    fg_error e={0};
    size_t n;
    OK(load(dir,"filing.htm",&html,&e));
    /* Prefixes deliberately borrow a larger buffer without a terminator at len. */
    for(n=0;n<html.len;n+=7) {
        fg_status s;
        OK(fg_buffer_set(&md,FG_STR("sentinel"),&e));
        s=fg_html_to_markdown((fg_str){html.data,n},&md,&e);
        CHECK(s==FG_OK||s==FG_PARSE);
        if(s!=FG_OK) CHECK(!strcmp(md.data,"sentinel"));
        s=fg_xbrl_extract((fg_str){html.data,n},&v,&e);
        CHECK(s==FG_OK||s==FG_PARSE);
        if(s!=FG_OK) CHECK(v==NULL);
        fg_json_free(v);v=NULL;
    }
    fg_buffer_free(&html);fg_buffer_free(&md);return 0;
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    CHECK(acquisition(argv[1])==0);
    CHECK(html_cases()==0);
    CHECK(xbrl_cases()==0);
    CHECK(failures(argv[1])==0);
    CHECK(document_paths()==0);
    CHECK(bounded_inputs(argv[1])==0);
    puts("SEC acquisition, Markdown, inline XBRL and companyfacts tests passed");
    return 0;
}

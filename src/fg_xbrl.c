#include "fg_sec_internal.h"
#include <stdio.h>
#include <string.h>

static const fg_json *get(const fg_json *v,const char *key) {return fg_json_get(v,FG_STR(key));}
static fg_status set_bool(fg_json *v,const char *key,int b,fg_error *e)
{
    fg_json *n=NULL;
    fg_status s=fg_json_new_bool(b,&n,e);
    return s==FG_OK?fg_sec_set_value(v,key,n,e):s;
}
static fg_status attr_string(fg_json *v,const char *key,const fg_html_tag *t,
                             const char *attr,fg_error *e)
{
    fg_buffer decoded={0};
    fg_status s=fg_html_entities(fg_html_attr(t,attr,NULL),&decoded,e);
    if(s==FG_OK) s=fg_sec_set_string(v,key,(fg_str){decoded.data,decoded.len},e);
    fg_buffer_free(&decoded);return s;
}
static int begins(fg_str s,const char *prefix)
{
    size_t n=strlen(prefix);
    return s.len>=n && !memcmp(s.data,prefix,n);
}
static int markup(fg_str s,size_t i)
{
    unsigned char c;
    if(i+1>=s.len||s.data[i]!='<') return 0;
    c=(unsigned char)s.data[i+1];
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='/'||c=='!'||c=='?';
}
static fg_status scan_tag(fg_str html,size_t *pos,fg_html_tag *t,int *found,fg_error *e)
{
    while(*pos<html.len) {
        fg_status s;
        size_t ignored;
        if(!markup(html,*pos)) {++*pos;continue;}
        if((s=fg_html_tag_read(html,*pos,t,e))!=FG_OK) return s;
        *pos=t->end;
        if(t->special) continue;
        if(!t->closing && (fg_html_is(t,"script")||fg_html_is(t,"style"))) {
            if((s=fg_html_close(html,t,&ignored,pos,e))!=FG_OK) return s;
            continue;
        }
        *found=1;return FG_OK;
    }
    *found=0;return FG_OK;
}
static fg_status date_field(fg_json *obj,const char *key,fg_str html,fg_error *e)
{
    fg_buffer b={0};
    int64_t epoch=0;
    fg_status s;
    if(get(obj,key)) return fg_fail(e,FG_PARSE,"Duplicate XBRL period date");
    s=fg_html_text(html,&b,e);
    if(s==FG_OK) s=fg_sec_date((fg_str){b.data,b.len},&epoch,e);
    if(s==FG_OK) s=fg_sec_set_string(obj,key,(fg_str){b.data,b.len},e);
    if(s==FG_OK) s=fg_sec_set_i64(obj,!strcmp(key,"start")?"period_start":"period_end",epoch,e);
    fg_buffer_free(&b);return s;
}
static fg_status context_parse(fg_str html,const fg_html_tag *tag,fg_json **out,
                               size_t *after,fg_error *e)
{
    size_t end,pos=tag->end,period_depth=0;
    fg_json *obj=NULL;
    fg_status s=fg_html_close(html,tag,&end,after,e);
    int found;
    if(s!=FG_OK) return s;
    if((s=fg_json_new(FG_JSON_OBJECT,&obj,e))!=FG_OK) return s;
    while(pos<end) {
        fg_html_tag t;
        size_t content_end,next;
        const char *key=NULL;
        if((s=scan_tag(fg_sec_slice(html,0,end),&pos,&t,&found,e))!=FG_OK || !found) break;
        if(fg_html_local_is(&t,"period")) {
            if(!t.closing) ++period_depth;
            else if(period_depth) --period_depth;
            continue;
        }
        if(t.closing||!period_depth) continue;
        if(fg_html_local_is(&t,"startDate")) key="start";
        else if(fg_html_local_is(&t,"endDate")) key="end";
        else if(fg_html_local_is(&t,"instant")) key="instant";
        else if(fg_html_local_is(&t,"forever")) {
            s=set_bool(obj,"forever",1,e);
            if(s!=FG_OK) break;
        }
        if(!key) continue;
        s=fg_html_close(html,&t,&content_end,&next,e);
        if(s==FG_OK) s=date_field(obj,key,fg_sec_slice(html,t.end,content_end),e);
        if(s!=FG_OK) break;
        pos=next;
    }
    if(s==FG_OK) {
        int instant=get(obj,"instant")!=NULL, start=get(obj,"start")!=NULL,
            finish=get(obj,"end")!=NULL, forever=get(obj,"forever")!=NULL;
        if((instant && (start||finish||forever)) || (forever && (start||finish)) ||
           start!=finish || (!instant&&!start&&!forever))
            s=fg_fail(e,FG_PARSE,"Invalid XBRL context period");
        else if(start) {
            int64_t a=0,b=0;
            s=fg_json_i64(get(obj,"period_start"),&a,e);
            if(s==FG_OK) s=fg_json_i64(get(obj,"period_end"),&b,e);
            if(s==FG_OK && a>b) s=fg_fail(e,FG_PARSE,"XBRL period starts after end");
        }
    }
    if(s==FG_OK) *out=obj;else fg_json_free(obj);
    return s;
}
static fg_status put_id(fg_json *map,const fg_html_tag *t,fg_json *value,fg_error *e)
{
    fg_buffer id={0};
    fg_status s=fg_html_entities(fg_html_attr(t,"id",NULL),&id,e);
    fg_str key={id.data,id.len};
    if(s==FG_OK && (!id.len||fg_json_get(map,key))) s=fg_fail(e,FG_PARSE,"Missing or duplicate XBRL id");
    if(s==FG_OK) s=fg_json_set(map,key,value,e);
    if(s!=FG_OK) fg_json_free(value);
    fg_buffer_free(&id);return s;
}
static fg_status indexes(fg_str html,fg_json *contexts,fg_json *continuations,fg_error *e)
{
    size_t pos=0;
    int found;
    fg_status s;
    for(;;) {
        fg_html_tag t;
        fg_json *value=NULL;
        if((s=scan_tag(html,&pos,&t,&found,e))!=FG_OK||!found) return s;
        if(t.closing) continue;
        if(fg_html_local_is(&t,"context")) {
            if((s=context_parse(html,&t,&value,&pos,e))!=FG_OK) return s;
            if((s=put_id(contexts,&t,value,e))!=FG_OK) return s;
        } else if(fg_html_is(&t,"ix:continuation")) {
            s=fg_json_new_u64((uint64_t)t.start,&value,e);
            if(s!=FG_OK) return s;
            if((s=put_id(continuations,&t,value,e))!=FG_OK) return s;
        }
    }
}
static fg_status collect_text(fg_str html,const fg_html_tag *tag,const fg_json *continuations,
                              fg_buffer *out,fg_error *e)
{
    fg_html_tag current=*tag;
    fg_json *seen=NULL;
    fg_status s=fg_json_new(FG_JSON_OBJECT,&seen,e);
    size_t links=0;
    if(s!=FG_OK) return s;
    for(;;) {
        size_t end,after;
        fg_buffer id={0};
        fg_str key;
        const fg_json *offset;
        uint64_t pos;
        fg_json *mark=NULL;
        if((s=fg_html_close(html,&current,&end,&after,e))!=FG_OK) break;
        s=fg_html_text(fg_sec_slice(html,current.end,end),out,e);
        if(s!=FG_OK) break;
        s=fg_html_entities(fg_html_attr(&current,"continuedAt",NULL),&id,e);
        key=(fg_str){id.data,id.len};
        if(s!=FG_OK || !id.len) {fg_buffer_free(&id);break;}
        offset=fg_json_get(continuations,key);
        if(!offset||fg_json_get(seen,key)||++links>1024) {
            fg_buffer_free(&id);
            s=fg_fail(e,FG_PARSE,"Missing, cyclic or excessive XBRL continuation");break;
        }
        s=fg_json_new_bool(1,&mark,e);
        if(s==FG_OK) s=fg_json_set(seen,key,mark,e);
        fg_buffer_free(&id);
        if(s!=FG_OK) {fg_json_free(mark);break;}
        s=fg_json_u64(offset,&pos,e);
        if(s!=FG_OK) break;
        if(pos>=html.len) {s=fg_fail(e,FG_PARSE,"Invalid continuation offset");break;}
        s=fg_html_tag_read(html,(size_t)pos,&current,e);
        if(s!=FG_OK) break;
        if(out->len && (s=fg_buffer_append(out," ",1,e))!=FG_OK) break;
    }
    fg_json_free(seen);return s;
}
static fg_status signed_integer(fg_str text,int *out,fg_error *e)
{
    size_t i=0;
    unsigned n=0;
    int neg=0;
    if(!text.len) {*out=0;return FG_OK;}
    if(text.data[i]=='-'||text.data[i]=='+') neg=text.data[i++]=='-';
    if(i==text.len) return fg_fail(e,FG_PARSE,"Invalid XBRL integer attribute");
    for(;i<text.len;++i) {
        if(text.data[i]<'0'||text.data[i]>'9')
            return fg_fail(e,FG_PARSE,"Invalid XBRL integer attribute");
        if(n>100000) return fg_fail(e,FG_LIMIT,"XBRL integer attribute exceeds limit");
        n=n*10+(unsigned)(text.data[i]-'0');
    }
    if(n>100000) return fg_fail(e,FG_LIMIT,"XBRL scale/precision exceeds 100000");
    *out=neg?-(int)n:(int)n;return FG_OK;
}
static fg_str local_name(fg_str text)
{
    size_t i;
    for(i=0;i<text.len;++i) if(text.data[i]==':') return fg_sec_slice(text,i+1,text.len);
    return text;
}
static fg_status numeric_value(fg_json *fact,int nil,fg_error *e)
{
    fg_str text=fg_sec_trim(fg_json_string(get(fact,"text")));
    fg_str scale=fg_json_string(get(fact,"scale")), sign=fg_json_string(get(fact,"sign"));
    fg_str decimals=fg_json_string(get(fact,"decimals"));
    fg_str format=local_name(fg_json_string(get(fact,"format")));
    fg_buffer clean={0}, number={0};
    fg_json *value=NULL;
    int power=0, precision, negative=0, dot=0, comma=0, dashzero=0, supported=1;
    size_t i=0, first=0;
    fg_status s;
    if((s=signed_integer(scale,&power,e))!=FG_OK) return s;
    if(decimals.len&&!fg_sec_iequal(decimals,FG_STR("INF")) &&
       (s=signed_integer(decimals,&precision,e))!=FG_OK) return s;
    if(sign.len&&!fg_str_equal(sign,FG_STR("-"))&&!fg_str_equal(sign,FG_STR("+")))
        return fg_fail(e,FG_PARSE,"Invalid XBRL sign");
    if(format.len) {
        if(fg_sec_iequal(format,FG_STR("numcommadecimal"))||
           fg_sec_iequal(format,FG_STR("num-comma-decimal"))||
           fg_sec_iequal(format,FG_STR("numdotcomma"))) comma=1;
        else if(fg_sec_iequal(format,FG_STR("zerodash"))||
                fg_sec_iequal(format,FG_STR("numdash"))||
                fg_sec_iequal(format,FG_STR("fixed-zero"))) dashzero=1;
        else if(!fg_sec_iequal(format,FG_STR("numdotdecimal"))&&
                !fg_sec_iequal(format,FG_STR("num-dot-decimal"))&&
                !fg_sec_iequal(format,FG_STR("numcommadot"))&&
                !fg_sec_iequal(format,FG_STR("numspacedot"))&&
                !fg_sec_iequal(format,FG_STR("numspacecomma"))) supported=0;
        if(fg_sec_iequal(format,FG_STR("numspacecomma"))) comma=1;
    }
    if(nil||!text.len||!supported||fg_sec_iequal(text,FG_STR("N/A"))) goto null_value;
    if(fg_str_equal(text,FG_STR("-"))||fg_str_equal(text,FG_STR("\xe2\x80\x94"))||
       fg_str_equal(text,FG_STR("\xe2\x80\x93"))) {
        if(!dashzero) goto null_value;
        text=FG_STR("0");
    } else if(dashzero) {
        if(fg_sec_iequal(format,FG_STR("fixed-zero"))) text=FG_STR("0");
        else {s=fg_fail(e,FG_PARSE,"Invalid XBRL zero-dash value");goto done;}
    }
    if(text.len>=2&&text.data[0]=='('&&text.data[text.len-1]==')') {
        negative=1;text=fg_sec_trim(fg_sec_slice(text,1,text.len-1));
    }
    if(text.len&&(text.data[0]=='-'||text.data[0]=='+')) {
        if(text.data[0]=='-') negative=1;
        ++text.data;--text.len;
    } else if(begins(text,"\xe2\x88\x92")) {
        negative=1;text.data+=3;text.len-=3;
    }
    for(i=0;i<text.len;++i) {
        unsigned char c=(unsigned char)text.data[i];
        char out=(char)c;
        if(c=='e'||c=='E') {
            int exponent=0;
            fg_str suffix=fg_sec_slice(text,i+1,text.len);
            if(!suffix.len || !clean.len) {
                s=fg_fail(e,FG_PARSE,"Invalid XBRL numeric exponent");goto done;
            }
            if((s=signed_integer(suffix,&exponent,e))!=FG_OK) goto done;
            power+=exponent;
            if(power>100000||power< -100000) {
                s=fg_fail(e,FG_LIMIT,"XBRL scaled exponent exceeds 100000");goto done;
            }
            break;
        }
        if(fg_sec_space(c)||c=='$'||c=='%'||(comma?c=='.':c==',')) continue;
        if(c==0xc2&&i+1<text.len&&(unsigned char)text.data[i+1]==0xa0) {++i;continue;}
        if(c==(unsigned char)(comma?',':'.')) {
            if(dot++) {s=fg_fail(e,FG_PARSE,"Multiple XBRL decimal points");goto done;}
            out='.';
        } else if(c<'0'||c>'9') {s=fg_fail(e,FG_PARSE,"Invalid XBRL numeric text");goto done;}
        if((s=fg_buffer_append(&clean,&out,1,e))!=FG_OK) goto done;
    }
    if(!clean.len || (clean.len==1&&clean.data[0]=='.')) {
        s=fg_fail(e,FG_PARSE,"Empty XBRL number");goto done;
    }
    if(fg_str_equal(sign,FG_STR("-"))) negative=!negative;
    if(negative && (s=fg_buffer_append(&number,"-",1,e))!=FG_OK) goto done;
    while(first+1<clean.len&&clean.data[first]=='0'&&clean.data[first+1]!='.') ++first;
    if(clean.data[first]=='.' && (s=fg_buffer_append(&number,"0",1,e))!=FG_OK) goto done;
    if((s=fg_buffer_append(&number,clean.data+first,clean.len-first,e))!=FG_OK) goto done;
    if(number.data[number.len-1]=='.' && (s=fg_buffer_append(&number,"0",1,e))!=FG_OK) goto done;
    if(power) {
        char exponent[32];
        snprintf(exponent,sizeof(exponent),"e%d",power);
        if((s=fg_buffer_append(&number,exponent,strlen(exponent),e))!=FG_OK) goto done;
    }
    s=fg_json_new_number((fg_str){number.data,number.len},&value,e);
    goto assign;
null_value:
    s=fg_json_new(FG_JSON_NULL,&value,e);
assign:
    if(s==FG_OK) {s=fg_sec_set_value(fact,"value",value,e);value=NULL;}
done:
    fg_json_free(value);fg_buffer_free(&clean);fg_buffer_free(&number);return s;
}
static fg_status context_copy(fg_json *fact,const fg_json *context,fg_error *e)
{
    static const char *strings[]={"start","end","instant"};
    static const char *times[]={"period_start","period_end"};
    size_t i;
    fg_status s;
    for(i=0;i<sizeof(strings)/sizeof(strings[0]);++i)
        if(get(context,strings[i]) &&
           (s=fg_sec_set_string(fact,strings[i],fg_json_string(get(context,strings[i])),e))!=FG_OK) return s;
    for(i=0;i<sizeof(times)/sizeof(times[0]);++i) if(get(context,times[i])) {
        int64_t value;
        if((s=fg_json_i64(get(context,times[i]),&value,e))!=FG_OK ||
           (s=fg_sec_set_i64(fact,times[i],value,e))!=FG_OK) return s;
    }
    if(get(context,"forever")) return set_bool(fact,"forever",1,e);
    return FG_OK;
}
fg_status fg_xbrl_extract(fg_str html,fg_json **out,fg_error *e)
{
    fg_json *facts=NULL,*contexts=NULL,*continuations=NULL,*fact=NULL;
    fg_status s;
    size_t pos=0;
    int found;
    if(!out) return fg_fail(e,FG_INVALID,"Missing XBRL output");
    *out=NULL;
    if((s=fg_sec_utf8(html,e))!=FG_OK) return s;
    if((s=fg_json_new(FG_JSON_ARRAY,&facts,e))!=FG_OK ||
       (s=fg_json_new(FG_JSON_OBJECT,&contexts,e))!=FG_OK ||
       (s=fg_json_new(FG_JSON_OBJECT,&continuations,e))!=FG_OK) goto done;
    if((s=indexes(html,contexts,continuations,e))!=FG_OK) goto done;
    for(;;) {
        fg_html_tag t;
        fg_buffer text={0};
        int numeric,nil=0;
        fg_str nil_text;
        const fg_json *context;
        if((s=scan_tag(html,&pos,&t,&found,e))!=FG_OK||!found) break;
        numeric=fg_html_is(&t,"ix:nonfraction");
        if(t.closing||(!numeric&&!fg_html_is(&t,"ix:nonnumeric"))) continue;
        if((s=fg_json_new(FG_JSON_OBJECT,&fact,e))!=FG_OK) break;
        if((s=attr_string(fact,"name",&t,"name",e))!=FG_OK ||
           (s=attr_string(fact,"context",&t,"contextRef",e))!=FG_OK ||
           (s=attr_string(fact,"unit",&t,"unitRef",e))!=FG_OK ||
           (s=attr_string(fact,"decimals",&t,"decimals",e))!=FG_OK ||
           (s=attr_string(fact,"scale",&t,"scale",e))!=FG_OK ||
           (s=attr_string(fact,"sign",&t,"sign",e))!=FG_OK ||
           (s=attr_string(fact,"format",&t,"format",e))!=FG_OK) break;
        if(!fg_json_string(get(fact,"name")).len) {
            s=fg_fail(e,FG_PARSE,"Missing inline XBRL concept name");break;
        }
        nil_text=fg_html_attr(&t,"xsi:nil",NULL);
        if(nil_text.len) {
            if(fg_sec_iequal(nil_text,FG_STR("true"))||fg_str_equal(nil_text,FG_STR("1"))) nil=1;
            else if(!fg_sec_iequal(nil_text,FG_STR("false"))&&!fg_str_equal(nil_text,FG_STR("0"))) {
                s=fg_fail(e,FG_PARSE,"Invalid XBRL nil attribute");break;
            }
        }
        s=collect_text(html,&t,continuations,&text,e);
        if(s==FG_OK) s=fg_sec_set_string(fact,"text",(fg_str){text.data,text.len},e);
        fg_buffer_free(&text);
        if(s!=FG_OK || (s=set_bool(fact,"numeric",numeric,e))!=FG_OK ||
           (s=set_bool(fact,"nil",nil,e))!=FG_OK) break;
        context=fg_json_get(contexts,fg_json_string(get(fact,"context")));
        if(context && (s=context_copy(fact,context,e))!=FG_OK) break;
        if(numeric) s=numeric_value(fact,nil,e);
        else {
            fg_json *value=NULL;
            s=fg_json_new(FG_JSON_NULL,&value,e);
            if(s==FG_OK) s=fg_sec_set_value(fact,"value",value,e);
        }
        if(s!=FG_OK) break;
        if((s=fg_json_append(facts,fact,e))!=FG_OK) break;
        fact=NULL;
    }
done:
    fg_json_free(contexts);fg_json_free(continuations);fg_json_free(fact);
    if(s==FG_OK) *out=facts;else fg_json_free(facts);
    return s;
}

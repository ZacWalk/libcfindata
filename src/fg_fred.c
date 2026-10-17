#include "fg_sources_internal.h"

fg_status fg_fred_observations(fg_client *c, const fg_fred_options *o,
    fg_fred_callback callback, void *user, fg_error *e)
{
    fg_buffer base = {0}, url = {0};
    fg_json *root = NULL;
    fg_status s = FG_OK;
    uint64_t offset = 0, total = 0, expected = 0;
    size_t pages = 0;
    int64_t start = INT64_MIN, end = INT64_MAX;
    fg_header accept = {FG_STR("Accept"),FG_STR("application/json")};
    if (!c || !o || !callback || !fg_s_valid(o->series_id,1) || !fg_s_valid(o->api_key,1) ||
        !fg_s_valid(o->observation_start,0) || !fg_s_valid(o->observation_end,0) ||
        (o->vintage != FG_FRED_FIRST_RELEASE && o->vintage != FG_FRED_LATEST))
        return fg_fail(e,FG_INVALID,"Invalid FRED options");
    if ((o->observation_start.len && fg_s_date(o->observation_start,&start,NULL) != FG_OK) ||
        (o->observation_end.len && fg_s_date(o->observation_end,&end,NULL) != FG_OK) || start > end)
        return fg_fail(e,FG_INVALID,"Invalid FRED observation window");
#define TRY(x) do { s=(x); if(s!=FG_OK) goto done; } while(0)
    TRY(fg_buffer_set(&base,FG_STR("https://api.stlouisfed.org/fred/series/observations?file_type=json&limit=100000&sort_order=asc"),e));
    TRY(fg_s_query(&base,"series_id",o->series_id,e));
    TRY(fg_s_query(&base,"api_key",o->api_key,e));
    if (o->observation_start.len) TRY(fg_s_query(&base,"observation_start",o->observation_start,e));
    if (o->observation_end.len) TRY(fg_s_query(&base,"observation_end",o->observation_end,e));
    if (o->vintage == FG_FRED_FIRST_RELEASE)
        TRY(fg_buffer_append(&base,"&output_type=4&realtime_start=1776-07-04&realtime_end=9999-12-31",
            strlen("&output_type=4&realtime_start=1776-07-04&realtime_end=9999-12-31"),e));
    else TRY(fg_s_query(&base,"output_type",FG_STR("1"),e));
    do {
        const fg_json *rows;
        uint64_t got_offset, limit;
        size_t i, n;
        if (++pages > 5000) { s = fg_fail(e,FG_LIMIT,"FRED page limit exceeded"); goto done; }
        TRY(fg_buffer_set(&url,fg_s_view(&base),e));
        TRY(fg_s_uquery(&url,"offset",offset,e));
        TRY(fg_s_fetch(c,fg_s_view(&url),&accept,1,&root,e));
        rows = fg_s_get(root,"observations");
        if (fg_json_type(root) != FG_JSON_OBJECT || fg_json_type(rows) != FG_JSON_ARRAY ||
            fg_json_u64(fg_s_get(root,"count"),&total,NULL) != FG_OK ||
            fg_json_u64(fg_s_get(root,"offset"),&got_offset,NULL) != FG_OK ||
            fg_json_u64(fg_s_get(root,"limit"),&limit,NULL) != FG_OK ||
            got_offset != offset || !limit || limit > 100000 ||
            (offset && total != expected)) {
            s = fg_fail(e,FG_PARSE,"Invalid FRED pagination metadata"); goto done;
        }
        n = fg_json_size(rows); expected = total;
        if (total < offset || n > limit || (uint64_t)n > total-offset || (!n && total != offset)) {
            s = fg_fail(e,FG_PARSE,"Inconsistent FRED observation count"); goto done;
        }
        for (i=0;i<n;++i) {
            const fg_json *row = fg_json_at(rows,i);
            fg_fred_observation record = {0};
            fg_str date;
            record.series_id = o->series_id;
            TRY(fg_s_string(row,"date",0,&date,e));
            TRY(fg_s_date(date,&record.observation_date,e));
            TRY(fg_s_string(row,"realtime_start",0,&date,e));
            TRY(fg_s_date(date,&record.realtime_start,e));
            TRY(fg_s_string(row,"realtime_end",0,&date,e));
            TRY(fg_s_date(date,&record.realtime_end,e));
            if (record.realtime_end < record.realtime_start ||
                record.observation_date < start || record.observation_date > end) {
                s = fg_fail(e,FG_PARSE,"FRED observation date outside requested window"); goto done;
            }
            TRY(fg_s_string(row,"value",0,&record.value_text,e));
            record.has_value = !fg_s_equal(record.value_text,".");
            if (record.has_value) TRY(fg_s_decimal(record.value_text,&record.value,e));
            record.is_first_release = o->vintage == FG_FRED_FIRST_RELEASE;
            record.has_knowledge_time = record.is_first_release;
            if (record.has_knowledge_time) record.knowledge_time = record.realtime_start;
            TRY(callback(user,&record,e));
        }
        offset += (uint64_t)n;
        fg_json_free(root); root = NULL;
    } while (offset < total);
done:
    fg_json_free(root); fg_buffer_free(&base); fg_buffer_free(&url);
    return s;
#undef TRY
}

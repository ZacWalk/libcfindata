#ifndef FG_SOURCES_H
#define FG_SOURCES_H

#include "fg_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous acquisition only: no environment, filesystem, storage, analysis,
   or implicit clock-based date windows. Network functions use the supplied
   client. Injected transports MUST NOT follow redirects or manage cookies.
   Callback records/strings borrow memory until return; copy anything retained.
   A failed callback stops immediately. Earlier callbacks are not rolled back.
   Raw JSON outputs own their trees (fg_json_free); *out is NULL on failure.
   Provider/transport errors never include response bodies, URLs or credentials.
   Every fg_str honors its length; URL inputs reject NUL/control bytes. */

typedef enum fg_fred_vintage {
    FG_FRED_FIRST_RELEASE = 0, FG_FRED_LATEST = 1
} fg_fred_vintage;
typedef struct fg_fred_options {
    fg_str series_id, api_key;
    fg_str observation_start, observation_end; /* Optional YYYY-MM-DD. */
    fg_fred_vintage vintage;
} fg_fred_options;
typedef struct fg_fred_observation {
    fg_str series_id, value_text;
    int64_t observation_date, realtime_start, realtime_end;
    double value; /* Approximate; use value_text for exact decimals. */
    int has_value; /* FRED "." is missing, not zero. */
    int is_first_release;
    /* Only FIRST_RELEASE identifies publication time. LATEST realtime_start
       is the provider's real-time query window, NOT an original release date. */
    int has_knowledge_time;
    int64_t knowledge_time;
} fg_fred_observation;
typedef fg_status (*fg_fred_callback)(void *, const fg_fred_observation *, fg_error *);
/* FIRST_RELEASE always output_type=4, full ALFRED window, limit=100000,
   offset pagination (at most 5000 pages). Never falls back to revised observations. LATEST is
   explicit and not suitable as original-release backtest data. */
fg_status fg_fred_observations(fg_client *, const fg_fred_options *,
                              fg_fred_callback, void *, fg_error *);

typedef struct fg_positioning_options {
    char delimiter; /* Zero/comma: RFC4180 CSV; tab: quoted TSV adapter. */
    fg_str known_date_column; /* Optional exact header, YYYY-MM-DD values. */
    int has_known_at;
    int64_t known_at; /* Explicit snapshot time, only for missing known date. */
    fg_str market_contains; /* CFTC only; empty includes all markets. */
} fg_positioning_options;
typedef struct fg_cftc_record {
    fg_str market;
    int64_t report_date, knowledge_time;
    int has_knowledge_time;
    uint64_t open_interest, dealer_long, dealer_short, asset_mgr_long,
             asset_mgr_short, lev_money_long, lev_money_short;
    /* Bits 0..5 correspond to the six participant fields above. Missing
       columns/empty cells have clear bits, not invented zero positions. */
    unsigned present;
} fg_cftc_record;
typedef struct fg_finra_record {
    int64_t month_date, knowledge_time; /* Month is first day, midnight UTC. */
    int has_knowledge_time, has_debit_balances;
    double debit_balances; /* USD millions; approximate. */
    fg_str debit_text; /* Original field, may include thousands separators. */
} fg_finra_record;
typedef fg_status (*fg_cftc_callback)(void *, const fg_cftc_record *, fg_error *);
typedef fg_status (*fg_finra_callback)(void *, const fg_finra_record *, fg_error *);
/* CFTC: legacy TFF Futures Only named columns. FINRA: legacy first two
   columns YYYY-MM and debit balance (or YYYY-MM-01). Grouped numbers accepted.
   Required bad fields fail; blank FINRA balances are explicitly missing.
   No 3-day/month lag is guessed. Supply a real known-date column or snapshot.
   Known times earlier than the report/month date are rejected.
   Input order is preserved, no analysis/rolling normalization is performed. */
fg_status fg_cftc_parse(fg_str, const fg_positioning_options *, fg_cftc_callback, void *, fg_error *);
fg_status fg_finra_parse(fg_str, const fg_positioning_options *, fg_finra_callback, void *, fg_error *);
/* Explicit HTTPS public URL only, no credentials/redirects. Body must be
   uncompressed CSV/TSV in the formats above, not ZIP/XLSX or HTML. Downloaded
   historical CFTC ZIP members can be passed to parse after caller extraction.
   No automatic archive dependency or scraping of report landing pages. */
fg_status fg_cftc_fetch(fg_client *, fg_str, const fg_positioning_options *, fg_cftc_callback, void *, fg_error *);
fg_status fg_finra_fetch(fg_client *, fg_str, const fg_positioning_options *, fg_finra_callback, void *, fg_error *);

#ifdef __cplusplus
}
#endif
#endif

# libcfindata

[![CI](https://github.com/ZacWalk/libcfindata/actions/workflows/ci.yml/badge.svg)](https://github.com/ZacWalk/libcfindata/actions/workflows/ci.yml)

A C11 financial-data acquisition library and example CLI for Windows and Linux.
Callers choose how to store and use acquired records.

Licensed under [MIT](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md)
for the vendored build tool, external dependencies and provider-data boundaries.

## Sources

| Source | Acquisition |
|---|---|
| SEC EDGAR | Ticker/CIK map, recent and historical submissions, filing documents, HTML-to-Markdown, inline XBRL, and the distinct companyfacts API |
| FRED / ALFRED | Paginated first-release observations by default; revised/latest observations only by explicit request |
| CFTC / FINRA | Public CSV/TSV acquisition and parsing, with explicit publication-date provenance |

The library retains exact JSON number lexemes, integer timestamps/IDs, and
publication/knowledge times separately from observation, period or report
dates. Missing observations are not silently converted to zero.

## Build and test

Requirements: x64 Windows or Linux, PowerShell 7.4+, CMake 3.24+, Ninja,
MSVC or GCC. [dd v0.2.2](https://github.com/ZacWalk/dd/tree/v0.2.2) is vendored
unchanged. There is no MSBuild solution or bespoke compiler-discovery driver.

**Windows:** MSVC and the Windows SDK are sufficient; WinHTTP comes from the SDK.
No vcpkg, archive/hash package restore or dependency DLL copying is needed.
The consumer still chooses its MSVC CRT.

When updating an existing build, run `cmake --fresh --preset windows-release`
and `cmake --fresh --preset windows-debug` in an x64 developer terminal to clear
the retired package toolchain from the cache. Do the same for `windows-asan`
if it was configured previously. This does not delete data or package caches.

**Ubuntu:** install `g++ cmake ninja-build libcurl4-openssl-dev`, plus PowerShell.
CMake uses system libcurl.
dd checks for G++ even though this library only compiles C.
Dependency acquisition can require network; the test suite does not use external
services or credentials.

```powershell
.\dd.ps1 dep install --non-interactive
.\dd.ps1 doctor --non-interactive
.\dd.ps1 build                     # Release and Debug
.\dd.ps1 test                      # 10 CTests in each configuration
.\dd.ps1 test --app cfindata         # acquisition/CLI tests only
.\dd.ps1 asan --dry-run
.\dd.ps1 asan --yes                # 9 offline/loopback tests; separate Debug tree
.\dd.ps1 run -- --help
.\dd.ps1 build release --app cfindata
.\dd.ps1 clean both --dry-run
```

Run from the repository or pass `--project PATH`. Native outputs are in
`build/<x64-windows|x64-linux>/<debug|release>/`: `cfindata.lib` / `libcfindata.a`
and `cfindata.exe` / `cfindata`. Library targets are buildable/testable, not runnable.
Sanitizers use a separate `asan/` tree: ASan on MSVC, ASan plus fatal UBSan on GCC.
The asan project command reuses dd's build/test helpers and separate state keys.
Standard `clean` does not remove sanitizer trees, dependency caches or `data/`.

CI runs the same lifecycle on Windows and Linux for pushes, pull requests and
manual dispatch: validate pins, doctor, both configurations, then sanitizers.
Windows requires no package restore; Linux installs its transport development
package. Tests use only synthetic fixtures and bounded loopback requests. Checkout credentials are
not retained. Scratch/logs stay under `tmp/`, with failure-only log and test-report
artifacts retained for seven days; no credentials, executables or data are uploaded.

For CMake-only consumers, `add_subdirectory` and link target `cfindata`;
`include/` is public. `-DBUILD_TESTING=OFF` omits test programs.
No runtime service is started by the library.

```cmake
set(BUILD_TESTING OFF CACHE BOOL "Build tests")
add_subdirectory(libcfindata)
target_link_libraries(your_target PRIVATE cfindata)
```

The current distribution is a static library with an example CLI. It does not
yet provide an installed CMake package or a shared-library ABI.

## Example CLI

Prefix commands below with `.\dd.ps1 run --` or invoke the built `cfindata` binary.
`--help` lists all arguments, limits, output formats and credential precedence.
Unix timestamp arguments are UTC seconds.

```text
sec map --ticker AAPL --user-agent "Your application contact@example.org"
sec companyfacts --cik 320193 --user-agent "Your application contact@example.org"
sec filings --cik 320193 --since 2024-01-01 --user-agent "Your application contact@example.org"
fred observations --series JTSJOL --start 2023-01-01 --end 2023-01-31
cftc --url https://YOUR-PROVIDER/tff.csv --known-date-column published
finra --file margin.csv --known-at 1706659200
convert --file filing.htm
xbrl --file filing.htm
```

Set `SEC_USER_AGENT` and `FRED_API_KEY` in the calling process, or use the documented explicit
flags. `.env` loading is opt-in through `--env-dir ABSOLUTE-DIRECTORY`; the
library never reads credentials implicitly. Do not put secrets in shell history
or source control. Local conversion commands need no credentials.

Records are compact JSON/JSONL, except explicit Markdown output. Streamed records
are not rolled back if a later page fails. Nonzero exits distinguish invalid
input, parse/I/O/HTTP errors, limits and missing records;
errors never dump credential-bearing request URLs or provider bodies.

### Important boundaries

- SEC requires a real configurable User-Agent and spaces request starts by at
  least 120 ms. Filing/XBRL and companyfacts are independent APIs. Companyfacts
  knowledge time is `filed`, not the accounting period end.
- ALFRED first-release mode never falls back to revised observations.
- CFTC/FINRA accept uncompressed CSV/TSV and explicit publication metadata.
  They do not scrape landing pages or decode arbitrary XLSX/historical CFTC ZIPs.
  Extract those externally before calling the pure parsers.
- Unsupported inline-XBRL transformations produce `value:null` with original
  text/format metadata; they are not guessed. Markdown nested tables flatten.

## C API and ownership

Start with [fg.h](include/fg.h), [fg_data.h](include/fg_data.h),
[fg_sec.h](include/fg_sec.h), [fg_sources.h](include/fg_sources.h), and
[fg_source_utils.h](include/fg_source_utils.h). Headers document each callback and result.

```c
#include "fg.h"
#include "fg_sec.h"

fg_error error = {0};
fg_client *client = NULL;
fg_json *facts = NULL;
fg_client_options options = {0};
options.user_agent = FG_STR("Your application contact@example.org");
fg_status status = fg_client_create(NULL, &options, &client, &error);
if (status == FG_OK)
    status = fg_sec_companyfacts(client, 320193, &facts, &error);
/* Check status before consuming facts. */
fg_json_free(facts);
fg_client_free(client);
```

`fg_str` borrows UTF-8 bytes with an explicit length. Initialize owned buffers
and responses to `{0}` and release them using their matching free function.
JSON nodes own copies; callback records are borrowed until return. A client
with a NULL transport owns its default transport; an injected `fg_transport`
and its user context must outlive the client. Handles are single-threaded.

Injected `get`/`post` callbacks make fixture replay possible and let applications
adapt an existing transport without making this C library depend on it. Native
transports validate TLS, bound responses, disable redirects and never keep a
cookie jar. HTTP is allowed only for
loopback testing; public acquisition requires HTTPS.

### Application-owned sources

Applications can build their own providers on the public transport and JSON
APIs. [fg_source_utils.h](include/fg_source_utils.h) exposes the existing
provider-neutral date/timestamp conversion, string/number validation, URL query
encoding, redacted JSON requests and bounded pagination helpers. Applications
must not include private headers from `src/` or copy the parser/transport.
[fg_sources.h](include/fg_sources.h) declares only the FRED/ALFRED and
CFTC/FINRA sources; market bar, corporate-action and price-adjustment types
belong to the applications that acquire such data.

The suite contains ten CTests: nine acquisition/CLI tests plus dd provenance.

## License and data use

The MIT license covers the software, not the records it retrieves. Users supply
their own authorized credentials and must follow provider terms, entitlements,
rate limits, attribution rules and redistribution restrictions. In particular,
availability through a public endpoint does not establish a right to redistribute
its data. This project is not affiliated with or endorsed by the providers.

No account data or production datasets are distributed. Tests use synthetic
fixtures. Local `data/`, build outputs and credential files are not part of the
source distribution. Do not include credentials, account responses or personal
datasets in bug reports.

## Maintenance

See [docs/testing.md](docs/testing.md) for coverage. dd provenance in
[docs/dd-upstream.json](docs/dd-upstream.json) covers all 38 runtime and template
files. Never patch vendored dd locally; update the reviewed release and
regenerate every fingerprint together. Preserve the `-text` attributes and test
from a fresh checkout.

## Documentation

[AGENTS.md](AGENTS.md) — conventions for contributors and coding agents.

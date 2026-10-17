# Acquisition testing

The normal `dd test` run executes **10 tests in both Release and Debug**.
`dd asan --yes` executes **9 acquisition/CLI tests** in a separate Debug tree.
No external network, real credentials, persistent datasets, or implicitly loaded
environment files are used. Dependency restore is a separate build-time concern.

Windows/Linux CI runs on pushes, pull requests and manual dispatch. Before the
normal and sanitizer suites it validates declarations with `dd dep install` and
checks tools with `dd doctor`; every command is non-interactive. Windows uses
SDK WinHTTP with no package restore. Linux installs G++, CMake, Ninja and
libcurl development headers. Failure artifacts contain logs and test
reports only, retained for seven days.

The package-restoration records below are historical: archive and hash
dependencies now belong to their consuming application. The library's retired
toolchain/manifest/restore checks are gone; dd and its deadlines are unchanged.

| CTest | Coverage |
|---|---|
| data | Strict UTF-8 JSON, surrogate pairs, deterministic objects, exact integers and numeric range checks, RFC 4180 multiline CSV, explicit BOM/quoted `.env` parsing and upward loading |
| transport | GET/POST injection, validation, errors/retained HTTP bodies, size limits, buffer ownership, SEC User-Agent/pacing with a fake clock, UTF-8 file paths |
| sec | Ticker map, paginated filings, XSL document paths, companyfacts and filed knowledge time, Markdown tables/entities/filtering (cover dropped only up to a recognised end, code-point table padding, browser-compatible comment ends and `colspan`), iXBRL scaling/sign/context/continuation handling |
| sources | First-release versus latest FRED, missing values, CFTC/FINRA records and the public provider-neutral date/query/pagination helpers |
| cli_fixtures | Every source through the actual CLI runner, JSON/JSONL outputs, argument validation, credential precedence, malformed/provider/I/O errors |
| cli_help | Native CLI help and command discovery |
| cli_invalid | Native process fails on invalid commands |
| cli_unicode | Native process receives a real UTF-8 path including non-BMP characters; exercises Windows wide argv, not just injected arguments |
| transport_loopback | Real WinHTTP/libcurl GET and binary POST, retained 404, explicit cookies and no implicit jar, no redirects/auth retention, limits and timeout; listener is bound to loopback and always disposed |
| dd_adoption | All 38 byte-pinned files, native presets and library/CLI selection, sanitizer adapter, C-only build, SDK-only Windows/no archive dependencies, CI/badge contracts, script parsing and invalid-configuration rejection |

The transport tests share a CTest resource lock for their temporary file names.
Other test paths live in the current build tree. Run commands from the repository
or use `--project`; tests receive absolute fixture directories.

## 2026-10-19 application-source boundary

### Final provider scope

The library now contains only SEC EDGAR/XBRL, FRED/ALFRED and CFTC/FINRA source
acquisition, plus neutral transport and parsing utilities. The archive
provider implementation, C contract, regression executable and eight synthetic
fixtures moved together to its application; fixture bytes are unchanged.
Archive and digest packages are no longer configured or linked here. Market
bar, corporate-action and price-adjustment types, their 28 adjustment checks
and the unwired inline-header fixture moved to the application that uses them;
the retained `sources` suite runs 267 checks. The CLI now includes `<math.h>`
itself rather than through the public header.

`dd test --non-interactive` passed **10/10 in both Release and Debug on MSVC and
GCC**. `dd asan --yes --non-interactive` passed **9/9 on Windows ASan and Linux
ASan/fatal UBSan**. Windows was freshly configured without a package toolchain.
One incremental Debug CLI link reported LNK1201 while writing its PDB; a focused
relink succeeded without code changes, followed by the full passing gate.

### Earlier extraction

The following counts describe the first extraction; the subsequent archive
provider extraction leaves the ten CTests listed above.

- Application-specific acquisition implementations and synthetic fixtures were
  moved to their consumer. Public source acquisition, generic bars/actions and
  pure adjustment arithmetic retain their existing regression groups.
- `fg_source_utils.h` publishes the existing neutral helpers without copying
  parsing, transport or calendar logic. Direct tests cover UTC offsets and
  nanoseconds, invalid dates, query encoding, token lifetime/cycles and missing
  pagination fields; the retained source suite passes 295 checks.
- `dd test --non-interactive`: **11/11 in Release and Debug on both MSVC and
  GCC (Ubuntu/WSL)**. `dd asan --yes --non-interactive`: **10/10** on Windows
  ASan and Linux ASan/fatal UBSan. No new compiler or sanitizer diagnostics.
- Removed application-specific example CLI commands are absent from native
  help and reject invocation with `FG_INVALID`. No live providers or persistent
  datasets were used.

## 2026-09-16 P8 cold-cache configure deadline

CI3 exposed a second root-selection issue: standalone restore used `C:\vcpkg`,
but dd's normal MSVC environment import replaced `VCPKG_ROOT` with Visual
Studio's embedded installation. Different tools/registry modes changed package
ABIs and rebuilt the dependencies inside the unchanged configure deadline.
Using the same compiler did not make those vcpkg installations equivalent.

All three workflows now preserve their chosen root in `CI_VCPKG_ROOT` alongside
`VCPKG_ROOT`. Windows presets use a project-owned wrapper that prefers the stable
CI value, validates the actual toolchain, sets the CMake process environment and
includes that same root's toolchain. Invalid roots and stale cached roots fail
explicitly. Seven CMake-script cases cover precedence, fallback, missing/invalid
roots, environment propagation and cache consistency without adding a CTest.
No timeout, package pin, CRT selection, vendor runtime or Linux lane changes
are involved.

The corrected flow was tested with a real standalone Git-mode vcpkg root and
the distinct VS18 embedded root, not two aliases for the same installation.
Standalone vcpkg was version `2026-07-27`; embedded vcpkg was `2026-05-27`.
Each repository's actual pre-restore ran first. The unchanged dd compiler
initializer then demonstrably replaced `VCPKG_ROOT` with the embedded path while
retaining `CI_VCPKG_ROOT`. Fresh configure and actual dd Release/Debug runs still
selected the standalone root and performed **zero package reinstalls/rebuilds**.
dd configure times were 6.7/6.6 seconds for libcfindata, 8.6/8.7 for equity-app and
9.1/9.0 for crypto-app. libcfindata passed 12/12 CTests in both configurations;
crypto passed 7/7 offscreen tests; equity built both configurations and passed
its adoption CTest in each, without running visible GUI smoke.

The later [run 35115899665](https://github.com/ZacWalk/libcfindata/actions/runs/35115899665)
failed earlier in standalone restore: the hosted `C:\vcpkg` checkout lacked
the pinned `9e44ec0e9f247d77c230ced0ee66c76296837807` baseline object.
The same issue affected equity-app run 35115905058 and crypto-app run 35115900784;
both private application checkouts had authenticated successfully.

All three pre-restore steps now validate the unchanged manifest baseline as a
full 40-character SHA, run an exact `git fetch --no-tags` from the official
`https://github.com/microsoft/vcpkg.git`, and require
`git cat-file -e "<baseline>:versions/baseline.json"` to succeed before install.
Both exit codes are checked. No floating branch, pull, checkout, reset, baseline
change or baseline-file dump is involved. This remains within the bounded
30-minute pre-restore, separate from dd's unchanged configure deadline.

The missing-object case was reproduced in newly initialized, isolated Git
metadata: `cat-file -e` failed before the exact official fetch and succeeded
afterward, without changing HEAD. The resulting baseline blob was
`7cb679a82ee816a0367338261d1ef4dba2bc19f1` (224,722 bytes); its contents were not
dumped. All three actual workflow fetch/check prefixes passed, and a `main`
baseline fixture was rejected before fetching. Actionlint, 28 actual workflow
script parses and all three adoption guards passed.

[Hosted run 35107217906](https://github.com/ZacWalk/libcfindata/actions/runs/35107217906)
passed Linux. Windows Release configure hit dd's 600-second limit while building
OpenSSL from a cold vcpkg cache. Debug then spent another 8.9 minutes completing
the install and passed 12/12 tests, but the overall command correctly retained
the Release failure and exited 1. A green Debug suite is not a substitute for
Release validation.

Windows CI now pre-restores the pinned manifest under its own bounded step, as
do equity-app (static MT) and crypto-app (static MD). No dd runtime, package pin,
CRT selection, sanitizer lane, configure timeout or test-failure handling was
weakened. Hosted cold-cache success must be checked after publication.

Local verification ran the exact pre-restore scripts for all three manifests
against existing package caches. Malformed-manifest fixtures made the real
vcpkg executable return 1; logging preserved that status and the explicit
failure check threw. All 28 workflow run bodies parsed and actionlint passed.
Subsequent dd configure times were 7.6/7.4 seconds for libcfindata, 9.0/9.2 for
equity-app and 9.0/9.4 for crypto-app (Release/Debug). libcfindata passed 12/12
tests in both configurations; both apps built and passed their adoption CTest
in each configuration. These are warm-cache local results, not proof of hosted
cold-cache timing.

## 2026-09-15 P3 migration results

- Original MSBuild application built successfully; its legacy `--selftest`
  passed **10/10** before migration. The old Markdown dataset test had no inputs.
- Integrated C library: MSVC and GCC build and pass all source/CLI fixture suites.
  Final `dd test`: **11/11 Release and 11/11 Debug on each platform**.
- Final `dd asan --yes`: **10/10 on each platform**, with fatal UBSan on GCC.
  No new compiler diagnostics after splitting the long CLI help literal.
- The source suites exercise thousands of assertions and mutations, including
  2,000 numeric conversion comparisons, truncated SEC markup/JSON, ZIP member
  mutations, and exact timestamps that cannot be represented by double.
- Review-driven regressions were fixed and covered: excluded SEC Form 4 XSL
  paths, native curl cookies persisting across independent sessions, and
  Windows ANSI argv.
- First Linux CI exposed Git normalizing embedded CRLF inside quoted CSV fields.
  Fixture payloads now have `-text` attributes as well as the vendored runtime;
  fresh-checkout checks include `core.autocrlf=false` rather than relying on the
  Windows working tree's line-ending conversion.
- App-only migration in equity-app: baseline **9/9**, then **11/11** Debug and
  Release with native GUI/CLI builds and smoke checks. Follow-up truncation and
  timeout regression tests pass. See that repository's `docs/testing.md`.

## Application-specific acquisition

Application-specific provider implementations and their synthetic fixtures and
regression suites are maintained by the consuming application. The current
library tests retain FRED, positioning and shared utility
coverage independently; the historical counts above predate that extraction.

## 2026-10-09 manual QA fixes

- Markdown: a filing with the SEC cover heading but no `PART ` or table-of-
  contents line after it (every 8-K; a 10-Q using "Part I" and "INDEX") was
  dropped whole as "cover" — Apple's 2026-07-30 8-K converted to 0 bytes with
  exit 0. The cover is now dropped only when its end is recognised; otherwise
  every line is kept. Table padding counts UTF-8 code points, so cells holding
  ’, — or € no longer misalign. Both `html_cases` regressions failed before.
- Verification: `dd test` **12/12 Release and Debug**; `dd asan --yes` (MSVC)
  **11/11**; WSL GCC 13 `-Wall -Wextra -Wpedantic` Debug **11/11**
  acquisition/CLI tests with no diagnostics, and GCC ASan + fatal UBSan **11/11**.
- Follow-up, browser leniency: the tag reader ended a comment only at a `-->`
  after `<!--`, but browsers also accept the empty comments `<!-->` and
  `<!--->` and end a comment at `--!>`. Each failed with `FG_PARSE`, or, when
  another comment followed, silently swallowed the text up to its `-->`.
  Comments now end where the HTML tokenizer ends them: a `-->` may reuse the
  opener's dashes, a `--!>` may not (`<!--!>` stays open). XBRL extraction
  shares the reader. A one-off check against a model of the tokenizer's
  comment states, not in the suite, agreed on all 2,441,406 inputs of up to
  nine characters after `<!--`. A non-numeric, negative or zero `colspan`
  failed with `FG_PARSE`; it is now parsed with HTML's integer rules, an
  invalid one counts 1 and one above 1000 is clamped to 1000, inside the
  unchanged 1024-column row limit. The seven new regression `html_cases` and
  three clamp checks each failed before; an eighth case guards that `<!---!>`
  does not close. `dd test` **12/12 Release and Debug**; WSL GCC 13
  `-std=c11 -Wall -Wextra -Wpedantic` Debug with ASan + fatal UBSan **12/12**,
  no diagnostics.

## Limits of the evidence

Fixtures establish deterministic request/response behavior, not continued uptime
or unchanged contracts at live providers. Authenticated FRED requests are not run
in CI. Consumers must supply their own authorized credentials.
No financial efficacy or production scoring claims follow from these tests.

CFTC/FINRA inputs are explicit CSV/TSV, not arbitrary archived spreadsheets.
Unsupported iXBRL transforms remain explicit null values with source metadata;
nested HTML tables flatten. Streaming callbacks/CLI output are not transactional.
Size and pagination caps are intentional and return errors rather than silently
truncating data.

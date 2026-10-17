# AGENTS.md

libcfindata is a C11 library for financial data acquisition: it fetches and parses
market data from SEC EDGAR/XBRL, FRED/ALFRED and CFTC/FINRA, and exposes
them through a C API. Applications own storage, analysis and presentation.

Project code and the included dd files are MIT licensed; see `LICENSE` and
`THIRD_PARTY_NOTICES.md`. External dependencies retain their own licenses, and
provider-data rights are separate from the software license. Do not commit
credentials, account responses or production datasets.

## Layout

| Path | Purpose |
| --- | --- |
| `include/` | Public headers (`fg_*.h`) |
| `src/` | Implementation (`fg_*.c`) plus the `cfindata_cli` driver |
| `tests/` | CTest executables and fixtures |
| `dd.psd1` | Project manifest: targets, presets and output paths |
| `docs/dd-upstream.json` | SHA-256 fingerprints of every vendored dd file |

The CMake project is `libcfindata`; the library target is `cfindata` and the CLI
target is `cfindata_cli`. The `fg_` source and symbol prefix predates the rename
and is retained deliberately — do not rename it as a drive-by change.

## Build and validation

This repository uses the unmodified [dd](https://github.com/ZacWalk/dd) build
driver (`dd.ps1` and `.dd/`), byte-for-byte upstream:
[docs/dd-upstream.json](docs/dd-upstream.json) records the vendored hashes, and
`.gitattributes` keeps them byte-stable. `dd.psd1` owns target, preset and command
metadata and declares `dependencies.owner = 'application'`, so dd manages no
dependency here: CMake owns sources. Windows uses only SDK WinHTTP; Linux finds
system libcurl with `find_package`. No archive/hash packages or vcpkg toolchain
are required. See [README.md](README.md#build-and-test).

- Canonical local gate: `pwsh -File dd.ps1 test` (Release and Debug).
- Sanitizer gate: `pwsh -File dd.ps1 asan --yes`. `scripts/asan.ps1` reuses the
  pinned dd functions; it is not another build driver.
- `.\dd.ps1 commands --json` lists this project's `asan` command. dd's
  [two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes)
  and [commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands)
  are documented upstream.
- Scratch files go under `tmp/`, including a build tree's `tmp/`. Never alter
  persistent `data/` or `exe/` during validation.

## Conventions

- Network-touching code paths must be reachable offline through the fixtures in
  `tests/fixtures/`. Tests never need live providers, API keys, `.env` or
  persistent data; the native transport test uses a bounded, ephemeral loopback
  listener.
- Packages come only through `find_package` as above. Do not add a second
  acquisition path in CMake.

## Code

- Pure C11; no C++ or platform-h dependency. Public names use `fg_`.
- `fg_str` is borrowed UTF-8 with byte length. Buffers/JSON own memory and have
  matching free functions; callback views expire on return. See header contracts.
- `fg_status` plus `fg_error` for failures. Do not swallow parser/HTTP/limit
  errors or return successful empty/zero data. Missing fields are explicit.
- Use the one parser in `fg_data.c` for JSON/CSV/env; preserve exact decimal
  lexemes and integer timestamps/IDs rather than routing through double.
- No implicit credentials or storage. Libraries never write stdout; the CLI
  owns formatting. Do not include URLs containing keys or provider bodies in
  error messages. Native Windows argv must be converted from UTF-16 to UTF-8.
- Every source uses injectable `fg_transport`. Default transports disable
  redirects and cookie storage. Never
  disable TLS certificate verification.
- SEC requests use `fg_sec_get`, a configured real User-Agent and 120 ms
  minimum pacing. Filter excluded filings before download-path restrictions;
  SEC's XSL document subpaths are legitimate, traversal is not.
- Keep release/knowledge times distinct from observation/period/report dates.
  FRED defaults to first-release ALFRED; no revised fallback.
- App models, scoring, prompts, and LLM integrations belong to applications. No orders,
  WebSocket streaming, database, stock-report model or LLM code belongs here.

## Tests and structure

- `tests/test.h`: simple assertions and ordered transport fixtures; no framework.
- Source fixtures are synthetic and tracked under `tests/fixtures/`. The
  `.env` fixtures contain only synthetic test strings and are intentionally tracked.
- `src/fg_core.c`, `fg_transport.c`, `fg_data.c`: core/transport/parsing.
- `fg_sec.c`, `fg_html.c`, `fg_xbrl.c`: SEC and document processing.
- `fg_fred.c`, `fg_positioning.c`: market sources.
- `include/fg_source_utils.h` exposes
  provider-neutral helpers for application-owned sources; their implementation
  stays in `fg_sources_internal.c`. Internal headers are not application APIs.
- `src/main.c`, `fg_cli.c`: native entry point and shared CLI runner tested with
  injected transport. Use `dd run -- --help` for current acquisition commands.
- Validate on both MSVC `/W4` and GCC `-Wall -Wextra -Wpedantic`, including
  sanitizer builds. Prove newly introduced negative-path checks fail before
  accepting the fix; don't rely on green happy-path fixtures alone.

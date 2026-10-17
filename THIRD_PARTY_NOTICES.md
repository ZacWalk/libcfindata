# Third-party notices

## Project code and vendored dd

The original libcfindata code is distributed under the [MIT license](LICENSE),
copyright 2026 Zac Walker.

The vendored `dd.ps1` and `.dd/` files come from
[ZacWalk/dd v0.2.2](https://github.com/ZacWalk/dd/tree/114890d721907dee64651371947f49dd33dad27c).
Their exact revision and SHA-256 fingerprints are recorded in
[docs/dd-upstream.json](docs/dd-upstream.json).

That upstream revision does not include a license file. Zac Walker, as the
copyright holder, expressly grants the MIT license in this repository to these
vendored dd files as well. This grant covers the included copies; it does not
claim to change the licensing of other upstream revisions. The vendored bytes
remain unchanged.

## External dependencies

External packages are not relicensed under libcfindata's MIT license. CMake
finds libcurl through system packages on Linux. Windows uses SDK WinHTTP and
requires no external package restore. Package sources are not vendored here.

| Dependency | Licensing |
|---|---|
| [libcurl](https://curl.se/docs/copyright.html) | curl's permissive copyright and permission notice; used by the Linux transport |

Dependencies can have additional transitive dependencies depending on the
platform and selected package features. When distributing binaries, include
the license texts and notices required by every bundled dependency, not just
this summary. System packages provide their own copyright files.
Windows system libraries remain subject to the
applicable Microsoft terms.

## Provider data

The software license does not grant rights to provider data, account access,
provider trademarks, or redistribution of retrieved records. Users must comply
with each provider's terms, access entitlements, attribution requirements and
rate limits. The tracked test fixtures are synthetic, not a licensed market
dataset.

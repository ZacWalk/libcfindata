# Financial data acquisition foundations

This is the acquisition-only historical snapshot through April 13, 2026.
It retains the original, separable HTTP and document-processing foundations:

- `src/http_client.h` and `src/http_client.cpp`: Windows WinINet transport.
- `src/util.h`: UTF-8 string utilities.
- `src/json_parser.h`: JSON parsing.
- `src/html_to_md.h`: HTML-to-Markdown conversion.

These are historical C++20 sources, not the current C11 API or a supported
standalone release. The following commit replaces them with the complete
libcfindata C11 library, provider APIs, build system, offline tests and usage
documentation.

Application entry points, stock-report models and their coupled provider
adapters, scoring, LLM integration, prompts, IDE settings and local datasets
are intentionally excluded from this reconstructed public history.

## License

Copyright (c) 2026 Zac Walker. Distributed under the [MIT license](LICENSE).
The license grant was added when preparing this reconstructed history for
publication; the commit date records the original source snapshot date.
Provider data and access rights are not licensed by this software license.

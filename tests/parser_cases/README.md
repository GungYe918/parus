# Parser Cases

This directory is consumed by `parus_parser_tests` (parser-only).

Supported file prefixes:

1. `ok_*.pr`
2. `warn_*.pr`
3. `err_*.pr`

Optional in-file directives:

1. `//@expect-error <DiagCodeName>`
2. `//@expect-warning <DiagCodeName>`
3. `//@expect-no-parser-error`

If directives are present, directive expectations override filename prefix fallback.

# Parser Crash Regression Corpus

This directory stores minimized parser inputs that previously triggered a crash,
hang, or non-termination.

Rules:

1. Keep each file as small as possible while reproducing behavior.
2. Preserve the failing pattern only (remove unrelated code).
3. Never delete an existing crash regression unless the feature itself is removed.

Validation target:

- `parus_parser_crash_tests`

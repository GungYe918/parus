# parusc Diagnostics

## 출력 모드

1. `--diag-format text` (기본)
2. `--diag-format json`

## text 모드

1. `diag::render_one_context` 기반 컨텍스트 출력
2. `--lang`, `--context` 옵션 반영

## json 모드 스키마

각 진단 object 필드:

1. `severity`
2. `code`
3. `message`
4. `file`
5. `line`, `col`
6. `end_line`, `end_col`
7. `args` (string array)
8. `range` (0-based LSP 친화)

## 종료 코드

1. error 없음: `0`
2. error 존재: `1`
3. `-fsyntax-only`도 동일한 규칙 사용

## 코드 근거

1. `compiler/parusc/src/p0/P0Compiler.cpp` (`flush_diags_`)

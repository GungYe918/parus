# parusc Diagnostics

## 출력 모드

1. `--diag-format text` (기본)
2. `--diag-format json`

## text 모드

1. `diag::render_one_context` 기반 컨텍스트 출력
2. `--lang`, `--context` 옵션 반영
3. generic/mono/sidecar 관련 진단은 primary span 외에 secondary label, `note:`, `help:`를 출력할 수 있다

예시 출력 순서:

1. primary error line
2. primary span context
3. secondary labeled spans
4. `note: ...`
5. `help: ...`

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
9. optional `labels`
10. optional `notes`
11. optional `help`

`labels` 항목 필드:

1. `file`
2. `line`, `col`
3. `end_line`, `end_col`
4. `range`
5. `message`

호환성 규칙:

1. 기존 필드는 유지한다.
2. `labels/notes/help`는 additive 확장이다.
3. 기존 JSON consumer는 새 필드를 무시해도 된다.

## Generic/Mono 우선 적용 범위

structured diagnostics는 이번 라운드에서 아래 오류군에 우선 적용한다.

1. generic constraint failures
2. template-sidecar unavailable/schema failure
3. unsupported dependency closure
4. missing closure node
5. conflicting canonical sidecar identity
6. hidden helper/proto visibility misuse

대표 코드:

1. `TemplateSidecarUnavailable`
2. `TemplateSidecarSchema`
3. `TemplateSidecarUnsupportedClosure`
4. `TemplateSidecarMissingNode`
5. `TemplateSidecarConflictingNode`
6. `GenericConstraintProtoNotFound`

추가 품질 규칙:

1. hidden helper/proto/class lexical misuse는 ordinary visibility/import error code를 유지하되, `note:`로 closure-private/internal rule과 proto-target ergonomics의 경계를 설명한다.
2. broken sidecar/schema failure는 payload 자체 문제인지, unsupported closure shape인지 구분되는 primary code를 사용한다.
3. generic/mono 관련 error는 가능한 경우 `help:`에 실제 수정 방향을 포함한다.

## 종료 코드

1. error 없음: `0`
2. error 존재: `1`
3. `-fsyntax-only`도 동일한 규칙 사용

## 코드 근거

1. `compiler/parusc/src/p0/P0Compiler.cpp` (`flush_diags_`)
2. `frontend/src/diag/render.cpp`
3. `frontend/include/parus/diag/Diagnostic.hpp`

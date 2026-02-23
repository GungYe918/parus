# parusd Diagnostic Schema

## 목적

`textDocument/publishDiagnostics`에서 전달하는 진단 필드를 고정한다.

## notification 형태

```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/publishDiagnostics",
  "params": {
    "uri": "file:///...",
    "version": 3,
    "diagnostics": [ ... ]
  }
}
```

## diagnostic 항목

1. `range.start.line` (0-based)
2. `range.start.character` (0-based)
3. `range.end.line` (0-based)
4. `range.end.character` (0-based)
5. `severity` (LSP numeric)
6. `code` (Parus 또는 LEI diag code name)
7. `source` (`parusd`)
8. `message` (현재 영문)

## severity 매핑

1. warning -> `2`
2. error/fatal -> `1`

## 분석 파이프라인

`Lexer -> Parser -> Passes -> TypeChecker -> CapabilityCheck`

파이프라인 중 오류가 생기면 해당 단계까지의 진단을 publish한다.

LEI는 `parse + evaluator`를 함께 수행한다.
현재 문서 경로와 일치하는 `L_*`(semantic) 진단을 parse 진단과 합쳐 publish한다.

## 코드 근거

1. `tools/parusd/src/main.cpp` (`analyze_document_`, `build_publish_diagnostics_`)

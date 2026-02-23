# parusd Docs

`parusd`는 Parus/LEI를 함께 처리하는 standalone language server다.

## 실행

```sh
parusd --stdio
```

## 문서 인덱스

1. `STDIO_PROTOCOL.md`: stdio LSP framing과 처리 메서드
2. `DIAGNOSTIC_SCHEMA.md`: publishDiagnostics payload 규약

## 언어 지원 범위

1. `*.pr`, `*.parus`: 기존 Parus 파이프라인(증분 파싱 + semantic tokens)
2. `*.lei`: LEI parse + evaluator lint + semantic tokens

## 코드 근거

1. `tools/parusd/src/main.cpp`

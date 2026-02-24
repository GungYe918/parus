# parusd Docs

`parusd`는 Parus/LEI를 함께 처리하는 standalone language server다.
Parus 문서 lint 시 LEI `config.lei`를 기준으로 bundle/module context를 구성해
cross-module `import`/`export` 해석을 수행한다.

## 실행

```sh
parusd --stdio
```

## 문서 인덱스

1. `STDIO_PROTOCOL.md`: stdio LSP framing과 처리 메서드
2. `DIAGNOSTIC_SCHEMA.md`: publishDiagnostics payload 규약

## 언어 지원 범위

1. `*.pr`, `*.parus`: Parus 파이프라인(증분 파싱 + bundle/module-aware lint + semantic tokens)
2. `*.lei`: LEI parse + evaluator lint (semantic tokens는 빈 결과 반환)

## bundle/module 컨텍스트 동기화

1. `parusd`는 `config.lei` + `plan master`를 평가해 module-first graph를 읽는다.
2. bundle export index(`.lei-cache/index/*.exports.json`, v3)를 on-demand prepass로 생성/갱신한다.
3. `workspace/didChangeWatchedFiles`로 `.lei` 변경을 받으면 같은 프로젝트 루트의 열린 `.pr` 문서를 자동 재진단한다.

## 코드 근거

1. `tools/parusd/src/main.cpp`

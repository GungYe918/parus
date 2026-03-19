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
2. bundle export index(`target/parus/index/*.exports.json`, v4)를 on-demand prepass로 생성/갱신한다.
3. `workspace/didChangeWatchedFiles`로 `.lei` 변경을 받으면 같은 프로젝트 루트의 열린 `.pr` 문서를 자동 재진단한다.

## 코드 근거

1. `tools/parusd/src/main.cpp`

## C Import 안정화 메모

1. `import "X.h" as c;` 심볼은 LSP 내부에서 libclang으로 수집한 external export로 주입된다.
2. Go to Definition은 가능하면 C 헤더 원본 파일/라인으로 이동한다.
3. 시스템 헤더 탐색 순서는 `config.lei cimport.isystem` + VSCode `parus.cimport.*` + auto-probe 순이다.
4. C import 실패 시 `CImportLibClangUnavailable` 또는 상세 원인 진단이 같이 표면화된다.
5. VSCode에서 C import 심볼이 전부 미해결로 보이면 `parus.server.path`가 최신 `parusd/parusc`를 가리키는지 확인 후 서버를 재시작한다.

# parusd stdio Protocol

## 목적

`parusd`의 LSP stdio 처리 경로와 지원 메서드를 정의한다.

## framing

1. 입력: `Content-Length` 헤더 + JSON payload
2. 출력: 동일 framing으로 response/notification 송신

## 지원 메서드

1. `initialize`
2. `initialized`
3. `shutdown`
4. `exit`
5. `textDocument/didOpen`
6. `textDocument/didChange`
7. `textDocument/didClose`
8. `textDocument/semanticTokens/full`

지원하지 않는 요청은 JSON-RPC `-32601 method not found` 반환.

## 문서 상태 관리

1. URI별 `DocumentState` 유지
2. change 이벤트마다 텍스트 패치 적용
3. 문서 확장자별 라우팅:
   - `*.pr`, `*.parus`: Parus 파이프라인 실행
   - `*.lei`: LEI parse + evaluator lint 실행 (열린 LEI 문서는 메모리 오버레이로 반영)
4. 변경 후 진단 publish

## initialize 응답 capabilities

1. `textDocumentSync` (`openClose=true`, `change=2`)
2. `positionEncoding = utf-16`
3. `semanticTokensProvider` (`full=true`, `range=false`)

## semanticTokens 동작

1. Parus 문서: 토큰 분류 결과 반환
2. LEI 문서: keyword/function/parameter/variable/string/number/operator 분류 반환

## 코드 근거

1. `tools/parusd/src/main.cpp` (`read_lsp_message_`, `write_lsp_message_`, `LspServer`)

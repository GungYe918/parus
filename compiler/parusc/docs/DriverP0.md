# Driver and p0

## 목적

사용자 CLI 오케스트레이션(`driver`)과 실제 컴파일 파이프라인(`p0`)을 분리한다.

## 구성

1. Driver: `compiler/parusc/src/driver/Driver.cpp`
2. P0 compiler: `compiler/parusc/src/p0/P0Compiler.cpp`

## Driver 책임

1. 옵션 모드 분기(`compile`, `lsp`, `usage`, `version`)
2. 입력 파일 로딩 + 경로 정규화
3. `Invocation` 구성 후 p0 호출
4. LSP 모드에서 `parusd --stdio` 프로세스 위임

## p0 책임

1. Lex/Parse/Pass/Tyck/CAP 수행
2. `-fsyntax-only` 게이트 처리
3. SIR/OIR 생성 및 검증
4. backend(aot)/link 단계 호출
5. 진단 출력(text/json)

## 실패 경계

1. parse error 발생 시 pass/tyck로 진행하지 않음
2. 각 단계 오류는 즉시 진단 flush 후 non-zero 반환

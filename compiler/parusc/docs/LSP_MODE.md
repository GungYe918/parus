# parusc LSP Mode

## 목적

`parusc`에서 LSP 서버 실행을 오케스트레이션하는 경로를 설명한다.

## 호출 형태

```sh
parusc lsp --stdio
```

## 동작

1. Driver가 `Mode::kLsp`로 분기
2. `parusd` 실행 경로 탐색
   - `PARUSD`
   - `PARUS_TOOLCHAIN_ROOT/bin/parusd`
   - `parusc` 형제 경로
   - fallback `parusd`
3. child argv 실행: `parusd --stdio`

## 실패 조건

1. `--stdio` 누락
2. 알 수 없는 lsp 옵션
3. child process 실행 실패

## 코드 근거

1. `compiler/parusc/src/cli/Options.cpp`
2. `compiler/parusc/src/driver/Driver.cpp`

<!-- compiler/parusc/docs/DriverP0.md -->
# Driver / p0 구조

## 개요

`parusc`는 내부적으로 두 계층으로 분리된다.

- Driver: 사용자 입력(옵션/파일)을 검증하고 내부 컴파일러 호출 정보(`Invocation`)를 구성
- p0: 실제 컴파일 파이프라인 실행

이 구조는 clang의 `driver` + `cc1` 분리와 유사한 방향을 따른다.

## 소스 위치

- Driver
  - `compiler/parusc/src/driver/Driver.cpp`
  - `compiler/parusc/include/parusc/driver/Driver.hpp`
- p0
  - `compiler/parusc/src/p0/P0Compiler.cpp`
  - `compiler/parusc/include/parusc/p0/P0Compiler.hpp`

## 계층별 책임

### Driver

- CLI 파싱 결과를 해석
- 입력 파일 로드/경로 정규화
- 내부 컴파일러 호출 요청 생성
- 사용자 친화 에러 경로 유지

### p0 내부 컴파일러

- Lexer/Parser/Pass/TypeCheck/CapCheck 실행
- SIR 빌드/검증
- OIR 빌드/검증/패스 실행
- Backend(AOT LLVM lane) 호출

## 왜 분리했는가

- 사용자 CLI 정책 변경이 내부 컴파일 파이프라인과 강결합되지 않게 하기 위해
- 향후 다중 입력, 증분 컴파일, 별도 드라이버(IDE/LSP/빌드시스템)를 추가하기 위해
- 테스트 단위(Driver vs p0)를 분리하기 위해

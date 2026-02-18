<!-- docs/internal/compiler/driver/parusc/README.md -->
# parusc

`parusc`는 Parus 언어의 공식 컴파일러 도구다.  
구조는 clang과 유사하게 다음 두 계층으로 나뉜다.

- Driver 계층: 사용자 CLI를 처리하고 빌드 요청을 구성
- 내부 컴파일러(`p0`) 계층: 실제 컴파일 파이프라인(AST -> SIR -> OIR -> Backend) 실행

관련 문서:

- [CLI 사용법](./CLI.md)
- [Driver / p0 이원화 구조](./DriverP0.md)
- [Extension 가이드(초기 API)](./Extensions.md)

현재 상태(v0):

- 일반 사용자 경로: `parusc input.pr -o output.ll`
- 개발자 내부 옵션: `-Xparus`를 통해서만 접근 가능
- OIR -> LLVM-IR 매핑은 부트스트랩 단계(텍스트 LLVM-IR emission 중심)

## 빌드 체인(2단계)

프로젝트 루트 빌드는 다음 두 단계를 분리해 진행한다.

1. `parus_stage1`: `frontend` + `backend` 라이브러리 빌드
2. `parus_stage2`: 위 라이브러리를 링크해 `parusc` 도구 빌드

`./run.sh`는 이 두 단계를 자동으로 순차 실행한다.

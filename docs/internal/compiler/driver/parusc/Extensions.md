<!-- docs/internal/compiler/driver/parusc/Extensions.md -->
# parusc Extension (초기 설계)

## 목적

`parusc`에 확장 포인트를 두어 다음을 가능하게 한다.

- 빌트인 함수 확장(도메인 특화 내장 함수)
- 정적 분석/Lint 확장(`clang-tidy` 유사 규칙 추가)

## 현재 상태

초기 API 헤더만 제공하며, 로딩/등록/실행 런타임은 아직 구현 전 단계다.

- 헤더: `compiler/parusc/include/parusc/ext/ExtensionAPI.hpp`

## 제공 인터페이스(초기)

- `parusc::ext::Extension`: 확장 모듈 루트
- `parusc::ext::BuiltinRegistrar`: 빌트인 등록 인터페이스
- `parusc::ext::LintRule`: lint 룰 인터페이스
- `parusc::ext::LintContext`: lint 실행 컨텍스트
- `parusc::ext::Diagnostic`: 확장 진단 모델

## 사용 시나리오(목표)

1. 확장 모듈이 `Extension` 구현 제공
2. 컴파일 초기 단계에서 `register_builtins()` 호출
3. lint 단계에서 `create_lint_rules()`의 룰 실행
4. 룰은 `LintContext::emit()`으로 진단 보고

## 구현 보류 항목

- 확장 바이너리 로딩(동적 라이브러리/정적 등록)
- ABI 안정성(버전 negotiation)
- 확장 sandbox/보안 정책
- 빌트인 확장과 타입체커의 정합성 보장 규약

위 항목은 컴파일러 코어 구조가 더 안정화된 뒤 구현하는 것이 안전하다.

# parusc Extensions

## 상태

현재는 확장 API 스캐폴딩 단계다.

## 코드 근거

1. `compiler/parusc/include/parusc/ext/ExtensionAPI.hpp`

## 의도된 확장 포인트

1. builtin 등록
2. lint 규칙 등록
3. 확장 진단 보고

## 비범위 (현재)

1. 동적 로딩 체계
2. 확장 ABI 버전 negotiation
3. sandbox/권한 정책

## 노트

LSP/진단/파이프라인이 안정된 이후 확장 런타임을 단계적으로 도입한다.

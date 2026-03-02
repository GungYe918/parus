# Parus Language Spec Index (v0)

문서 버전: `v0-split`
상태: `Normative (Language Specification Index)`

이 문서는 기존 `docs/spec_v0.md`를 챕터 단위로 분해한 인덱스다.
언어 규칙 해석은 이 인덱스와 하위 챕터를 기준으로 수행한다.

## 챕터 목록

1. `spec/00.md` - Preface / Scope
1. `spec/01.md` - 0. 설계 목표와 철학
1. `spec/02.md` - 1. 문자, 토큰, 기본 규칙
1. `spec/03.md` - 2. 리터럴
1. `spec/04.md` - 3. 프리패스와 단위 계층, `import`/`use`, FFI
1. `spec/05.md` - 3.4 `-ffreestanding` / `-fno-std` 경계 정의
1. `spec/06.md` - 4. 타입 시스템 (v0 정의 + 향후 확장 방향)
1. `spec/07.md` - 5. 바인딩과 가변성: let, set, mut
1. `spec/08.md` - 6. 함수 선언: @attribute, qualifier, 호출 규칙, non-?` / `?` 함수, 예외 허용 범위, 호출 제약
1. `spec/09.md` - 6.1 함수 선언 기본형 (확장판, v0 기준)
1. `spec/10.md` - 7. 제어 흐름: if, switch, while, loop
1. `spec/11.md` - 7.2 switch
1. `spec/12.md` - 7.3 while (statement 루프)
1. `spec/13.md` - 7.4 loop (표현식 루프)
1. `spec/14.md` - 7.5 loop의 값 반환 규칙
1. `spec/15.md` - 7.6 변수 선언과 타입 규칙
1. `spec/16.md` - 7.7 loop 형태별 상세 규칙
1. `spec/17.md` - 7.8 범위 표현식
1. `spec/18.md` - 7.9 기대 효과
1. `spec/19.md` - 8. 표현식과 연산자, 파이프 << 와 hole _
1. `spec/20.md` - 9. actor, draft, pub/sub, commit, recast
1. `spec/21.md` - 10. 타입 정의: struct, proto, class, 접근 제한자
1. `spec/22.md` - 10.4 proto + 제네릭 결합 방향(v1+)
1. `spec/23.md` - 11. acts: 행동 묶음과 타입 부착(메서드/연산자)
1. `spec/24.md` - 12. 람다/콜백 (전역 람다 금지)
1. `spec/25.md` - 13. 심볼, ABI, 디버깅, 맹글링 규칙
1. `spec/26.md` - 14. 구현 체크리스트 (v0)
1. `spec/27.md` - 15. 종합 예시 (여러 기능 한 번에)
1. `spec/28.md` - 16. EBNF 테이블 (v0 전체 문서 반영, 파서 제작 가능 수준)
1. `spec/29.md` - 17. Macro System (Draft, Phase1.5)
1. `spec/30_BUNDLE_MODULE_RESOLUTION.md` - 18. Bundle/Module 이름 해석과 가시성 규칙 (Normative)
1. `spec/31_PROTO_CONSTRAINTS.md` - 19. Proto 제약 규칙 (Constraint-only, v1)

## EBNF

EBNF 정본은 다음 챕터를 따른다.

1. `docs/reference/language/spec/28.md`

## 참조 문서

1. `docs/reference/language/terminology.md`
2. `docs/reference/language/item-model.md`
3. `docs/reference/abi/README.md`

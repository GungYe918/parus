# Parus ABI Documentation

이 디렉터리는 Parus ABI 명세의 단일 저장소다.

- 현재 고정 버전: `v0.0.1`
- 단일 신뢰 문서: `docs/abi/v0.0.1/ABI.md`
- OOP 모델 정본: `docs/abi/v0.0.1/OOP_MODEL.md`
- 제네릭/제약 모델 합의 문서: `docs/abi/v0.0.1/GENERICS_MODEL.md`
- 문자열 모델 상세: `docs/abi/v0.0.1/STRING_MODEL.md`
- 저장소 정책 상세: `docs/abi/v0.0.1/STORAGE_POLICY.md`
- nullable 모델 상세: `docs/abi/v0.0.1/NULLABLE_MODEL.md`
- 언어 용어 정본: `docs/language/TERMINOLOGY.md`
- 파일 item 규칙 정본: `docs/language/ITEM_MODEL.md`

## 버전 정책 (`MAJOR.MINOR.PATCH`)

Parus ABI 문서 버전은 `X.Y.Z` 3자리로 관리한다.

- `MAJOR (X)`:
  Parus ABI가 크게 변경되는 경우 올린다.
  C ABI 라인은 별도 명시가 없으면 유지한다.
- `MINOR (Y)`:
  ABI 규약이 중간 수준으로 변경/확장되는 경우 올린다.
  (구현/호환성 영향이 있을 수 있음)
- `PATCH (Z)`:
  문서 수정, 설명 보강, 오탈자 수정 등 ABI 의미가 완전히 동일한 경우만 올린다.

## 현재 적용 상태

- C ABI 라인: `c-v0` (불변 목표)
- Parus ABI 문서 버전: `v0.0.1`

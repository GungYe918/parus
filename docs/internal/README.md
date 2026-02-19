# Internal Docs Hub

이 디렉터리는 링크 허브 전용이다.
구현 본문 문서는 소스트리 인접 `*/docs`로 이동했다.

## 컴파일러 내부 문서 허브

1. `docs/internal/compiler/README.md`
2. `docs/internal/contributing/CodeConventions.md`

## 정책

1. `docs/internal/**`에는 구현 상세 본문을 두지 않는다.
2. 구현 문서는 해당 컴포넌트 루트(`frontend/docs`, `backend/docs`, `compiler/*/docs`, `tools/*/docs`, `sysroot/*.md`)에 둔다.
3. 언어/ABI 의미 정본은 항상 `docs/reference/**`를 우선한다.

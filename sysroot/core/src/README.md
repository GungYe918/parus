# Parus Core Source Seed

`sysroot/core/src`는 Parus core prelude 소스 위치다.

현재 최소 시드로 `prelude.pr`를 제공하며, `parusc`는 sysroot가 설정되어 있으면
이 prelude를 자동 로드해 builtin 타입 확장(예: `i32.size()`)을 사용할 수 있게 한다.

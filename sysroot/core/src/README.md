# Parus Core Source Seed

`sysroot/core/src`는 Parus core prelude 소스 위치다.

현재 최소 시드로 `prelude.pr`를 제공한다.

`prelude.pr` 내부의 builtin `acts` 선언은 `$![Impl::Core];` 파일 표식을 포함해야 하며,
non-core bundle에서는 동일 표식을 사용할 수 없다.

non-core 사용자 코드에서의 core 자동 주입은 source 병합이 아니라
`sysroot/core/index/core.exports.json` export-index 자동 로드로 처리한다.

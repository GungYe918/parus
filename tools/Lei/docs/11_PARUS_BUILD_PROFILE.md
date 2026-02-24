# 11. Parus Build Profile

## 엔트리 규약

1. 엔트리 파일은 `config.lei`다.
2. 기본 엔트리 plan은 `master`다.
3. CLI `--plan <name>`로 override 가능하다.
4. `export plan master`는 엔진 정책으로 금지된다.

## Module-First 계약

1. `bundle`은 `modules` 필드를 필수로 가진다.
2. 각 module은 `sources/imports`를 가진다(`head`는 자동 계산).
3. Parus import gate는 `module.imports`를 정본으로 사용하고 내부적으로 top-head canonicalization한다.
4. cross-bundle import는 대상 bundle이 `bundle.deps`에도 있어야 한다.
5. `bundle.sources`는 지원하지 않는다.

## inline bundle 정책

1. `config.lei` inline bundle 선언은 최종 bundle이 1개일 때만 허용한다.
2. 최종 bundle이 2개 이상이면 각 bundle 폴더의 `<bundle>.lei` 선언만 허용한다.

## 진단 정책

1. `B_INLINE_BUNDLE_MULTI_FORBIDDEN`
2. `B_BUNDLE_MODULES_REQUIRED`
3. `B_IMPORT_MODULE_NOT_DECLARED`
4. `B_BUNDLE_DEP_NOT_DECLARED`
5. `B_LEGACY_BUNDLE_SOURCES_REMOVED`

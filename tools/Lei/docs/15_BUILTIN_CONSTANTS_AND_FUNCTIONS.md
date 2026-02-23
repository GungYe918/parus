# 15. Builtin Constants And Functions Catalog

## 목적

이 문서는 LEI 엔진이 제공하는 빌트인 상수/함수의 단일 정본 카탈로그를 고정한다.

원칙:

1. 문서에 명시된 항목만 공개 계약으로 간주한다.
2. 표면 문법은 Core가 담당하고, 빌트인은 Build API 계층에서 제공한다.
3. 빌트인 목록 변경은 이 문서를 우선 갱신한 뒤 구현을 갱신한다.

## 범위

이 카탈로그는 다음 4계열을 다룬다.

1. `lei.*`
2. `host.*`
3. `toolchain.*`
4. `parus.*`

주의:

1. 내부 포맷/ABI 레이아웃 상세는 상수로 노출하지 않는다.
2. 산출물 형식 선택(`.parlib/.a/.so/.dylib`)은 상수 집합이 아니라 빌드 플랜 스키마/정책 필드로 다룬다.

## 상수 카탈로그 (v1)

### 1) `lei.*` 최소 집합

1. `lei.version`
2. `lei.engine_name`
3. `lei.engine_semver`
4. `lei.entry_plan_default`
5. `lei.view_formats`
6. `lei.reserved_plan_names`
7. `lei.syntax_generation`

### 2) `host.*` 최소 집합

1. `host.os`
2. `host.arch`
3. `host.family`
4. `host.exe_suffix`
5. `host.shared_lib_suffix`
6. `host.static_lib_suffix`
7. `host.path_sep`
8. `host.path_list_sep`
9. `host.case_sensitive_fs`
10. `host.endian`
11. `host.cpu_count`
12. `host.triple`

### 3) `toolchain.*` 축소 집합

1. `toolchain.generator_default`
2. `toolchain.llvm_use_toolchain_default`
3. `toolchain.llvm_require_toolchain_default`

### 4) `parus.*` 유효 집합

1. `parus.version_major`
2. `parus.version_minor`
3. `parus.version_patch`
4. `parus.version_string`
5. `parus.tools.parusc`
6. `parus.tools.parusd`
7. `parus.tools.parus_lld`
8. `parus.backends.supported`
9. `parus.backends.enabled`
10. `parus.aot.engines`
11. `parus.llvm.lanes_supported`
12. `parus.llvm.lane_selected`
13. `parus.linker.modes`
14. `parus.diag.formats`
15. `parus.langs`
16. `parus.opt_levels`
17. `parus.macro_budget.default_aot.depth`
18. `parus.macro_budget.default_aot.steps`
19. `parus.macro_budget.default_aot.output_tokens`
20. `parus.macro_budget.default_jit.depth`
21. `parus.macro_budget.default_jit.steps`
22. `parus.macro_budget.default_jit.output_tokens`
23. `parus.macro_budget.hard_max.depth`
24. `parus.macro_budget.hard_max.steps`
25. `parus.macro_budget.hard_max.output_tokens`

## 함수 카탈로그 (v1)

### 1) 코어 변환/검사

1. `len(value) -> int`
2. `type_name(value) -> string`
3. `to_int(value) -> int`
4. `to_float(value) -> float`
5. `to_string(value) -> string`
6. `to_bool(value) -> bool`
7. `deep_equal(a, b) -> bool`

### 2) 문자열

1. `str.len(s) -> int`
2. `str.contains(s, needle) -> bool`
3. `str.starts_with(s, prefix) -> bool`
4. `str.ends_with(s, suffix) -> bool`
5. `str.split(s, sep) -> [string]`
6. `str.join(parts:[string], sep:string) -> string`
7. `str.replace(s, from, to) -> string`
8. `str.trim(s) -> string`
9. `str.lower(s) -> string`
10. `str.upper(s) -> string`

### 3) 배열/객체

1. `arr.len(a) -> int`
2. `arr.concat(a, b) -> array`
3. `arr.contains(a, v) -> bool`
4. `arr.uniq(a) -> array`
5. `arr.sorted(a:[string]) -> [string]`
6. `arr.slice(a, begin, end) -> array`
7. `obj.keys(o) -> [string]`
8. `obj.has(o, key) -> bool`
9. `obj.get(o, key, default?) -> any`
10. `obj.values(o) -> array`

### 4) 경로/파일 조회 (결정적 읽기 전용)

1. `path.join(parts:[string]) -> string`
2. `path.normalize(p) -> string`
3. `path.dirname(p) -> string`
4. `path.basename(p) -> string`
5. `path.stem(p) -> string`
6. `path.ext(p) -> string`
7. `path.is_abs(p) -> bool`
8. `path.rel(base, target) -> string`
9. `path.to_slash(p) -> string`
10. `path.to_native(p) -> string`
11. `fs.exists(p) -> bool`
12. `fs.is_file(p) -> bool`
13. `fs.is_dir(p) -> bool`
14. `fs.glob(patterns:[string]) -> [string]`
15. `fs.glob_under(root, patterns:[string]) -> [string]`
16. `fs.read_text(p) -> string`
17. `fs.read_lines(p) -> [string]`
18. `fs.sha256(p) -> string`
19. `fs.file_size(p) -> int`

### 5) SemVer

1. `semver.parse(s) -> object`
2. `semver.compare(a, b) -> int`
3. `semver.satisfies(version, range) -> bool`
4. `semver.bump(version, part) -> string`

### 6) Parus 연동 헬퍼

1. `parus.default_target() -> string`
2. `parus.host_target() -> string`
3. `parus.tool_path(name) -> string`
4. `parus.backend_enabled(name) -> bool`
5. `parus.aot_engine_enabled(name) -> bool`
6. `parus.llvm_lane_selected() -> int`
7. `parus.llvm_lane_supported(lane:int) -> bool`
8. `parus.make_parusc_cmd(args:[string]) -> [string]`
9. `parus.make_link_cmd(args:[string]) -> [string]`
10. `parus.normalize_bundle_name(name) -> string`

## 비노출 원칙

1. 내부 포맷 구조/청크/오프셋/ABI 레이아웃은 상수로 공개하지 않는다.
2. 내부 포맷 정책은 전용 스키마/검증 정책 문서에서 관리한다.
3. 비결정 함수(시간/랜덤/환경 의존/네트워크/셸 실행)는 기본 제공하지 않는다.

## 삭제 이력 (이번 라운드)

다음 범주는 카탈로그에서 제거되었다.

1. 결정성 상태를 직접 노출하는 플래그형 상수
2. CMake 최소 버전 및 C++ 표준 숫자 상수
3. 툴체인 lane 기본값/지원 목록 상수
4. 내부 단위 계층(file/module/project 등)을 노출하는 상수
5. 내부 포맷 구조를 직접 드러내는 상수군

제거 이유:

1. 엔진 정책과 중복되어 오해를 유발함
2. 구현 세부와 공개 API를 과도하게 결합시킴
3. ABI/포맷 변경 민감도를 상수 계약에 노출시킴

## 관련 문서

1. `09_INTRINSICS.md`
2. `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`
3. `14_LEI_PRODUCT_AND_PROFILE_MODEL.md`

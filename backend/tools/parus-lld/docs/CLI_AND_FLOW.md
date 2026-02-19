# parus-lld CLI and Flow

## 목적

현재 구현 기준 `parus-lld` 옵션과 link 실행 경로를 정리한다.

## CLI

```sh
parus-lld [options] <inputs...>
```

주요 옵션:

1. `-o <path>` (필수)
2. `--target <triple>`
3. `--sysroot <path>`
4. `--apple-sdk-root <path>`
5. `--toolchain-hash <u64>`
6. `--target-hash <u64>`
7. `--backend <path>`
8. `--verbose`

`-o`/`input` 누락 시 실패한다.

## 실행 흐름

1. 옵션 파싱
2. sysroot/target/sdk 경로 해석
3. 입력 계획 수립
   - 일반 `.o/.a/...`는 직접 link input
   - `.parlib`는 파싱 후 object/native deps 추출
4. hash preflight
5. backend linker 경로 결정 (`ld64.lld`/`ld.lld`/`lld-link`)
6. argv 실행 (`posix_spawnp`/`_spawnvp`)

## Darwin 보강

1. 입력 object 최소 버전 검사
2. target arch/min version에 맞춰 `-platform_version` 구성
3. SDK root가 있으면 `-syslibroot`, `-L<sdk>/usr/lib` 추가

## 코드 근거

1. `backend/tools/parus-lld/main.cpp`

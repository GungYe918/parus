# parusc CLI

## 기본 형태

```sh
parusc [options] <input.pr>
parusc lsp --stdio
```

## compile 모드 주요 옵션

1. `-h`, `--help`
2. `--version`
3. `-fsyntax-only`
4. `--diag-format text|json`
5. `-o <path>`
6. `--target <triple>`
7. `--sysroot <path>`
8. `--apple-sdk-root <path>`
9. `-O0|-O1|-O2|-O3`
10. `--lang en|ko`
11. `--context <N>`
12. `-fmax-errors=<N>`
13. `-fuse-linker=auto|parus-lld|lld|clang`
14. `--no-link-fallback`
15. `-Wshadow`, `-Werror=shadow`

## `-Xparus` 내부 옵션

1. `-Xparus -token-dump`
2. `-Xparus -ast-dump`
3. `-Xparus -sir-dump`
4. `-Xparus -oir-dump`
5. `-Xparus -emit-llvm-ir`
6. `-Xparus -emit-object`

## 출력 기본값

1. 일반 compile: `a.out`
2. `-Xparus -emit-llvm-ir`: `a.ll`
3. `-Xparus -emit-object`: `a.o`

## `-fsyntax-only` 충돌 규칙

1. `-o`와 함께 사용 불가
2. `-Xparus`의 emit 옵션과 함께 사용 불가
3. target/sysroot/linker 계열 옵션과 함께 사용 불가

## 코드 근거

1. `compiler/parusc/src/cli/Options.cpp`

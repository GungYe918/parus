<!-- compiler/parusc/docs/CLI.md -->
# parusc CLI

## 기본 사용

```bash
parusc main.pr -o main.ll
```

`-Xparus` 없이 실행하면 내부 IR dump/디버그 옵션은 노출되지 않는다.

## 주요 옵션

- `-h`, `--help`: 사용법 출력
- `--version`: 버전 출력
- `-o <path>`: 출력 파일 경로
- `-O0|-O1|-O2|-O3`: 최적화 레벨
- `--lang en|ko`: 진단 언어
- `--context <N>`: 진단 컨텍스트 줄 수
- `-fmax-errors=<N>`: 최대 에러 개수
- `-Wshadow`, `-Werror=shadow`: shadowing 정책

## 개발자 전용 내부 옵션 (`-Xparus`)

아래 옵션은 반드시 `-Xparus`로 전달해야 한다.

- `-Xparus -token-dump`
- `-Xparus -ast-dump`
- `-Xparus -sir-dump`
- `-Xparus -oir-dump`
- `-Xparus -emit-llvm-ir`
- `-Xparus -emit-object` (현재 미구현, 에러 반환)

예시:

```bash
parusc main.pr -o main.ll -Xparus -sir-dump
```

## 출력 기본값

- `-o` 미지정 시 기본 출력: `a.ll`
- `-Xparus -emit-object` 사용 시 기본 출력: `a.o`  
  단, 객체 출력은 현재 구현 전 단계다.

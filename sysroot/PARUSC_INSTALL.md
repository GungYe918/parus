# Parusc Local Installation Rules (Draft)

문서 버전: `v0-draft`  
상태: `Design Draft (next implementation target)`

이 문서는 `parusc`/`parus-lld`의 로컬 설치 규칙을 정의한다.  
시스템 전역(`/usr/local`, `/opt`) 설치는 기본 경로가 아니며, sudo 없는 설치를 원칙으로 한다.

---

## 1. 기본 원칙

1. 설치 루트는 `~/.local`을 사용한다.
2. 사용자 실행 진입점은 `parusc`로 통일한다.
3. `parus-lld` 등 개별 도구는 내부적으로 설치되지만 사용자가 직접 호출할 필요는 없다.

---

## 2. 기본 설치 경로

1. 실행 링크/런처: `~/.local/bin`
2. 실제 toolchain: `~/.local/share/parus/toolchains/<toolchain-id>`
3. active 선택자: `~/.local/share/parus/active-toolchain` (symlink)

---

## 3. zsh PATH 규칙

`~/.zshrc`에 다음을 추가한다.

```sh
export PATH="$HOME/.local/bin:$PATH"
```

새 터미널 시작 후 `command -v parusc`로 경로를 검증한다.

---

## 4. 도구 오케스트레이션 규칙

1. 사용자는 `parusc`만 호출한다.
2. `parusc`는 active toolchain의 `parus-lld`를 호출한다.
3. `parus-lld`는 LLVM linker(`ld64.lld`/`ld.lld`/`lld-link`)를 argv 기반으로 호출한다.
4. shell-string(`system("...")`) 실행 경로는 금지한다.

---

## 5. 설치/갱신 절차 (예정)

구현 완료 후 기본 절차는 다음 흐름을 따른다.

1. toolchain-id 결정
2. `<toolchain-id>/bin`, `libexec`, `sysroot` 배치
3. `~/.local/bin/parusc`, `~/.local/bin/parus-lld` 갱신
4. `active-toolchain`을 새 toolchain으로 전환
5. `parusc --version`과 간단한 링크 smoke test 수행

---

## 6. 충돌/안전 규칙

1. hash 불일치 toolchain은 active로 전환하지 않는다.
2. 불완전 설치(중간 실패) 시 기존 active-toolchain을 유지한다.
3. 플랫폼 SDK(특히 macOS)는 번들하지 않고 참조만 사용한다.


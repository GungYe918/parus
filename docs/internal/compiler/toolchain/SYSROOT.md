# Parus Sysroot Specification (Draft)

문서 버전: `v0-draft`  
상태: `Design Draft (next implementation target)`

이 문서는 Parus의 sysroot 구조를 정의한다.  
현재는 설계 문서이며, 다음 구현 턴에서 실제 코드/설치 로직에 반영한다.

---

## 1. 목표

1. Parus 전용 sysroot를 도입하여 `parusc`가 전체 도구 체인을 오케스트레이션한다.
2. Rust와 유사하게 개별 도구를 설치하되, 사용자 진입점은 `parusc` 하나로 고정한다.
3. macOS SDK는 번들하지 않고 참조(reference)만 허용한다.
4. 표준 라이브러리 소스의 canonical 위치를 프로젝트와 설치 sysroot에 명확히 분리한다.

---

## 2. 로컬 설치 루트 (sudo 불필요)

Parus 기본 설치 루트:

```text
~/.local/
  bin/
  share/parus/
```

`~/.local/bin`은 실행 진입점, `~/.local/share/parus`는 toolchain/sysroot 데이터 저장소로 사용한다.

---

## 3. 설치 후 디렉터리 구조

```text
~/.local/
  bin/
    parusc
    parus-lld

  share/parus/
    active-toolchain -> toolchains/<toolchain-id>
    toolchains/
      <toolchain-id>/
        bin/
          parusc
          parus-lld
          ld64.lld | ld.lld | lld-link
        libexec/
        sysroot/
          manifest.json
          std/
            src/
          targets/
            <target-triple>/
              manifest.json
              parlib/
              lib/
              std/
                parlib/
                obj/
              native/
                apple-sdk.ref
```

---

## 4. 프로젝트 내 sysroot 시드(seed)

프로젝트 루트의 `sysroot/`는 향후 표준 라이브러리 소스 작성 위치로 사용한다.

초기 정책:

1. 프로젝트 `sysroot/std/src`는 표준 라이브러리 소스의 원천(source of truth) 후보.
2. 설치 단계에서 필요한 파일만 `~/.local/share/parus/toolchains/<id>/sysroot/std/src`로 동기화.
3. 빌드 산출물(`.parlib`, `.a`, `.o`)은 프로젝트 시드가 아니라 설치 sysroot target 하위에 저장.

---

## 5. target 및 hash 정책

각 target sysroot는 최소 다음 정보를 `manifest.json`에 가진다.

1. `target_triple`
2. `target_hash`
3. `toolchain_hash`
4. `runtime_set` (`pcore/prt/pstd` 버전 식별자)
5. `abi_line` (`c-v0` 등)

링크 단계 정책:

1. `toolchain_hash` 불일치 -> 하드 에러
2. `target_hash` 불일치 -> 하드 에러
3. 외부 C/시스템 라이브러리는 foreign 입력으로 분리 취급(해시 고정 대상 아님)

---

## 6. macOS SDK 정책

1. Apple SDK는 sysroot에 포함하지 않는다.
2. SDK 해석은 참조형으로만 수행한다 (`--apple-sdk-root`, `SDKROOT`, `xcrun` 순).
3. SDK 경로/버전 정보는 `native/apple-sdk.ref`에 기록할 수 있으나 payload 저장은 금지한다.

---

## 7. 표준 라이브러리 위치

표준 라이브러리 위치는 다음으로 고정한다.

1. 소스: `sysroot/std/src` (프로젝트) / `.../toolchains/<id>/sysroot/std/src` (설치본)
2. target별 파생 산출물:
   - `.../targets/<triple>/std/parlib`
   - `.../targets/<triple>/std/obj`
3. 기본 링크 라이브러리:
   - `.../targets/<triple>/lib/libpcore.a`
   - `.../targets/<triple>/lib/libprt.a`
   - `.../targets/<triple>/lib/libpstd.a`


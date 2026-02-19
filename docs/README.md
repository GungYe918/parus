# Parus Documentation

Parus 문서는 역할별로 분리한다.

## 1. 사용자 정본 (우선)

1. 언어 정본: `docs/reference/language/SPEC.md`
2. ABI 정본: `docs/reference/abi/README.md`
3. 사용자 가이드: `docs/guides/user/manual_v0.md`

## 2. 개발자 구현 문서 (소스트리 인접)

1. frontend: `frontend/docs/README.md`
2. backend: `backend/docs/README.md`
3. parusc: `compiler/parusc/docs/README.md`
4. parusd: `tools/parusd/docs/README.md`
5. parus-lld: `backend/tools/parus-lld/docs/README.md`
6. sysroot/toolchain: `sysroot/SYSROOT.md`

## 3. 허브 문서

1. internal 허브: `docs/internal/README.md`
2. compiler 허브: `docs/internal/compiler/README.md`

## 우선순위 규칙

1. 주제별 normative 문서
2. `docs/reference/**`
3. 개발자 문서(`*/docs`, `sysroot/*.md`)

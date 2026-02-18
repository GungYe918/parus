# Parus

Parus is a systems programming language focused on explicit control flow, data-oriented design, and static dispatch.

## Status

- Development stage: pre-alpha
- Compiler entrypoint: `parusc`
- ABI baseline: `c-v0`

## Documentation

- Language reference index: `docs/reference/language/SPEC.md`
- ABI reference index: `docs/reference/abi/README.md`
- User guide: `docs/guides/user/manual_v0.md`
- Internal compiler docs: `docs/internal/compiler/README.md`

## Repository Layout (high level)

- `frontend/`: parser, name resolution, type checker, SIR
- `backend/`: OIR, LLVM lowering, linker integration
- `compiler/parusc/`: compiler driver
- `docs/`: documentation hierarchy
- `tests/`: language and FFI tests

## Build

```sh
./run.sh
```

## Install (local)

```sh
./install.sh
```

## Note

This repository prioritizes explicit specs. For language and ABI rules, always start from the docs under `docs/reference/`.

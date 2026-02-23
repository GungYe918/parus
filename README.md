# Parus

Parus is a systems programming language focused on explicit control flow, data-oriented design, and static dispatch.

## Status

- Development stage: pre-alpha
- CLI entrypoint: `parus`
- ABI baseline: `c-v0`

## Documentation

User-facing canonical docs:

1. Language reference: `docs/reference/language/SPEC.md`
2. ABI reference: `docs/reference/abi/README.md`
3. User manual: `docs/guides/user/manual_v0.md`

Compiler implementation docs (source-tree adjacent):

1. `frontend/docs/README.md`
2. `backend/docs/README.md`
3. `compiler/parusc/docs/README.md`
4. `tools/parusd/docs/README.md`
5. `backend/tools/parus-lld/docs/README.md`

Internal hub index:

1. `docs/internal/compiler/README.md`

## Repository Layout (high level)

- `frontend/`: parser, name resolution, type checker, SIR/OIR handoff
- `backend/`: OIR/backend integration, LLVM lowering, linker integration
- `compiler/parusc/`: compiler driver
- `tools/parus/`: orchestrator CLI (`parus`)
- `tools/parusd/`: standalone LSP server
- `backend/tools/parus-lld/`: linker driver
- `docs/`: reference and hub documentation
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

Language and ABI semantics are governed by `docs/reference/**`.

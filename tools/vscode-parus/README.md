# vscode-parus

VSCode extension workspace for Parus language support.

## Current scope

- MVP target: diagnostics only (`publishDiagnostics`) with underline + Problems integration.
- LSP server launch priority:
  1. `parus.server.path` if set
  2. `parusc lsp --stdio`
  3. `parusd --stdio` fallback
- Multi-root workspace policy (MVP): show warning and recommend explicit `parus.server.path`.

## Local commands

- `npm run compile`
- `npm run watch`
- `npm run check`

# vscode-parus

VSCode extension workspace for Parus/LEI language support.

## Current scope

- Diagnostics (`publishDiagnostics`) with underline + Problems integration.
- Semantic tokens (`textDocument/semanticTokens/full`) for `.pr`, `.parus`, `.lei`.
- LEI local completion/snippets (`import/from`, `proto`, `plan`, `def`, `for`, `if/else`, `assert`, `export plan`).
- Supported file extensions: `.pr`, `.parus`, `.lei`
- LSP server launch priority:
  1. `parus.server.path` if set
  2. `parusc lsp --stdio`
  3. `parusd --stdio` fallback
- Multi-root workspace policy (MVP): show warning and recommend explicit `parus.server.path`.

## Local commands

- `npm run compile`
- `npm run watch`
- `npm run check`

## Local install (dev)

1. Build and install toolchain:
   - `cd /Users/gungye/workspace/Lang/gaupel`
   - `./install.sh`
2. Build extension:
   - `cd /Users/gungye/workspace/Lang/gaupel/tools/vscode-parus`
   - `npm run check`
   - `npm run compile`
3. Run extension:
   - Open `/Users/gungye/workspace/Lang/gaupel/tools/vscode-parus` in VSCode and press `F5`
   - Or symlink the folder into `~/.vscode/extensions` and run `Developer: Reload Window`

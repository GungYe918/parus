import * as vscode from "vscode";

export function refreshLintStatusBar(
  item: vscode.StatusBarItem | undefined,
  diagnosticsByUri: Map<string, vscode.Diagnostic[]>,
  editor: vscode.TextEditor | undefined
): void {
  if (!item) return;
  if (!editor) {
    item.text = "$(issue-opened) Parus: no file";
    item.tooltip = "Parus diagnostics";
    return;
  }

  const diagnostics = diagnosticsByUri.get(editor.document.uri.toString()) ?? [];
  if (diagnostics.length === 0) {
    item.text = "$(check) Parus: clean";
    item.tooltip = "Parus diagnostics: 0";
    return;
  }

  let errors = 0;
  let warnings = 0;
  for (const diag of diagnostics) {
    if (diag.severity === vscode.DiagnosticSeverity.Warning) {
      warnings += 1;
    } else {
      errors += 1;
    }
  }
  item.text = `$(issue-opened) Parus E:${errors} W:${warnings}`;
  item.tooltip = `Parus diagnostics\nErrors: ${errors}\nWarnings: ${warnings}`;
}

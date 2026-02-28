import * as vscode from "vscode";

const LEI_NAME_DIAGNOSTIC_CODES = new Set([
  "L_UNKNOWN_IDENTIFIER",
  "L_IMPORT_NOT_FOUND",
  "L_IMPORT_SYMBOL_NOT_FOUND",
  "L_IMPORT_CYCLE",
  "L_PLAN_NOT_FOUND",
  "L_EXPORT_PLAN_NOT_FOUND",
]);

const LEI_TYPE_DIAGNOSTIC_CODES = new Set([
  "L_TYPE_MISMATCH",
  "L_PROTO_TYPE_MISMATCH",
  "L_BUILTIN_PLAN_SCHEMA_VIOLATION",
]);

export function enhanceDiagnostics(
  uri: vscode.Uri,
  diagnostics: readonly vscode.Diagnostic[]
): vscode.Diagnostic[] {
  return diagnostics.map((diag) => enhanceOneDiagnostic(uri, diag));
}

function enhanceOneDiagnostic(uri: vscode.Uri, diagnostic: vscode.Diagnostic): vscode.Diagnostic {
  if (diagnostic.source !== "parusd") {
    return diagnostic;
  }

  const code = diagnosticCodeToString(diagnostic.code);
  const category = inferDiagnosticCategory(code);
  const prefix = category ? `[${category}] ` : "";

  const message = diagnostic.message.startsWith(prefix)
    ? diagnostic.message
    : `${prefix}${diagnostic.message}`;

  const range = normalizeDiagnosticRange(uri, diagnostic.range);
  const normalized = new vscode.Diagnostic(
    range,
    message,
    diagnostic.severity ?? vscode.DiagnosticSeverity.Error
  );
  normalized.code = diagnostic.code;
  normalized.source = diagnostic.source;
  normalized.tags = diagnostic.tags;
  normalized.relatedInformation = diagnostic.relatedInformation;
  return normalized;
}

function normalizeDiagnosticRange(uri: vscode.Uri, range: vscode.Range): vscode.Range {
  if (uri.scheme !== "file") return range;
  const startLine = Math.max(0, range.start.line);
  const startChar = Math.max(0, range.start.character);
  const endLine = Math.max(startLine, range.end.line);
  const endChar = endLine === startLine
    ? Math.max(startChar + 1, range.end.character)
    : Math.max(0, range.end.character);
  return new vscode.Range(
    new vscode.Position(startLine, startChar),
    new vscode.Position(endLine, endChar)
  );
}

function diagnosticCodeToString(
  code: string | number | { value: string | number } | undefined
): string | undefined {
  if (code === undefined) return undefined;
  if (typeof code === "string") return code;
  if (typeof code === "number") return String(code);
  if (typeof code === "object" && code !== null) {
    if (typeof code.value === "string") return code.value;
    if (typeof code.value === "number") return String(code.value);
  }
  return undefined;
}

function inferDiagnosticCategory(code: string | undefined): string | undefined {
  if (!code) return undefined;

  if (code.startsWith("Type")) return "Type";
  if (code.startsWith("Cap") || code.startsWith("Borrow") || code.startsWith("Escape")) return "Capability";
  if (code.startsWith("Parse") || code.startsWith("Lexer")) return "Syntax";
  if (code.startsWith("L_")) {
    if (LEI_NAME_DIAGNOSTIC_CODES.has(code)) return "LEI/Name";
    if (LEI_TYPE_DIAGNOSTIC_CODES.has(code)) return "LEI/Type";
    return "LEI";
  }
  return undefined;
}

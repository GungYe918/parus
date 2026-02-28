import * as vscode from "vscode";

export function createLogger(
  outputChannel: vscode.OutputChannel | undefined
): (line: string) => void {
  return (line: string) => {
    outputChannel?.appendLine(`[${new Date().toISOString()}] ${line}`);
  };
}

export function toErrorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  return String(error);
}

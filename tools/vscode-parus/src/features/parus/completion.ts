import * as vscode from "vscode";

const PARUS_FALLBACK_KEYWORDS = [
  "def",
  "field",
  "proto",
  "class",
  "actor",
  "acts",
  "import",
  "use",
  "with",
  "require",
  "spawn",
  "commit",
  "recast",
  "init",
  "deinit",
] as const;

export function registerParusFallbackCompletionProvider(): vscode.Disposable {
  return vscode.languages.registerCompletionItemProvider(
    [{ language: "parus", scheme: "file" }],
    {
      provideCompletionItems() {
        const items: vscode.CompletionItem[] = [];
        for (const keyword of PARUS_FALLBACK_KEYWORDS) {
          const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
          item.detail = "Parus keyword (fallback)";
          items.push(item);
        }
        return items;
      },
    }
  );
}

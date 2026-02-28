import * as vscode from "vscode";

function wordRangeAt(document: vscode.TextDocument, position: vscode.Position): vscode.Range | undefined {
  return document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
}

export function registerFallbackDefinitionProvider(): vscode.Disposable {
  return vscode.languages.registerDefinitionProvider(
    [{ language: "parus", scheme: "file" }, { language: "lei", scheme: "file" }],
    {
      provideDefinition(document, position): vscode.ProviderResult<vscode.Definition> {
        const range = wordRangeAt(document, position);
        if (!range) return undefined;
        const symbol = document.getText(range);
        if (!symbol) return undefined;

        const fullText = document.getText();
        const escaped = symbol.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
        const re = new RegExp(`\\b${escaped}\\b`, "g");

        let match: RegExpExecArray | null;
        while ((match = re.exec(fullText)) !== null) {
          const start = document.positionAt(match.index);
          if (start.isEqual(position)) continue;
          const end = document.positionAt(match.index + symbol.length);
          return new vscode.Location(document.uri, new vscode.Range(start, end));
        }
        return undefined;
      },
    }
  );
}

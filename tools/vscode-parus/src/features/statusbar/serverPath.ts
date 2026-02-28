import * as vscode from "vscode";

interface ServerConfigLike {
  serverPath: string;
  serverMode: "driver" | "direct";
}

export function updateServerPathStatusBar(
  item: vscode.StatusBarItem | undefined,
  cfg: ServerConfigLike
): void {
  if (!item) return;
  const source = cfg.serverPath.trim() === "" ? "auto" : cfg.serverPath.trim();
  const modeLabel = cfg.serverMode === "direct" ? "direct" : "driver";
  item.text = `$(plug) Parus ${modeLabel}`;
  item.tooltip = `Parus Language Server\nmode: ${modeLabel}\npath: ${source}`;
}

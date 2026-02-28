import * as path from "node:path";

export type ServerMode = "driver" | "direct";

export interface ExecutableLike {
  command: string;
  args?: string[];
}

export function detectModeFromCommand(command: string, fallback: ServerMode): ServerMode {
  const base = path.basename(command).toLowerCase();
  if (base.startsWith("parusd")) return "direct";
  if (base.startsWith("parusc")) return "driver";
  return fallback;
}

export function formatExecutable(exec: ExecutableLike): string {
  const args = exec.args ?? [];
  if (args.length === 0) return exec.command;
  return `${exec.command} ${args.join(" ")}`;
}

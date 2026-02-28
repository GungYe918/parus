import * as vscode from "vscode";
import { Trace } from "vscode-languageclient/node";

export type ServerMode = "driver" | "direct";
export type TraceSetting = "off" | "messages" | "verbose";

export interface ParusConfig {
  serverPath: string;
  serverMode: ServerMode;
  traceServer: TraceSetting;
  diagnosticsDebounceMs: number;
  idleTimeoutSec: number;
}

const SETTINGS_SECTION = "parus";

export function readConfig(): ParusConfig {
  const cfg = vscode.workspace.getConfiguration(SETTINGS_SECTION);

  const serverPath = cfg.get<string>("server.path", "").trim();
  const rawMode = cfg.get<string>("server.mode", "driver");
  const mode: ServerMode = rawMode === "direct" ? "direct" : "driver";

  const rawTrace = cfg.get<string>("trace.server", "off");
  const traceServer: TraceSetting =
    rawTrace === "messages" || rawTrace === "verbose" ? rawTrace : "off";

  const debounceMs = cfg.get<number>("diagnostics.debounceMs", 200);
  const idleTimeoutSec = cfg.get<number>("server.idleTimeoutSec", 0);

  return {
    serverPath,
    serverMode: mode,
    traceServer,
    diagnosticsDebounceMs: clampNumber(debounceMs, 0, 5000),
    idleTimeoutSec: clampNumber(idleTimeoutSec, 0, 3600),
  };
}

export function toTraceLevel(trace: TraceSetting): Trace {
  switch (trace) {
    case "verbose":
      return Trace.Verbose;
    case "messages":
      return Trace.Messages;
    case "off":
    default:
      return Trace.Off;
  }
}

export function clampNumber(value: number, min: number, max: number): number {
  if (Number.isNaN(value)) return min;
  return Math.min(max, Math.max(min, value));
}

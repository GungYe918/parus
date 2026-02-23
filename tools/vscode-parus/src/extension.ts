import * as fs from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import * as path from "node:path";
import * as vscode from "vscode";
import {
  CloseAction,
  ErrorAction,
  type ErrorHandler,
  type Executable,
  LanguageClient,
  type LanguageClientOptions,
  RevealOutputChannelOn,
  type ServerOptions,
  Trace,
} from "vscode-languageclient/node";

type ServerMode = "driver" | "direct";
type TraceSetting = "off" | "messages" | "verbose";

interface ParusConfig {
  serverPath: string;
  serverMode: ServerMode;
  traceServer: TraceSetting;
  diagnosticsDebounceMs: number;
  idleTimeoutSec: number;
}

interface LaunchCandidate {
  mode: ServerMode;
  executable: Executable;
  reason?: string;
}

interface QueuedChange {
  event: vscode.TextDocumentChangeEvent;
  next: (event: vscode.TextDocumentChangeEvent) => Promise<void>;
  resolvers: Array<() => void>;
}

interface PendingChangeBucket {
  queue: QueuedChange[];
  timer?: NodeJS.Timeout;
  flushing: boolean;
}

type PathPickAction =
  | { kind: "auto" }
  | { kind: "workspace-parusc"; command: string }
  | { kind: "workspace-parusd"; command: string }
  | { kind: "finder" }
  | { kind: "input" };

interface PathPickItem extends vscode.QuickPickItem {
  action: PathPickAction;
}

const SUPPORTED_LANGUAGE_IDS = new Set(["parus", "lei"]);
const SETTINGS_SECTION = "parus";
const COMMAND_RESTART = "parus.restartLanguageServer";
const COMMAND_SHOW_LOGS = "parus.showLanguageServerLogs";
const COMMAND_CONFIGURE_SERVER_PATH = "parus.configureServerPath";
const STATUSBAR_PROBLEMS_COMMAND = "workbench.actions.view.problems";
const START_TIMEOUT_MS = 10_000;
const LEI_KEYWORDS = [
  "import",
  "from",
  "export",
  "proto",
  "plan",
  "let",
  "var",
  "def",
  "assert",
  "if",
  "else",
  "true",
  "false",
  "int",
  "float",
  "string",
  "bool",
  "return",
  "for",
  "in",
] as const;
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

function isSupportedLanguageId(languageId: string): boolean {
  return SUPPORTED_LANGUAGE_IDS.has(languageId);
}

let client: LanguageClient | undefined;
let outputChannel: vscode.OutputChannel | undefined;
let traceChannel: vscode.OutputChannel | undefined;
let serverPathStatusBarItem: vscode.StatusBarItem | undefined;
let lintStatusBarItem: vscode.StatusBarItem | undefined;
let changeDebouncer: ChangeDebouncer | undefined;
let idleStopController: IdleStopController | undefined;
let suppressConfigRestart = false;
const parusDiagnosticsByUri = new Map<string, vscode.Diagnostic[]>();
let lifecycleQueue: Promise<void> = Promise.resolve();

interface LeiSnippetSpec {
  label: string;
  detail: string;
  body: string;
}

const LEI_SNIPPETS: LeiSnippetSpec[] = [
  {
    label: "import/from",
    detail: "LEI import alias snippet",
    body: 'import ${1:alias} from "${2:./module.lei}";',
  },
  {
    label: "proto",
    detail: "LEI proto declaration snippet",
    body: "proto ${1:BuildConfig} {\n\t${2:name}: string;\n\t${3:jobs}: int = ${4:8};\n}",
  },
  {
    label: "plan",
    detail: "LEI plan declaration snippet",
    body: "plan ${1:master} {\n\t${2:key} = ${3:value};\n};",
  },
  {
    label: "def",
    detail: "LEI function declaration snippet",
    body: "def ${1:name}(${2:param}: ${3:string}) -> ${4:bool} {\n\t${5:return true;}\n}",
  },
  {
    label: "for",
    detail: "LEI for-in loop snippet",
    body: "for ${1:item} in ${2:items} {\n\t${3:// todo}\n}",
  },
  {
    label: "if/else",
    detail: "LEI if/else snippet",
    body: "if (${1:cond}) {\n\t${2:// then}\n} else {\n\t${3:// else}\n}",
  },
  {
    label: "assert",
    detail: "LEI assert snippet",
    body: "assert ${1:expr};",
  },
  {
    label: "export plan",
    detail: "LEI export plan snippet",
    body: "export plan ${1:release};",
  },
];

class ChangeDebouncer {
  private readonly buckets = new Map<string, PendingChangeBucket>();

  constructor(
    private readonly getDebounceMs: () => number,
    private readonly log: (line: string) => void
  ) {}

  // 변경 이벤트를 버리지 않고 순서대로 보내되, 전송 시작 타이밍만 디바운스한다.
  public readonly didChange = (
    event: vscode.TextDocumentChangeEvent,
    next: (event: vscode.TextDocumentChangeEvent) => Promise<void>
  ): Promise<void> => {
    const delay = Math.max(0, this.getDebounceMs());
    if (delay === 0) {
      return next(event);
    }

    const uri = event.document.uri.toString();
    const bucket = this.getOrCreateBucket(uri);
    return new Promise<void>((resolve) => {
      bucket.queue.push({
        event,
        next,
        resolvers: [resolve],
      });
      this.schedule(uri, bucket, delay);
    });
  };

  public readonly didClose = async (
    document: vscode.TextDocument,
    next: (document: vscode.TextDocument) => Promise<void>
  ): Promise<void> => {
    this.cancel(document.uri.toString());
    await next(document);
  };

  public dispose(): void {
    for (const [uri, bucket] of this.buckets) {
      this.clearTimer(bucket);
      this.resolveAndClear(bucket);
      this.buckets.delete(uri);
    }
  }

  private getOrCreateBucket(uri: string): PendingChangeBucket {
    const existing = this.buckets.get(uri);
    if (existing) {
      return existing;
    }
    const created: PendingChangeBucket = {
      queue: [],
      flushing: false,
    };
    this.buckets.set(uri, created);
    return created;
  }

  private schedule(uri: string, bucket: PendingChangeBucket, delayMs: number): void {
    if (bucket.flushing) {
      return;
    }
    this.clearTimer(bucket);
    bucket.timer = setTimeout(() => {
      void this.flush(uri);
    }, delayMs);
  }

  private async flush(uri: string): Promise<void> {
    const bucket = this.buckets.get(uri);
    if (!bucket || bucket.flushing) {
      return;
    }

    bucket.flushing = true;
    this.clearTimer(bucket);
    try {
      while (bucket.queue.length > 0) {
        const entry = bucket.queue.shift();
        if (!entry) {
          continue;
        }
        try {
          await entry.next(entry.event);
        } catch (error) {
          this.log(`문서 변경 알림 전송 실패 (${uri}): ${toErrorMessage(error)}`);
        } finally {
          for (const resolve of entry.resolvers) {
            resolve();
          }
        }
      }
    } finally {
      bucket.flushing = false;
      if (bucket.queue.length === 0) {
        this.buckets.delete(uri);
      } else {
        this.schedule(uri, bucket, Math.max(0, this.getDebounceMs()));
      }
    }
  }

  private cancel(uri: string): void {
    const bucket = this.buckets.get(uri);
    if (!bucket) {
      return;
    }
    this.clearTimer(bucket);
    this.resolveAndClear(bucket);
    this.buckets.delete(uri);
  }

  private clearTimer(bucket: PendingChangeBucket): void {
    if (!bucket.timer) {
      return;
    }
    clearTimeout(bucket.timer);
    bucket.timer = undefined;
  }

  private resolveAndClear(bucket: PendingChangeBucket): void {
    while (bucket.queue.length > 0) {
      const entry = bucket.queue.shift();
      if (!entry) {
        continue;
      }
      for (const resolve of entry.resolvers) {
        resolve();
      }
    }
  }
}

class IdleStopController {
  private timer: NodeJS.Timeout | undefined;

  constructor(
    private readonly getIdleTimeoutSec: () => number,
    private readonly onIdle: () => Promise<void>
  ) {}

  public touch(): void {
    const timeoutSec = Math.max(0, this.getIdleTimeoutSec());
    if (timeoutSec <= 0) {
      this.clear();
      return;
    }

    this.clear();
    this.timer = setTimeout(() => {
      this.timer = undefined;
      void this.onIdle();
    }, timeoutSec * 1000);
  }

  public clear(): void {
    if (!this.timer) {
      return;
    }
    clearTimeout(this.timer);
    this.timer = undefined;
  }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  outputChannel = vscode.window.createOutputChannel("Parus Language Server");
  traceChannel = vscode.window.createOutputChannel("Parus Language Server Trace");
  context.subscriptions.push(outputChannel, traceChannel);

  const log = getLogger();
  log("확장이 활성화되었습니다.");
  idleStopController = new IdleStopController(
    () => readConfig().idleTimeoutSec,
    async () => {
      if (!client) {
        return;
      }
      log("유휴 시간 제한에 도달하여 서버를 중지합니다.");
      await enqueueLifecycle(() => stopLanguageClient("idle-timeout"), "유휴 중지");
    }
  );

  context.subscriptions.push(
    vscode.commands.registerCommand(COMMAND_RESTART, async () => {
      log("사용자 명령으로 서버 재시작을 요청했습니다.");
      await enqueueLifecycle(() => restartLanguageClient("user-command"), "재시작 명령");
    }),
    vscode.commands.registerCommand(COMMAND_SHOW_LOGS, async () => {
      outputChannel?.show(true);
      traceChannel?.show(true);
    }),
    vscode.commands.registerCommand(COMMAND_CONFIGURE_SERVER_PATH, async () => {
      try {
        await configureServerPathFromStatusBar();
      } catch (error) {
        log(`서버 경로 설정 중 오류: ${toErrorMessage(error)}`);
        void vscode.window.showErrorMessage(
          `Parus: 서버 경로 설정 중 오류가 발생했습니다: ${toErrorMessage(error)}`
        );
      }
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      const affectsPath = event.affectsConfiguration(`${SETTINGS_SECTION}.server.path`);
      const affectsMode = event.affectsConfiguration(`${SETTINGS_SECTION}.server.mode`);
      const affectsTrace = event.affectsConfiguration(`${SETTINGS_SECTION}.trace.server`);
      const affectsDebounce = event.affectsConfiguration(
        `${SETTINGS_SECTION}.diagnostics.debounceMs`
      );
      const affectsIdle = event.affectsConfiguration(`${SETTINGS_SECTION}.server.idleTimeoutSec`);
      if (!affectsPath && !affectsMode && !affectsTrace && !affectsDebounce && !affectsIdle) {
        return;
      }

      const cfg = readConfig();
      if (affectsPath || affectsMode) {
        updateServerPathStatusBar(cfg);
      }

      if (affectsTrace && client) {
        await client.setTrace(toTraceLevel(cfg.traceServer));
        log(`trace 레벨을 '${cfg.traceServer}'로 갱신했습니다.`);
      }

      if (affectsDebounce) {
        log(`진단 디바운스 값을 ${cfg.diagnosticsDebounceMs}ms로 갱신했습니다.`);
      }
      if (affectsIdle) {
        log(`유휴 중지 타임아웃을 ${cfg.idleTimeoutSec}s로 갱신했습니다.`);
        idleStopController?.touch();
      }

      if (suppressConfigRestart) {
        return;
      }

      if (affectsPath || affectsMode) {
        log("서버 경로/모드 설정이 변경되어 서버를 재시작합니다.");
        await enqueueLifecycle(() => restartLanguageClient("config-change"), "설정 변경 재시작");
      }
    }),
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      refreshLintStatusBar();
      if (editor?.document && isSupportedLanguageId(editor.document.languageId)) {
        void onParusActivity("active-editor");
      }
    }),
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (isSupportedLanguageId(doc.languageId)) {
        void onParusActivity("did-open");
      }
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (isSupportedLanguageId(event.document.languageId)) {
        void onParusActivity("did-change");
      }
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      parusDiagnosticsByUri.delete(doc.uri.toString());
      refreshLintStatusBar();
    })
  );

  serverPathStatusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 110);
  serverPathStatusBarItem.command = {
    command: COMMAND_CONFIGURE_SERVER_PATH,
    title: "Parus: Configure Language Server Path",
  };
  serverPathStatusBarItem.show();
  context.subscriptions.push(serverPathStatusBarItem);

  lintStatusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 109);
  lintStatusBarItem.command = {
    command: STATUSBAR_PROBLEMS_COMMAND,
    title: "Open Problems",
  };
  lintStatusBarItem.show();
  context.subscriptions.push(lintStatusBarItem);
  context.subscriptions.push(registerLeiCompletionProvider());

  updateServerPathStatusBar(readConfig());
  refreshLintStatusBar();

  if (hasParusDocumentContext()) {
    void enqueueLifecycle(() => startLanguageClient("activate"), "초기 시작");
  } else {
    log("Parus/LEI 문서가 열릴 때까지 서버 시작을 지연합니다.");
  }
}

export async function deactivate(): Promise<void> {
  await stopLanguageClient("deactivate");
  idleStopController?.clear();
  serverPathStatusBarItem?.dispose();
  lintStatusBarItem?.dispose();
  outputChannel?.dispose();
  traceChannel?.dispose();
  serverPathStatusBarItem = undefined;
  lintStatusBarItem = undefined;
  idleStopController = undefined;
  outputChannel = undefined;
  traceChannel = undefined;
}

async function configureServerPathFromStatusBar(): Promise<void> {
  getLogger()("서버 경로 설정 UI를 엽니다.");
  // 커맨드 팔레트에서 호출된 경우 기존 Quick Open을 먼저 닫아야 picker가 안정적으로 뜬다.
  await vscode.commands.executeCommand("workbench.action.closeQuickOpen");
  await sleep(30);

  const cfg = readConfig();
  const workspaceRoot = getPrimaryWorkspaceFolder();

  const workspaceParusc = await resolveFromWorkspaceBuild("parusc", workspaceRoot);
  const workspaceParusd = await resolveFromWorkspaceBuild("parusd", workspaceRoot);

  const items: PathPickItem[] = [
    {
      label: "$(sync) 자동 탐색 사용",
      detail: "server.path를 비우고 parusc -> parusd fallback 순서로 탐색",
      action: { kind: "auto" },
    },
  ];

  if (workspaceParusc) {
    items.push({
      label: "$(tools) 워크스페이스 parusc 사용",
      detail: workspaceParusc,
      action: { kind: "workspace-parusc", command: workspaceParusc },
    });
  }
  if (workspaceParusd) {
    items.push({
      label: "$(tools) 워크스페이스 parusd 사용",
      detail: workspaceParusd,
      action: { kind: "workspace-parusd", command: workspaceParusd },
    });
  }
  items.push({
    label: "$(folder-opened) Finder에서 실행 파일 선택",
    detail: "macOS Finder에서 parusc/parusd 실행 파일 선택",
    action: { kind: "finder" },
  });
  items.push({
    label: "$(edit) 경로 직접 입력",
    detail: "절대/상대 경로 또는 PATH 상의 명령어를 직접 입력",
    action: { kind: "input" },
  });

  const picked = await vscode.window.showQuickPick(items, {
    placeHolder: "Parus Language Server 실행 경로를 선택하세요",
    title: "Parus: Configure Server Path",
    ignoreFocusOut: true,
    matchOnDescription: true,
    matchOnDetail: true,
  });
  if (!picked) {
    return;
  }

  if (picked.action.kind === "auto") {
    await applyServerPathConfig("", "driver");
    void vscode.window.showInformationMessage("Parus: 자동 탐색 모드로 전환했습니다.");
    return;
  }

  if (picked.action.kind === "workspace-parusc") {
    await applyServerPathConfig(picked.action.command, "driver");
    void vscode.window.showInformationMessage(
      `Parus: parusc 경로를 설정했습니다.\n${picked.action.command}`
    );
    return;
  }

  if (picked.action.kind === "workspace-parusd") {
    await applyServerPathConfig(picked.action.command, "direct");
    void vscode.window.showInformationMessage(
      `Parus: parusd 경로를 설정했습니다.\n${picked.action.command}`
    );
    return;
  }

  if (picked.action.kind === "finder") {
    const selected = await pickExecutableWithFinder(workspaceRoot);
    if (!selected) {
      return;
    }
    const mode = detectModeFromCommand(selected, cfg.serverMode);
    await applyServerPathConfig(selected, mode);
    void vscode.window.showInformationMessage(`Parus: 서버 경로를 설정했습니다 (${mode}).\n${selected}`);
    return;
  }

  const manualInput = await vscode.window.showInputBox({
    title: "Parus: Server Path",
    prompt: "parusc/parusd 실행 경로를 입력하세요. 비우면 자동 탐색으로 돌아갑니다.",
    value: cfg.serverPath,
    ignoreFocusOut: true,
  });
  if (manualInput === undefined) {
    return;
  }

  const trimmed = manualInput.trim();
  if (trimmed === "") {
    await applyServerPathConfig("", "driver");
    void vscode.window.showInformationMessage("Parus: 자동 탐색 모드로 전환했습니다.");
    return;
  }

  const resolved = await resolveConfiguredCommand(trimmed, workspaceRoot);
  const detectedMode = detectModeFromCommand(resolved, cfg.serverMode);
  await applyServerPathConfig(resolved, detectedMode);
  void vscode.window.showInformationMessage(
    `Parus: 서버 경로를 설정했습니다 (${detectedMode}).\n${resolved}`
  );
}

async function pickExecutableWithFinder(
  workspaceRoot: string | undefined
): Promise<string | undefined> {
  const startUri = workspaceRoot
    ? vscode.Uri.file(workspaceRoot)
    : vscode.Uri.file(process.env.HOME ?? "/");

  const selected = await vscode.window.showOpenDialog({
    title: "Parus Language Server 실행 파일 선택",
    defaultUri: startUri,
    canSelectFiles: true,
    canSelectFolders: false,
    canSelectMany: false,
    openLabel: "선택",
  });
  if (!selected || selected.length === 0) {
    return undefined;
  }

  const first = selected[0];
  if (!first) {
    return undefined;
  }
  const chosenPath = first.fsPath;
  const resolved = await resolveExecutableFile(chosenPath);
  if (!resolved) {
    void vscode.window.showErrorMessage(
      `Parus: 선택한 파일을 실행할 수 없습니다.\n${chosenPath}\n실행 권한(chmod +x) 또는 경로를 확인하세요.`
    );
    return undefined;
  }
  return resolved;
}

async function applyServerPathConfig(serverPath: string, mode: ServerMode): Promise<void> {
  const cfg = vscode.workspace.getConfiguration(SETTINGS_SECTION);
  const target = getConfigTarget();

  suppressConfigRestart = true;
  try {
    await cfg.update("server.path", serverPath, target);
    await cfg.update("server.mode", mode, target);
  } finally {
    suppressConfigRestart = false;
  }

  updateServerPathStatusBar(readConfig());
  await enqueueLifecycle(
    () => restartLanguageClient("statusbar-configure"),
    "상태바 경로 설정 재시작"
  );
}

function getConfigTarget(): vscode.ConfigurationTarget {
  return (vscode.workspace.workspaceFolders?.length ?? 0) > 0
    ? vscode.ConfigurationTarget.Workspace
    : vscode.ConfigurationTarget.Global;
}

function hasParusDocumentContext(): boolean {
  if (
    vscode.window.activeTextEditor?.document &&
    isSupportedLanguageId(vscode.window.activeTextEditor.document.languageId)
  ) {
    return true;
  }
  return vscode.workspace.textDocuments.some((doc) => isSupportedLanguageId(doc.languageId));
}

async function onParusActivity(reason: string): Promise<void> {
  idleStopController?.touch();
  if (client || !hasParusDocumentContext()) {
    return;
  }

  const log = getLogger();
  log(`Parus/LEI 문서 활동 감지로 서버 시작을 시도합니다. (reason=${reason})`);
  await enqueueLifecycle(() => startLanguageClient(`activity:${reason}`), "활동 기반 시작");
}

async function restartLanguageClient(reason: string): Promise<void> {
  await stopLanguageClient(`restart:${reason}`);
  await startLanguageClient(`restart:${reason}`);
}

async function startLanguageClient(reason: string): Promise<void> {
  const log = getLogger();
  if (client) {
    idleStopController?.touch();
    log(`서버 시작 요청을 건너뜁니다. 이미 실행 중입니다. (reason=${reason})`);
    return;
  }

  const cfg = readConfig();
  const workspaceRoot = getPrimaryWorkspaceFolder();

  warnMultiRootIfNeeded(cfg.serverPath);
  updateServerPathStatusBar(cfg);

  let candidates: LaunchCandidate[];
  try {
    candidates = await resolveLaunchCandidates(cfg, workspaceRoot);
  } catch (error) {
    const message = `서버 실행 정보 해석 실패: ${toErrorMessage(error)}`;
    log(message);
    await vscode.window.showErrorMessage(`Parus: ${message}`);
    return;
  }

  if (candidates.length === 0) {
    const message =
      "Parus 서버 실행 파일(parusc/parusd)을 찾지 못했습니다. `parus.server.path`를 설정하세요.";
    log(message);
    const action = await vscode.window.showErrorMessage(`Parus: ${message}`, "Open Settings");
    if (action === "Open Settings") {
      void vscode.commands.executeCommand("workbench.action.openSettings", "parus.server.path");
    }
    return;
  }

  changeDebouncer?.dispose();
  changeDebouncer = new ChangeDebouncer(() => readConfig().diagnosticsDebounceMs, log);

  const clientOptions: LanguageClientOptions = {
    documentSelector: Array.from(SUPPORTED_LANGUAGE_IDS).map((language) => ({
      scheme: "file" as const,
      language,
    })),
    outputChannel,
    traceOutputChannel: traceChannel,
    revealOutputChannelOn: RevealOutputChannelOn.Error,
    initializationFailedHandler: (error) => {
      log(`서버 초기화 실패: ${toErrorMessage(error)}`);
      return false;
    },
    middleware: {
      // didChange를 순서 보장 디바운스로 보내 서버 진단 품질을 안정화한다.
      didChange: (event, next) => changeDebouncer?.didChange(event, next) ?? next(event),
      didClose: (document, next) => changeDebouncer?.didClose(document, next) ?? next(document),
      // parusd 진단 code/source를 메시지와 상태바 요약에 반영한다.
      handleDiagnostics: (uri, diagnostics, next) => {
        const enhanced = enhanceDiagnostics(uri, diagnostics);
        cacheParusDiagnostics(uri, enhanced);
        next(uri, enhanced);
      },
    },
    connectionOptions: {
      maxRestartCount: 5,
    },
    errorHandler: createErrorHandler(log),
  };

  for (const [idx, candidate] of candidates.entries()) {
    log(
      `서버 시작 시도(${idx + 1}/${candidates.length}, reason=${reason}): ${formatExecutable(
        candidate.executable
      )}`
    );

    const nextClient = new LanguageClient(
      "parus-language-server",
      "Parus Language Server",
      candidate.executable as ServerOptions,
      clientOptions
    );
    nextClient.onDidChangeState((event) => {
      log(`클라이언트 상태 변경: ${event.oldState} -> ${event.newState}`);
    });

    client = nextClient;
    try {
      await withTimeout(
        nextClient.start(),
        START_TIMEOUT_MS,
        `LSP initialize 응답 대기 시간이 ${START_TIMEOUT_MS}ms를 초과했습니다.`
      );
      await nextClient.setTrace(toTraceLevel(cfg.traceServer));
      log(`서버 시작 성공: ${formatExecutable(candidate.executable)}`);
      if (candidate.reason) {
        log(candidate.reason);
      }
      idleStopController?.touch();
      return;
    } catch (error) {
      log(
        `서버 시작 실패: ${formatExecutable(candidate.executable)} / ${toErrorMessage(error)}`
      );
      await safeStopClient(nextClient, log);
      if (client === nextClient) {
        client = undefined;
      }
    }
  }

  await vscode.window.showErrorMessage(
    "Parus: Language Server 시작에 실패했습니다. Output 채널 로그를 확인하세요."
  );
}

async function stopLanguageClient(reason: string): Promise<void> {
  const log = getLogger();
  idleStopController?.clear();

  changeDebouncer?.dispose();
  changeDebouncer = undefined;

  parusDiagnosticsByUri.clear();
  refreshLintStatusBar();

  if (!client) {
    return;
  }

  const current = client;
  client = undefined;
  log(`서버 중지 요청: reason=${reason}`);
  await safeStopClient(current, log);
}

function readConfig(): ParusConfig {
  const cfg = vscode.workspace.getConfiguration(SETTINGS_SECTION);
  const rawPath = cfg.get<string>("server.path", "");
  const mode = cfg.get<string>("server.mode", "driver");
  const trace = cfg.get<string>("trace.server", "off");
  const debounceMs = cfg.get<number>("diagnostics.debounceMs", 200);
  const idleTimeoutSec = cfg.get<number>("server.idleTimeoutSec", 0);

  return {
    serverPath: rawPath.trim(),
    serverMode: mode === "direct" ? "direct" : "driver",
    traceServer: trace === "messages" || trace === "verbose" ? trace : "off",
    diagnosticsDebounceMs: clampNumber(debounceMs, 0, 5000),
    idleTimeoutSec: clampNumber(idleTimeoutSec, 0, 3600),
  };
}

function warnMultiRootIfNeeded(serverPath: string): void {
  const folders = vscode.workspace.workspaceFolders ?? [];
  if (folders.length <= 1 || serverPath !== "") {
    return;
  }

  const msg =
    "Parus: multi-root workspace에서는 첫 번째 워크스페이스만 자동 탐색합니다. 안정적인 동작을 위해 `parus.server.path`를 설정하세요.";
  outputChannel?.appendLine(msg);
  void vscode.window.showWarningMessage(msg);
}

function getPrimaryWorkspaceFolder(): string | undefined {
  const first = vscode.workspace.workspaceFolders?.[0];
  return first?.uri.fsPath;
}

async function resolveLaunchCandidates(
  cfg: ParusConfig,
  workspaceRoot: string | undefined
): Promise<LaunchCandidate[]> {
  const out: LaunchCandidate[] = [];

  if (cfg.serverPath !== "") {
    const resolvedCommand = await resolveConfiguredCommand(cfg.serverPath, workspaceRoot);
    const detectedMode = detectModeFromCommand(resolvedCommand, cfg.serverMode);
    out.push({
      mode: detectedMode,
      executable: buildExecutable(resolvedCommand, detectedMode, workspaceRoot),
      reason:
        detectedMode !== cfg.serverMode
          ? `server.path 파일명 기준으로 모드를 '${detectedMode}'로 자동 보정했습니다.`
          : undefined,
    });
    return out;
  }

  if (cfg.serverMode === "direct") {
    const direct = await resolveAutoCommand("parusd", workspaceRoot);
    if (direct) {
      out.push({
        mode: "direct",
        executable: buildExecutable(direct, "direct", workspaceRoot),
      });
    }
    return out;
  }

  const driver = await resolveAutoCommand("parusc", workspaceRoot);
  if (driver) {
    out.push({
      mode: "driver",
      executable: buildExecutable(driver, "driver", workspaceRoot),
    });
  }

  const direct = await resolveAutoCommand("parusd", workspaceRoot);
  if (direct) {
    out.push({
      mode: "direct",
      executable: buildExecutable(direct, "direct", workspaceRoot),
      reason: "driver 실행이 실패하면 direct(parusd --stdio)로 자동 fallback합니다.",
    });
  }

  return out;
}

async function resolveConfiguredCommand(
  inputPath: string,
  workspaceRoot: string | undefined
): Promise<string> {
  if (!inputPath.includes("/") && !inputPath.includes("\\")) {
    const inPath = await resolveInPath(inputPath);
    return inPath ?? inputPath;
  }

  const resolvedPath = path.isAbsolute(inputPath)
    ? inputPath
    : workspaceRoot
      ? path.resolve(workspaceRoot, inputPath)
      : path.resolve(inputPath);

  const normalized = await resolveExecutableFile(resolvedPath);
  if (!normalized) {
    throw new Error(`설정된 server.path를 찾을 수 없습니다: ${resolvedPath}`);
  }
  return normalized;
}

async function resolveAutoCommand(
  command: "parusc" | "parusd",
  workspaceRoot: string | undefined
): Promise<string | undefined> {
  const fromWorkspace = await resolveFromWorkspaceBuild(command, workspaceRoot);
  if (fromWorkspace) {
    return fromWorkspace;
  }
  return resolveInPath(command);
}

async function resolveFromWorkspaceBuild(
  command: "parusc" | "parusd",
  workspaceRoot: string | undefined
): Promise<string | undefined> {
  if (!workspaceRoot) {
    return undefined;
  }

  const candidates = [
    path.join(workspaceRoot, "build", "compiler", "parusc", command),
    path.join(workspaceRoot, "build", command),
  ];

  for (const candidate of candidates) {
    const resolved = await resolveExecutableFile(candidate);
    if (resolved) {
      return resolved;
    }
  }

  return undefined;
}

async function resolveInPath(command: string): Promise<string | undefined> {
  const pathEnv = process.env.PATH;
  if (!pathEnv) {
    return undefined;
  }
  const dirs = pathEnv.split(path.delimiter).filter((part) => part !== "");
  for (const dir of dirs) {
    for (const name of expandExecutableName(command)) {
      const candidate = path.join(dir, name);
      if (await isExecutableFile(candidate)) {
        return candidate;
      }
    }
  }
  return undefined;
}

async function resolveExecutableFile(basePath: string): Promise<string | undefined> {
  for (const candidate of expandExecutablePath(basePath)) {
    if (await isExecutableFile(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

function expandExecutableName(command: string): string[] {
  if (process.platform === "win32" && !command.toLowerCase().endsWith(".exe")) {
    return [command, `${command}.exe`];
  }
  return [command];
}

function expandExecutablePath(filePath: string): string[] {
  if (process.platform === "win32" && !filePath.toLowerCase().endsWith(".exe")) {
    return [filePath, `${filePath}.exe`];
  }
  return [filePath];
}

async function isExecutableFile(filePath: string): Promise<boolean> {
  try {
    await fs.access(filePath, fsConstants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function detectModeFromCommand(command: string, fallback: ServerMode): ServerMode {
  const base = path.basename(command).toLowerCase();
  if (base.startsWith("parusd")) {
    return "direct";
  }
  if (base.startsWith("parusc")) {
    return "driver";
  }
  return fallback;
}

function buildExecutable(
  command: string,
  mode: ServerMode,
  workspaceRoot: string | undefined
): Executable {
  const args = mode === "driver" ? ["lsp", "--stdio"] : ["--stdio"];
  return {
    command,
    args,
    options: workspaceRoot ? { cwd: workspaceRoot } : undefined,
  };
}

function formatExecutable(executable: Executable): string {
  const parts = [executable.command, ...(executable.args ?? [])];
  if (executable.options?.cwd) {
    return `${parts.join(" ")} (cwd=${executable.options.cwd})`;
  }
  return parts.join(" ");
}

function createErrorHandler(log: (line: string) => void): ErrorHandler {
  let closeCount = 0;
  let windowStart = 0;

  return {
    error(error, _message, count) {
      log(`LSP 통신 오류 (count=${count ?? 0}): ${toErrorMessage(error)}`);
      return {
        action: ErrorAction.Continue,
      };
    },
    closed() {
      const now = Date.now();
      if (now - windowStart > 60_000) {
        windowStart = now;
        closeCount = 0;
      }
      closeCount += 1;

      if (closeCount <= 5) {
        const delayHintMs = Math.min(250 * 2 ** (closeCount - 1), 5000);
        log(`서버 연결 종료. 자동 재시작 시도 #${closeCount} (백오프 힌트: ${delayHintMs}ms)`);
        return {
          action: CloseAction.Restart,
        };
      }

      const msg =
        "서버가 반복 종료되어 자동 재시작을 중단합니다. `Parus: Restart Language Server`를 실행하세요.";
      log(msg);
      void vscode.window.showErrorMessage(`Parus: ${msg}`);
      return {
        action: CloseAction.DoNotRestart,
      };
    },
  };
}

function registerLeiCompletionProvider(): vscode.Disposable {
  return vscode.languages.registerCompletionItemProvider(
    [{ language: "lei", scheme: "file" }],
    {
      provideCompletionItems(document, position) {
        const range = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
        const items: vscode.CompletionItem[] = [];

        for (const keyword of LEI_KEYWORDS) {
          const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
          item.detail = "LEI keyword";
          item.sortText = `0_${keyword}`;
          if (range) {
            item.range = range;
          }
          items.push(item);
        }

        for (const spec of LEI_SNIPPETS) {
          const item = new vscode.CompletionItem(spec.label, vscode.CompletionItemKind.Snippet);
          item.detail = spec.detail;
          item.insertText = new vscode.SnippetString(spec.body);
          item.sortText = `1_${spec.label}`;
          items.push(item);
        }

        return items;
      },
    },
    ".",
    ":",
    "{"
  );
}

function enhanceDiagnostics(
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
  const prefixParts: string[] = [];
  if (category) {
    prefixParts.push(category);
  }
  if (code) {
    prefixParts.push(code);
  }
  const prefix = prefixParts.length > 0 ? `[${prefixParts.join("/")}] ` : "";
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
  if (!range.start.isEqual(range.end)) {
    return range;
  }

  const doc = vscode.workspace.textDocuments.find((d) => d.uri.toString() === uri.toString());
  if (!doc || doc.lineCount === 0) {
    return range;
  }

  const line = clampNumber(range.start.line, 0, Math.max(0, doc.lineCount - 1));
  const lineText = doc.lineAt(line).text;
  if (lineText.length === 0) {
    const pos = new vscode.Position(line, 0);
    return new vscode.Range(pos, pos);
  }

  const startChar = clampNumber(range.start.character, 0, lineText.length - 1);
  const endChar = Math.min(lineText.length, startChar + 1);
  return new vscode.Range(new vscode.Position(line, startChar), new vscode.Position(line, endChar));
}

function diagnosticCodeToString(
  code: string | number | { value: string | number; target: vscode.Uri } | undefined
): string | undefined {
  if (code === undefined) {
    return undefined;
  }
  if (typeof code === "string" || typeof code === "number") {
    return String(code);
  }
  if ("value" in code) {
    return String(code.value);
  }
  return undefined;
}

function inferDiagnosticCategory(code: string | undefined): string | undefined {
  if (!code) {
    return undefined;
  }

  const normalized = code.trim();
  if (normalized.startsWith("C_")) {
    return "syntax";
  }
  if (LEI_NAME_DIAGNOSTIC_CODES.has(normalized)) {
    return "name";
  }
  if (LEI_TYPE_DIAGNOSTIC_CODES.has(normalized)) {
    return "type";
  }
  if (normalized.startsWith("L_") || normalized.startsWith("B_")) {
    return "lint";
  }

  if (/(Capability|Cap|Borrow|Immutable|Mut|Move|Escape)/i.test(normalized)) {
    return "cap";
  }
  if (/(Type|Tyck|Cast|Return|Param|Infer|Mismatch)/i.test(normalized)) {
    return "type";
  }
  if (/(Name|Resolve|Shadow|Duplicate|Decl|Scope|Symbol)/i.test(normalized)) {
    return "name";
  }
  if (
    /(Expected|Unexpected|Token|Eof|Parse|Semicolon|Bracket|Paren|Header|Body|Expr|Stmt|Recovery)/i.test(
      normalized
    )
  ) {
    return "syntax";
  }
  return "lint";
}

function cacheParusDiagnostics(uri: vscode.Uri, diagnostics: readonly vscode.Diagnostic[]): void {
  const key = uri.toString();
  const onlyParusd = diagnostics.filter((diag) => diag.source === "parusd");
  if (onlyParusd.length === 0) {
    parusDiagnosticsByUri.delete(key);
  } else {
    parusDiagnosticsByUri.set(key, [...onlyParusd]);
  }
  refreshLintStatusBar();
}

function refreshLintStatusBar(): void {
  if (!lintStatusBarItem) {
    return;
  }

  const editor = vscode.window.activeTextEditor;
  if (!editor || !isSupportedLanguageId(editor.document.languageId)) {
    lintStatusBarItem.hide();
    return;
  }

  const diagnostics = parusDiagnosticsByUri.get(editor.document.uri.toString()) ?? [];
  let errors = 0;
  let warnings = 0;
  let infos = 0;
  let hints = 0;

  for (const diag of diagnostics) {
    switch (diag.severity) {
      case vscode.DiagnosticSeverity.Error:
        errors += 1;
        break;
      case vscode.DiagnosticSeverity.Warning:
        warnings += 1;
        break;
      case vscode.DiagnosticSeverity.Information:
        infos += 1;
        break;
      case vscode.DiagnosticSeverity.Hint:
        hints += 1;
        break;
      default:
        break;
    }
  }

  if (diagnostics.length === 0) {
    lintStatusBarItem.text = "$(check) Parus/LEI Lint: clean";
    lintStatusBarItem.tooltip = "Parus/LEI: 현재 파일 진단 없음";
    lintStatusBarItem.show();
    return;
  }

  const parts = [`$(error) ${errors}`, `$(warning) ${warnings}`];
  if (infos > 0) {
    parts.push(`$(info) ${infos}`);
  }
  if (hints > 0) {
    parts.push(`$(light-bulb) ${hints}`);
  }

  lintStatusBarItem.text = `Parus/LEI Lint ${parts.join(" ")}`;
  lintStatusBarItem.tooltip = "Parus/LEI: Problems 보기 열기";
  lintStatusBarItem.show();
}

function updateServerPathStatusBar(cfg: ParusConfig): void {
  if (!serverPathStatusBarItem) {
    return;
  }

  const modeText = cfg.serverMode === "driver" ? "driver" : "direct";
  const pathText = cfg.serverPath === "" ? "auto" : cfg.serverPath;
  const shortPath = cfg.serverPath === "" ? "auto" : path.basename(cfg.serverPath);

  serverPathStatusBarItem.text = `$(server-process) Parus LS: ${shortPath}`;
  serverPathStatusBarItem.tooltip = [
    "Parus Language Server",
    `mode: ${modeText}`,
    `path: ${pathText}`,
    "",
    "클릭해서 서버 경로를 설정합니다.",
  ].join("\n");
  serverPathStatusBarItem.show();
}

function toTraceLevel(trace: TraceSetting): Trace {
  switch (trace) {
    case "messages":
      return Trace.Messages;
    case "verbose":
      return Trace.Verbose;
    case "off":
    default:
      return Trace.Off;
  }
}

function clampNumber(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) {
    return min;
  }
  return Math.min(max, Math.max(min, Math.floor(value)));
}

function toErrorMessage(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

function getLogger(): (line: string) => void {
  return (line: string) => {
    outputChannel?.appendLine(`[${new Date().toISOString()}] ${line}`);
  };
}

async function safeStopClient(
  target: LanguageClient,
  log: (line: string) => void
): Promise<void> {
  try {
    await target.stop();
  } catch (error) {
    log(`서버 중지 중 오류: ${toErrorMessage(error)}`);
  }
}

function enqueueLifecycle(
  task: () => Promise<void>,
  label: string
): Promise<void> {
  const log = getLogger();
  lifecycleQueue = lifecycleQueue
    .catch((error) => {
      log(`이전 라이프사이클 작업 오류(${label} 전): ${toErrorMessage(error)}`);
    })
    .then(async () => {
      try {
        await task();
      } catch (error) {
        log(`라이프사이클 작업 오류(${label}): ${toErrorMessage(error)}`);
      }
    });
  return lifecycleQueue;
}

async function withTimeout<T>(
  promise: Promise<T>,
  timeoutMs: number,
  timeoutMessage: string
): Promise<T> {
  let timer: NodeJS.Timeout | undefined;
  try {
    return await Promise.race<T>([
      promise,
      new Promise<T>((_resolve, reject) => {
        timer = setTimeout(() => {
          reject(new Error(timeoutMessage));
        }, timeoutMs);
      }),
    ]);
  } finally {
    if (timer) {
      clearTimeout(timer);
    }
  }
}

function sleep(ms: number): Promise<void> {
  return new Promise<void>((resolve) => {
    setTimeout(resolve, ms);
  });
}

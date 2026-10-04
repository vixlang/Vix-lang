// VS Code client for the Vix language server, the compile workflow, and the
// activity bar panel.
//
// Three independent parts live here:
//   1. The language server client (vix-analyzer over stdio).
//   2. Compile commands, which shell out to vixc. The analyzer never calls vixc.
//   3. The sidebar webview, which drives the same commands.

const fs = require("fs");
const path = require("path");
const cp = require("child_process");
const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");
const { VixPanelProvider } = require("./panel");

const AUTO_COMPILE_ARGS = "--check";

let client;
let compileOutput;
let compileDiagnostics;
let terminal;
const autoCompileTimers = new Map();

const panelState = {
  compilerPath: "vixc",
  compilerFound: false,
  compilerVersion: "",
  compilerSource: "",
  serverPath: "",
  serverFound: false,
  serverSource: "bundled",
  optLevel: 0,
  status: "idle",
  message: "Ready",
  meta: "no build yet"
};

let extensionContext;

function configuration() {
  return vscode.workspace.getConfiguration("vix");
}

function compilerPath() {
  return configuration().get("compilerPath", "vixc");
}

function errorFormat() {
  return configuration().get("compilerErrorFormat", "bootstrap");
}

function serverPathSetting() {
  return vscode.workspace.getConfiguration("vixAnalyzer").get("serverPath", "");
}

function bundledServerPath(context) {
  const bundled = context.asAbsolutePath(path.join("server", "vix-analyzer"));
  try {
    fs.chmodSync(bundled, 0o755);
  } catch (error) {
    // The archive may already carry the executable bit.
  }
  return bundled;
}

function resolveServerPath(context) {
  const configured = serverPathSetting();
  if (configured && configured.length > 0) {
    return configured;
  }
  return bundledServerPath(context);
}

// Runs "<file> --version". A bare name is looked up on PATH; a path has to exist.
function probeCommand(command, done) {
  if (!command) {
    done(false, "");
    return;
  }
  const looksLikePath = command.indexOf("/") >= 0 || command.indexOf("\\") >= 0;
  if (looksLikePath && !fs.existsSync(command)) {
    done(false, "");
    return;
  }
  cp.execFile(command, ["--version"], { timeout: 5000 }, (error, stdout, stderr) => {
    if (error && (error.code === "ENOENT" || error.code === "EACCES" || error.code === "EPERM")) {
      done(false, "");
      return;
    }
    const text = (String(stdout || "") + String(stderr || "")).trim().split("\n")[0] || "";
    done(true, text);
  });
}

// Places a compiler may live when it is not on PATH. The repository layout is
// the most likely one while developing Vix itself.
function compilerCandidates(configured) {
  const candidates = [];
  if (configured && configured.length > 0) {
    candidates.push(configured);
  }
  candidates.push("vixc");
  const root = workspaceRoot();
  if (root) {
    candidates.push(path.join(root, "build", "vixc-patched"));
    candidates.push(path.join(root, "build", "vixc"));
  }
  candidates.push("/usr/local/bin/vixc");
  candidates.push("/opt/homebrew/bin/vixc");
  return candidates;
}

function workspaceRoot() {
  const folders = vscode.workspace.workspaceFolders;
  return folders && folders.length > 0 ? folders[0].uri.fsPath : undefined;
}

function createClient(context) {
  const command = resolveServerPath(context);
  const cwd = workspaceRoot();
  const serverOptions = {
    run: { command: command, transport: TransportKind.stdio, options: { cwd: cwd } },
    debug: { command: command, transport: TransportKind.stdio, options: { cwd: cwd } }
  };
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "vix" }],
    outputChannelName: "Vix Analyzer"
  };
  return new LanguageClient("vixAnalyzer", "Vix Analyzer", serverOptions, clientOptions);
}

// --- vixc diagnostics -------------------------------------------------------

function severityOf(word) {
  if (word === "warning") {
    return vscode.DiagnosticSeverity.Warning;
  }
  if (word === "note" || word === "info") {
    return vscode.DiagnosticSeverity.Information;
  }
  return vscode.DiagnosticSeverity.Error;
}

function parseBootstrapDiagnostics(text) {
  const diagnostics = [];
  let pending = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const header = /^(error|warning|note|info)\[([^\]]+)\]\s*:\s*(.*)$/.exec(line);
    if (header) {
      pending = { severity: severityOf(header[1]), code: header[2], message: header[3].trim() };
      continue;
    }
    if (!pending) {
      continue;
    }
    const pointer = /^-->\s+(.+?):(\d+):(\d+)\s*$/.exec(line);
    if (pointer) {
      const lineNumber = Math.max(0, parseInt(pointer[2], 10) - 1);
      const column = Math.max(0, parseInt(pointer[3], 10) - 1);
      diagnostics.push({
        range: new vscode.Range(lineNumber, column, lineNumber, column + 1),
        severity: pending.severity,
        code: pending.code,
        source: "vixc",
        message: pending.message
      });
      pending = null;
      continue;
    }
    if (line.length > 0 && !line.startsWith("|") && !line.startsWith("=") && !/^\d+\s*\|/.test(line)) {
      pending.message = line;
    }
  }
  return diagnostics;
}

function parseCCppDiagnostics(text) {
  const diagnostics = [];
  for (const raw of text.split(/\r?\n/)) {
    const match = /^(.+?):(\d+):(\d+):\s*(error|warning|note|fatal error)\s*:\s*(.*)$/.exec(raw.trim());
    if (!match) {
      continue;
    }
    const lineNumber = Math.max(0, parseInt(match[2], 10) - 1);
    const column = Math.max(0, parseInt(match[3], 10) - 1);
    diagnostics.push({
      range: new vscode.Range(lineNumber, column, lineNumber, column + 1),
      severity: severityOf(match[4] === "fatal error" ? "error" : match[4]),
      code: "",
      source: "vixc",
      message: match[5].trim()
    });
  }
  return diagnostics;
}

function diagnosticsFrom(text) {
  const format = errorFormat();
  if (format === "none") {
    return [];
  }
  return format === "c-cpp" ? parseCCppDiagnostics(text) : parseBootstrapDiagnostics(text);
}

// --- compiler invocation ----------------------------------------------------

function baseName(filePath) {
  return filePath.replace(/\.[^./\\]+$/, "");
}

function buildArgs(kind, filePath, level) {
  const stem = baseName(filePath);
  if (kind === "check") {
    return ["--check", filePath];
  }
  if (kind === "asm") {
    return ["-S", "-opt=l" + level, "-o", stem + ".s", filePath];
  }
  if (kind === "obj") {
    return ["-obj", "-opt=l" + level, "-o", stem + ".o", filePath];
  }
  return ["-opt=l" + level, "-o", stem, filePath];
}

function setStatus(status, message, meta) {
  panelState.status = status;
  panelState.message = message;
  if (meta !== undefined) {
    panelState.meta = meta;
  }
  pushState();
}

function pushState() {
  if (panelProvider) {
    panelProvider.push();
  }
}

function compileDocument(document, extraArgs, quiet) {
  const filePath = document.uri.fsPath;
  const compiler = compilerPath();
  const args = [filePath, ...String(extraArgs).split(/\s+/).filter(Boolean)];
  if (!quiet) {
    compileOutput.appendLine("$ " + compiler + " " + args.join(" "));
  }
  return new Promise((resolve) => {
    cp.execFile(compiler, args, { cwd: path.dirname(filePath), maxBuffer: 32 * 1024 * 1024 }, (error, stdout, stderr) => {
      if (stdout) {
        compileOutput.appendLine(stdout.trimEnd());
      }
      if (stderr) {
        compileOutput.appendLine(stderr.trimEnd());
      }
      if (error && error.code === "ENOENT") {
        vscode.window.showErrorMessage("Vix: compiler not found at '" + compiler + "'. Set vix.compilerPath.");
        resolve({ ok: false, missing: true });
        return;
      }
      const diagnostics = diagnosticsFrom(String(stderr || "") + "\n" + String(stdout || ""));
      compileDiagnostics.set(document.uri, diagnostics);
      resolve({ ok: !error, diagnostics: diagnostics });
    });
  });
}

// Runs "kind" against a document and reports progress to the panel.
async function runBuild(kind, document) {
  const filePath = document.uri.fsPath;
  const compiler = compilerPath();
  const args = buildArgs(kind, filePath, panelState.optLevel);
  const started = Date.now();
  setStatus("busy", "Running " + kind + "…");

  compileOutput.appendLine("$ " + compiler + " " + args.join(" "));
  const result = await new Promise((resolve) => {
    cp.execFile(compiler, args, { cwd: path.dirname(filePath), maxBuffer: 32 * 1024 * 1024 }, (error, stdout, stderr) => {
      if (stdout) {
        compileOutput.appendLine(stdout.trimEnd());
      }
      if (stderr) {
        compileOutput.appendLine(stderr.trimEnd());
      }
      resolve({ error: error, stdout: String(stdout || ""), stderr: String(stderr || "") });
    });
  });
  const elapsed = Date.now() - started;
  const diagnostics = diagnosticsFrom(result.stderr + "\n" + result.stdout);
  compileDiagnostics.set(document.uri, diagnostics);
  const errors = diagnostics.filter((d) => d.severity === vscode.DiagnosticSeverity.Error).length;

  if (result.error && result.error.code === "ENOENT") {
    setStatus("error", "vixc not found", compiler);
    vscode.window.showErrorMessage("Vix: compiler not found at '" + compiler + "'. Set the path in the Vix panel.");
    return { ok: false };
  }
  if (result.error) {
    setStatus("error", kind + " failed · " + errors + " error(s)", elapsed + " ms · exit " + result.error.code);
  } else {
    setStatus("ok", kind + " succeeded", elapsed + " ms");
  }
  return { ok: !result.error, diagnostics: diagnostics };
}

function activeVixDocument() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "vix") {
    vscode.window.showWarningMessage("Vix: open a .vix file first.");
    return undefined;
  }
  return editor.document;
}

function compileOptions() {
  const options = [];
  for (const level of [0, 1, 2, 3]) {
    options.push({ label: "$(play) Compile (l" + level + ")", description: "Build an executable", args: "-opt=l" + level, kind: "build" });
  }
  for (const level of [0, 1, 2, 3]) {
    options.push({ label: "$(symbol-method) Assembly (l" + level + ")", description: "Emit assembly (-S)", args: "-S -opt=l" + level, kind: "asm" });
  }
  for (const level of [0, 1, 2, 3]) {
    options.push({ label: "$(package) Object File (l" + level + ")", description: "Emit an object file (-obj)", args: "-obj -opt=l" + level, kind: "obj" });
  }
  return options;
}

function scheduleAutoCompile(document) {
  if (document.languageId !== "vix" || !configuration().get("autoCompile", true)) {
    return;
  }
  const key = document.uri.toString();
  const pending = autoCompileTimers.get(key);
  if (pending) {
    clearTimeout(pending);
  }
  const delay = configuration().get("autoCompileDelay", 700);
  autoCompileTimers.set(key, setTimeout(() => {
    autoCompileTimers.delete(key);
    compileDocument(document, AUTO_COMPILE_ARGS, true);
  }, delay));
}

// Writing to the Workspace target throws when no folder is open, which used to
// make the path unsettable. Fall back to the User (global) target.
async function updateSetting(section, key, value) {
  const settings = vscode.workspace.getConfiguration(section);
  const folders = vscode.workspace.workspaceFolders;
  const target = folders && folders.length > 0
    ? vscode.ConfigurationTarget.Workspace
    : vscode.ConfigurationTarget.Global;
  try {
    await settings.update(key, value, target);
  } catch (error) {
    await settings.update(key, value, vscode.ConfigurationTarget.Global);
  }
}

async function browseForCompiler() {
  const picked = await vscode.window.showOpenDialog({
    title: "Select the vixc executable",
    canSelectMany: false,
    openLabel: "Use this executable"
  });
  if (picked && picked.length > 0) {
    await setCompilerPath(picked[0].fsPath);
  }
}

async function browseForServer() {
  const picked = await vscode.window.showOpenDialog({
    title: "Select the vix-analyzer executable",
    canSelectMany: false,
    openLabel: "Use this executable"
  });
  if (picked && picked.length > 0) {
    await setServerPath(picked[0].fsPath);
  }
}

async function setCompilerPath(value) {
  await updateSetting("vix", "compilerPath", value);
  panelState.compilerPath = value;
  refreshCompilerState();
  vscode.window.showInformationMessage(value ? "Vix: compiler set to " + value : "Vix: compiler reset to PATH lookup");
}

async function setServerPath(value) {
  await updateSetting("vixAnalyzer", "serverPath", value);
  refreshServerState();
  await restartLanguageServer();
}

// Probes the configured compiler, then common locations, and reports which one
// answered so the field can show something the user can act on.
function refreshCompilerState() {
  const configured = compilerPath();
  const candidates = compilerCandidates(configured);
  let index = 0;
  const tryNext = () => {
    if (index >= candidates.length) {
      panelState.compilerPath = configured;
      panelState.compilerFound = false;
      panelState.compilerVersion = "";
      panelState.compilerSource = "";
      panelState.message = "vixc not found";
      panelState.meta = "pick the executable with Browse, or type its path";
      pushState();
      return;
    }
    const candidate = candidates[index];
    index += 1;
    probeCommand(candidate, (ok, version) => {
      if (!ok) {
        tryNext();
        return;
      }
      panelState.compilerPath = candidate;
      panelState.compilerFound = true;
      panelState.compilerVersion = version;
      panelState.compilerSource = candidate === configured && configured.indexOf("/") >= 0 ? "setting" : "detected";
      panelState.message = "Ready";
      panelState.meta = "vixc · " + candidate;
      pushState();
    });
  };
  tryNext();
}

function refreshServerState() {
  const configured = serverPathSetting();
  const target = configured && configured.length > 0 ? configured : bundledServerPath(extensionContext);
  panelState.serverPath = target;
  panelState.serverSource = configured && configured.length > 0 ? "setting" : "bundled";
  probeCommand(target, (ok) => {
    panelState.serverFound = ok;
    pushState();
  });
}

async function restartLanguageServer() {
  if (client) {
    try {
      await client.stop();
    } catch (error) {
      // A failed stop should not block starting a new client.
    }
  }
  const target = resolveServerPath(extensionContext);
  if (!fs.existsSync(target)) {
    panelState.serverFound = false;
    pushState();
    vscode.window.showErrorMessage("Vix: language server not found at " + target + ".");
    return;
  }
  client = createClient(extensionContext);
  await client.start();
  panelState.serverFound = true;
  pushState();
}

// --- activation -------------------------------------------------------------

let panelProvider;

async function activate(context) {
  extensionContext = context;
  compileOutput = vscode.window.createOutputChannel("Vix Compiler");
  compileDiagnostics = vscode.languages.createDiagnosticCollection("vixc");
  context.subscriptions.push(compileOutput, compileDiagnostics);

  panelProvider = new VixPanelProvider(context, panelState);
  context.subscriptions.push(
    vscode.window.registerWebviewViewProvider("vix.toolbar", panelProvider)
  );
  panelProvider.onAction = async (message) => {
    if (message.type === "ready") {
      refreshCompilerState();
      refreshServerState();
      pushState();
      return;
    }
    if (message.type === "setOpt") {
      panelState.optLevel = message.level;
      pushState();
      return;
    }
    if (message.type === "setCompiler") {
      await setCompilerPath(message.value || "");
      return;
    }
    if (message.type === "detectCompiler") {
      await setCompilerPath("");
      return;
    }
    if (message.type === "setServer") {
      await setServerPath(message.value || "");
      return;
    }
    if (message.type === "useBundledServer") {
      await setServerPath("");
      vscode.window.showInformationMessage("Vix: using the bundled language server.");
      return;
    }
    if (message.type === "restartServer") {
      await restartLanguageServer();
      vscode.window.showInformationMessage("Vix: language server restarted.");
      return;
    }
    if (message.type === "browseServer") {
      await browseForServer();
      return;
    }
    if (message.type === "pickCompiler" || message.type === "browseCompiler") {
      await browseForCompiler();
      return;
    }
    if (message.type === "openCompilerSetting") {
      vscode.commands.executeCommand("workbench.action.openSettings", "vix.compilerPath");
      return;
    }
    if (message.type === "revealOutput") {
      compileOutput.show();
      return;
    }
    if (message.type === "openSettings") {
      vscode.commands.executeCommand("workbench.action.openSettings", "vix");
      return;
    }
    const document = activeVixDocument();
    if (!document) {
      return;
    }
    if (message.action === "run") {
      const result = await runBuild("build", document);
      if (result.ok) {
        terminal = terminal || vscode.window.createTerminal("Vix");
        terminal.show();
        terminal.sendText('"' + baseName(document.uri.fsPath) + '"');
      }
      return;
    }
    if (message.action === "build" || message.action === "check" || message.action === "asm" || message.action === "obj") {
      await runBuild(message.action, document);
    }
  };

  refreshCompilerState();
  refreshServerState();

  const serverPath = resolveServerPath(context);
  if (fs.existsSync(serverPath)) {
    client = createClient(context);
    context.subscriptions.push(client);
    await client.start();
  } else {
    vscode.window.showErrorMessage(
      "vix-analyzer server not found at " + serverPath +
      ". Set the path in the Vix panel, or build it with scripts/build-analyzer.sh."
    );
  }

  const runWithArgs = (args) => () => {
    const document = activeVixDocument();
    if (document) {
      compileDocument(document, args);
    }
  };

  const registrations = [
    vscode.commands.registerCommand("vix.showCompilerOutput", () => compileOutput.show()),
    vscode.commands.registerCommand("vix.showPanel", () => vscode.commands.executeCommand("vix.toolbar.focus")),
    vscode.commands.registerCommand("vix.selectCompiler", () => browseForCompiler()),
    vscode.commands.registerCommand("vix.selectLanguageServer", () => browseForServer()),
    vscode.commands.registerCommand("vix.compile", async () => {
      const document = activeVixDocument();
      if (!document) {
        return;
      }
      const picked = await vscode.window.showQuickPick(compileOptions(), { title: "Vix: Compile", placeHolder: "Select a build target" });
      if (picked) {
        await runBuild(picked.kind, document);
      }
    }),
    vscode.commands.registerCommand("vix.run", async () => {
      const document = activeVixDocument();
      if (!document) {
        return;
      }
      const result = await runBuild("build", document);
      if (result.ok) {
        terminal = terminal || vscode.window.createTerminal("Vix");
        terminal.show();
        terminal.sendText('"' + baseName(document.uri.fsPath) + '"');
      }
    }),
    vscode.commands.registerCommand("vix.check", async () => {
      const document = activeVixDocument();
      if (document) {
        await runBuild("check", document);
      }
    }),
    vscode.commands.registerCommand("vix.restartLanguageServer", async () => {
      await restartLanguageServer();
      vscode.window.showInformationMessage("Vix: language server restarted.");
    }),
    vscode.commands.registerCommand("vix.reloadWindow", () => vscode.commands.executeCommand("workbench.action.reloadWindow")),
    vscode.commands.registerCommand("vix.formatDocument", () => vscode.commands.executeCommand("editor.action.formatDocument"))
  ];

  for (const level of [0, 1, 2, 3]) {
    registrations.push(vscode.commands.registerCommand("vix.compile.l" + level, runWithArgs("-opt=l" + level)));
    registrations.push(vscode.commands.registerCommand("vix.compile.asm.l" + level, runWithArgs("-S -opt=l" + level)));
    registrations.push(vscode.commands.registerCommand("vix.compile.obj.l" + level, runWithArgs("-obj -opt=l" + level)));
  }

  registrations.push(vscode.workspace.onDidSaveTextDocument((document) => {
    if (document.languageId === "vix") {
      compileDocument(document, AUTO_COMPILE_ARGS, true);
    }
  }));
  registrations.push(vscode.workspace.onDidChangeTextDocument((event) => scheduleAutoCompile(event.document)));
  registrations.push(vscode.workspace.onDidCloseTextDocument((document) => compileDiagnostics.delete(document.uri)));
  registrations.push(vscode.workspace.onDidChangeConfiguration((event) => {
    if (event.affectsConfiguration("vix.compilerPath")) {
      refreshCompilerState();
    }
    if (event.affectsConfiguration("vixAnalyzer.serverPath")) {
      refreshServerState();
    }
  }));

  context.subscriptions.push(...registrations);
}

function deactivate() {
  for (const timer of autoCompileTimers.values()) {
    clearTimeout(timer);
  }
  autoCompileTimers.clear();
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
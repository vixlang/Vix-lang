// Webview for the Vix activity bar panel.
//
// The panel has to answer two questions immediately: which executables is the
// extension using, and what do I do when one is missing. Both paths are shown as
// editable fields with Browse / Detect / Reset next to them.
const vscode = require("vscode");

function nonce() {
  let text = "";
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  for (let i = 0; i < 32; i++) {
    text += alphabet.charAt(Math.floor(Math.random() * alphabet.length));
  }
  return text;
}

function renderPanelHtml() {
  const n = nonce();
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${n}';">
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    padding: 12px;
    font-family: var(--vscode-font-family);
    font-size: 12px;
    color: var(--vscode-foreground);
    background: var(--vscode-sideBar-background, var(--vscode-editor-background));
    user-select: none;
  }
  .brand { display: flex; align-items: center; gap: 8px; margin-bottom: 12px; }
  .brand svg { width: 20px; height: 20px; color: var(--vscode-activityBar-foreground, currentColor); }
  .brand-title { font-size: 13px; font-weight: 600; letter-spacing: .3px; }

  .card {
    border: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25));
    border-radius: 8px;
    padding: 10px;
    margin-bottom: 10px;
  }
  .card-label {
    display: flex; align-items: center; gap: 6px;
    text-transform: uppercase; letter-spacing: .6px; font-size: 10px;
    color: var(--vscode-descriptionForeground); margin-bottom: 8px;
  }
  .card-label .spacer { margin-left: auto; text-transform: none; letter-spacing: 0; }

  .field { display: flex; align-items: center; gap: 6px; }
  input.path {
    flex: 1; min-width: 0;
    font-family: var(--vscode-editor-font-family, monospace); font-size: 11px;
    color: var(--vscode-input-foreground);
    background: var(--vscode-input-background);
    border: 1px solid var(--vscode-input-border, rgba(128,128,128,.35));
    border-radius: 6px; padding: 5px 7px;
  }
  input.path:focus { outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
  input.path.bad { border-color: var(--vscode-errorForeground, #f85149); }

  button {
    font-family: inherit; font-size: 11px; cursor: pointer;
    color: var(--vscode-button-secondaryForeground, var(--vscode-foreground));
    background: var(--vscode-button-secondaryBackground, rgba(128,128,128,.16));
    border: 1px solid transparent; border-radius: 6px; padding: 5px 8px;
    white-space: nowrap;
  }
  button:hover { background: var(--vscode-button-secondaryHoverBackground, rgba(128,128,128,.26)); }
  button.icon { padding: 5px 7px; }
  button.primary {
    background: var(--vscode-button-background); color: var(--vscode-button-foreground); font-weight: 600;
  }
  button.primary:hover { background: var(--vscode-button-hoverBackground); }
  button[disabled] { opacity: .5; cursor: default; }

  .status { display: flex; align-items: center; gap: 6px; margin-top: 8px; font-size: 11px; }
  .dot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: var(--vscode-descriptionForeground); }
  .dot.ok { background: var(--vscode-testing-iconPassed, #3fb950); }
  .dot.bad { background: var(--vscode-errorForeground, #f85149); }
  .muted { color: var(--vscode-descriptionForeground); }
  .help { margin-top: 6px; font-size: 10px; line-height: 1.5; color: var(--vscode-descriptionForeground); }

  .hint {
    margin-top: 8px; padding: 8px; border-radius: 6px;
    background: var(--vscode-inputValidation-warningBackground, rgba(210,153,34,.12));
    border: 1px solid var(--vscode-inputValidation-warningBorder, rgba(210,153,34,.5));
    font-size: 11px; line-height: 1.5;
  }
  .hint .row { display: flex; gap: 6px; margin-top: 8px; flex-wrap: wrap; }

  .seg { display: flex; border-radius: 6px; overflow: hidden; border: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25)); }
  .seg button { flex: 1; border: 0; border-radius: 0; padding: 6px 0; background: transparent; }
  .seg button.active { background: var(--vscode-button-background); color: var(--vscode-button-foreground); font-weight: 600; }

  .actions { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .actions .wide { grid-column: 1 / -1; }
  button.action { display: flex; align-items: center; justify-content: center; gap: 6px; padding: 8px 6px; font-size: 12px; }
  button.action svg { width: 13px; height: 13px; }

  .links { display: flex; gap: 12px; color: var(--vscode-textLink-foreground); font-size: 11px; margin-top: 4px; }
  .links span { cursor: pointer; }
  .links span:hover { text-decoration: underline; }
</style>
</head>
<body>
  <div class="brand">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
      <path d="M3.5 4.5 12 20 20.5 4.5"/><path d="M8.2 4.5 12 11.2 15.8 4.5"/>
    </svg>
    <div class="brand-title">Vix</div>
  </div>

  <div class="card">
    <div class="card-label">
      <span>Compiler</span>
      <span class="spacer muted" id="compiler-badge">checking…</span>
    </div>
    <div class="field">
      <input class="path" id="compiler-path" spellcheck="false" placeholder="path to vixc, or just vixc if it is on PATH">
      <button class="icon" id="compiler-browse" title="Choose the vixc executable">Browse</button>
    </div>
    <div class="status"><div class="dot" id="compiler-dot"></div><span id="compiler-status">not checked</span></div>
    <div class="help">Builds, runs and type-checks your files. The analyzer never calls it; it is only used by the buttons below.</div>
    <div id="compiler-hint"></div>
  </div>

  <div class="card">
    <div class="card-label">
      <span>Language server</span>
      <span class="spacer muted" id="server-badge">checking…</span>
    </div>
    <div class="field">
      <input class="path" id="server-path" spellcheck="false" placeholder="path to vix-analyzer">
      <button class="icon" id="server-browse" title="Choose the vix-analyzer executable">Browse</button>
    </div>
    <div class="status"><div class="dot" id="server-dot"></div><span id="server-status">not checked</span></div>
    <div class="help">Provides diagnostics, hover, go-to-definition, references, rename and formatting. A copy ships inside this extension.</div>
    <div id="server-hint"></div>
  </div>

  <div class="card">
    <div class="card-label">Optimization</div>
    <div class="seg" id="opt">
      <button data-level="0">l0</button>
      <button data-level="1">l1</button>
      <button data-level="2">l2</button>
      <button data-level="3">l3</button>
    </div>
  </div>

  <div class="card">
    <div class="card-label">Actions</div>
    <div class="actions">
      <button class="action primary wide" data-action="run">
        <svg viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg> Run
      </button>
      <button class="action" data-action="build">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3v12"/><path d="M7 10l5 5 5-5"/><path d="M4 21h16"/></svg> Build
      </button>
      <button class="action" data-action="check">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><path d="M4 12.5 9.5 18 20 6.5"/></svg> Check
      </button>
      <button class="action" data-action="asm">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 5 4 12l5 7"/><path d="M15 5l5 7-5 7"/></svg> Assembly
      </button>
      <button class="action" data-action="obj">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 8l8-4 8 4-8 4-8-4z"/><path d="M4 8v8l8 4 8-4V8"/></svg> Object
      </button>
      <button class="action wide" id="restart">Restart language server</button>
    </div>
  </div>

  <div class="card">
    <div class="card-label">Status</div>
    <div class="status" id="status"><div class="dot" id="status-dot"></div><span id="status-text">Ready</span></div>
    <div class="help" id="status-meta">no build yet</div>
  </div>

  <div class="links">
    <span id="output">Show output</span>
    <span id="settings">All settings</span>
  </div>

<script nonce="${n}">
  const vscode = acquireVsCodeApi();
  const $ = (id) => document.getElementById(id);
  let state = null;

  function hint(target, text, buttons) {
    const host = $(target);
    host.innerHTML = "";
    if (!text) return;
    const box = document.createElement("div");
    box.className = "hint";
    const label = document.createElement("div");
    label.textContent = text;
    box.appendChild(label);
    if (buttons && buttons.length) {
      const row = document.createElement("div");
      row.className = "row";
      for (const spec of buttons) {
        const b = document.createElement("button");
        b.textContent = spec.label;
        if (spec.primary) b.className = "primary";
        b.addEventListener("click", () => vscode.postMessage({ type: spec.type }));
        row.appendChild(b);
      }
      box.appendChild(row);
    }
    host.appendChild(box);
  }

  function apply(next) {
    state = next;

    $("compiler-badge").textContent = next.compilerSource === "detected" ? "auto-detected"
      : next.compilerSource === "setting" ? "from settings" : "";
    const compilerInput = $("compiler-path");
    if (document.activeElement !== compilerInput) compilerInput.value = next.compilerPath || "";
    compilerInput.classList.toggle("bad", !next.compilerFound);
    $("compiler-dot").className = "dot " + (next.compilerFound ? "ok" : "bad");
    $("compiler-status").textContent = next.compilerFound
      ? "found" + (next.compilerVersion ? " · " + next.compilerVersion : "")
      : "not found";
    hint("compiler-hint", next.compilerFound ? "" : "vixc was not found. Pick the executable, let the panel look for it, or type its path above.",
      next.compilerFound ? [] : [
        { label: "Locate vixc…", type: "browseCompiler", primary: true },
        { label: "Auto-detect", type: "detectCompiler" }
      ]);

    $("server-badge").textContent = next.serverSource === "bundled" ? "bundled"
      : next.serverSource === "setting" ? "from settings" : "";
    const serverInput = $("server-path");
    if (document.activeElement !== serverInput) serverInput.value = next.serverPath || "";
    serverInput.classList.toggle("bad", !next.serverFound);
    $("server-dot").className = "dot " + (next.serverFound ? "ok" : "bad");
    $("server-status").textContent = next.serverFound ? "running from this path" : "not found";
    const libraryProblem = next.clientLibraryProblem || "";
    hint("server-hint", libraryProblem
      ? "This extension install is incomplete: " + libraryProblem + ". Reinstall the Vix extension."
      : ((!next.serverFound || next.serverSource === "setting")
        ? (next.serverFound
            ? "A custom server is configured. Use the bundled one to go back to the shipped build."
            : "The language server was not found. Point the field above at a vix-analyzer build.")
        : ""),
      libraryProblem
        ? []
        : (next.serverFound && next.serverSource === "setting"
          ? [{ label: "Use bundled server", type: "useBundledServer", primary: true }]
          : (!next.serverFound ? [{ label: "Locate vix-analyzer…", type: "browseServer", primary: true }, { label: "Use bundled", type: "useBundledServer" }] : [])));

    for (const b of document.querySelectorAll("#opt button")) {
      b.classList.toggle("active", Number(b.dataset.level) === next.optLevel);
    }
    const status = $("status");
    status.className = "status " + next.status;
    $("status-dot").className = "dot " + (next.status === "ok" ? "ok" : next.status === "error" ? "bad" : "");
    $("status-text").textContent = next.message || "Ready";
    $("status-meta").textContent = next.meta || "no build yet";
    for (const b of document.querySelectorAll("button.action[data-action]")) {
      b.disabled = next.status === "busy";
    }
  }

  window.addEventListener("message", (event) => {
    if (event.data && event.data.type === "state") apply(event.data.state);
  });

  function commit(inputId, type) {
    const value = $(inputId).value.trim();
    vscode.postMessage({ type: type, value: value });
  }
  $("compiler-path").addEventListener("change", () => commit("compiler-path", "setCompiler"));
  $("compiler-path").addEventListener("keydown", (e) => { if (e.key === "Enter") commit("compiler-path", "setCompiler"); });
  $("server-path").addEventListener("change", () => commit("server-path", "setServer"));
  $("server-path").addEventListener("keydown", (e) => { if (e.key === "Enter") commit("server-path", "setServer"); });

  $("compiler-browse").addEventListener("click", () => vscode.postMessage({ type: "browseCompiler" }));
  $("server-browse").addEventListener("click", () => vscode.postMessage({ type: "browseServer" }));
  $("restart").addEventListener("click", () => vscode.postMessage({ type: "restartServer" }));

  for (const b of document.querySelectorAll("button.action[data-action]")) {
    b.addEventListener("click", () => vscode.postMessage({ type: "action", action: b.dataset.action }));
  }
  for (const b of document.querySelectorAll("#opt button")) {
    b.addEventListener("click", () => vscode.postMessage({ type: "setOpt", level: Number(b.dataset.level) }));
  }
  $("output").addEventListener("click", () => vscode.postMessage({ type: "revealOutput" }));
  $("settings").addEventListener("click", () => vscode.postMessage({ type: "openSettings" }));

  vscode.postMessage({ type: "ready" });
</script>
</body>
</html>`;
}

class VixPanelProvider {
  constructor(context, state) {
    this.context = context;
    this.state = state;
  }

  resolveWebviewView(view) {
    this.view = view;
    view.webview.options = { enableScripts: true };
    view.webview.html = renderPanelHtml();
    view.webview.onDidReceiveMessage((message) => {
      if (this.onAction) this.onAction(message);
    });
  }

  push() {
    if (this.view) this.view.webview.postMessage({ type: "state", state: this.state });
  }
}

module.exports = { VixPanelProvider, renderPanelHtml };

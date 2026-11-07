#!/usr/bin/env node
// Verifies that a packaged extension can actually load.
//
// The 0.8.1 VSIX shipped without node_modules because it was packaged with
// --no-dependencies. The extension then threw "Cannot find module
// 'vscode-languageclient/node'" while the module was being loaded, so
// activate() never ran and the activity bar panel stayed blank. This check
// reproduces that load path against the extracted VSIX.
const path = require("path");
const Module = require("module");
const fs = require("fs");

const extensionDir = process.argv[2];
if (!extensionDir) {
  console.error("usage: vsix_load_test.js <extracted-extension-dir>");
  process.exit(2);
}

const vsCodeStub = {
  window: {},
  commands: {},
  languages: {},
  workspace: {},
  Uri: {},
  DiagnosticSeverity: {},
  Diagnostic: function () {},
  Range: function () {},
  Position: function () {}
};

const originalLoad = Module._load;
Module._load = function (request) {
  if (request === "vscode") {
    return vsCodeStub;
  }
  return originalLoad.apply(this, arguments);
};

const extensionEntry = path.join(extensionDir, "extension.js");
const failures = [];

try {
  const extensionRequire = Module.createRequire(extensionEntry);
  extensionRequire.resolve("vscode-languageclient/node");
} catch (error) {
  failures.push("vscode-languageclient/node does not resolve from the package: " + error.message);
}

try {
  require(extensionEntry);
} catch (error) {
  failures.push("extension entry failed to load: " + error.message);
}

for (const required of ["server/vix-analyzer", "panel.js", "syntaxes/vix.tmLanguage.json"]) {
  if (!fs.existsSync(path.join(extensionDir, required))) {
    failures.push("missing from the package: " + required);
  }
}

if (failures.length > 0) {
  for (const failure of failures) {
    console.error("FAIL " + failure);
  }
  process.exit(1);
}
console.log("vsix: extension loads and its language client resolves");

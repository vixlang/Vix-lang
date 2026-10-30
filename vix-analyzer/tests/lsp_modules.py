#!/usr/bin/env python3
"""Module-system support: a document whose use declarations resolve against
real files on disk analyzes as the root of a merged program.

Covers the four behaviors the module wiring promises:
  - qualified references into used modules produce no false undefined-symbol
    diagnostics on the root document;
  - the root document's own errors are still reported at their real positions;
  - errors inside a used module are NOT reported on the root document;
  - hover and definition on a qualified reference resolve into the module file.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./build/vix-analyzer"

failures = []


def check(name, condition, detail=""):
    if condition:
        print(f"ok   {name}")
    else:
        print(f"FAIL {name} {detail}")
        failures.append(name)


def frame(message):
    body = json.dumps(message).encode()
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


def read_message(stream):
    header = b""
    while b"\r\n\r\n" not in header:
        byte = stream.read(1)
        if not byte:
            return None
        header += byte
    length = 0
    for line in header.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    return json.loads(stream.read(length).decode())


class Server:
    def __init__(self):
        self.proc = subprocess.Popen(
            [SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.request_id = 0

    def send(self, method, params=None, notify=True):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if not notify:
            self.request_id += 1
            message["id"] = self.request_id
        self.proc.stdin.write(frame(message))
        self.proc.stdin.flush()
        return self.request_id if not notify else None

    def request(self, method, params):
        request_id = self.send(method, params, notify=False)
        while True:
            message = read_message(self.proc.stdout)
            if message is None:
                return None
            if message.get("id") == request_id:
                return message.get("result")
            # notifications between request and response are dropped; the
            # server only interleaves publishDiagnostics here

    def open(self, uri, text, version=1):
        self.send("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "vix", "version": version, "text": text}})

    def change(self, uri, text, version):
        self.send("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": text}]})

    def diagnostics(self):
        # didOpen/didChange reply with publishDiagnostics before the next
        # request's response; drain until a response arrives and keep the last
        # diagnostic set seen.
        return self._last_diagnostics

    def wait_diagnostics(self, method, params):
        """Send a request and collect the diagnostics published before its reply."""
        request_id = self.send(method, params, notify=False)
        seen = None
        while True:
            message = read_message(self.proc.stdout)
            if message is None:
                return seen
            if message.get("method") == "textDocument/publishDiagnostics":
                seen = message["params"]["diagnostics"]
            if message.get("id") == request_id:
                return seen


def position_of(text, offset):
    line = text.count("\n", 0, offset)
    line_start = text.rfind("\n", 0, offset) + 1
    return {"line": line, "character": offset - line_start}


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        module_text = (
            "pub type Point = struct {\n"
            "    x: i32,\n"
            "    y: i32\n"
            "}\n"
            "pub fn add(a: i32, b: i32): i32\n"
            "{\n"
            "    return a + b\n"
            "}\n"
        )
        (root / "math.vix").write_text(module_text)
        main_text = (
            'use "math.vix" as math\n'
            'extern "C" { fn puts(s: string): i32 }\n'
            "fn main(): i32\n"
            "{\n"
            "    let p = math::Point{ x: 1, y: 2 }\n"
            "    let sum = math::add(p.x, p.y)\n"
            "    puts(\"ok\")\n"
            "    return sum\n"
            "}\n"
        )
        (root / "main.vix").write_text(main_text)

        server = Server()
        server.send("initialize", {"capabilities": {}})
        main_uri = (root / "main.vix").as_uri()
        server.open(main_uri, main_text)
        diags = server.wait_diagnostics(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": main_uri}}) or []

        codes = [d.get("code") for d in diags]
        check("no undefined-symbol noise for module references", "E2003" not in codes, codes)
        check("no module-not-found errors for on-disk modules", "E1000" not in codes, codes)

        # definition of math::add jumps into math.vix at the right line
        add_offset = main_text.index("math::add") + len("math::")
        result = server.request("textDocument/definition", {
            "textDocument": {"uri": main_uri}, "position": position_of(main_text, add_offset)})
        target = (result or {}).get("uri", "")
        line = ((result or {}).get("range", {}).get("start", {}) or {}).get("line", -1)
        check("definition resolves to the module file", target.endswith("math.vix"), result)
        check("definition points at the module's add (0-based line 4)", line == 4, line)

        # hover over math::add shows the module-qualified signature
        hover = server.request("textDocument/hover", {
            "textDocument": {"uri": main_uri}, "position": position_of(main_text, add_offset)})
        contents = (((hover or {}).get("contents", {}) or {}).get("value", ""))
        check("hover shows the qualified signature",
              "math::add" in contents and "fn" in contents, contents)

        # the module's own diagnostics never land on the root document
        (root / "math.vix").write_text(
            "pub fn broken(): i32\n{\n    return missing\n}\n")
        server.change(main_uri, main_text, 2)
        diags = server.wait_diagnostics(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": main_uri}}) or []
        messages = " ".join(d.get("message", "") for d in diags)
        check("module-internal errors are not reported on the root",
              "broken" not in messages and "missing" not in messages, messages)

        # a root-file error of its own is still reported, at its real position
        root_error_text = main_text.replace('    puts("ok")\n', '    puts(missing)\n')
        server.change(main_uri, root_error_text, 3)
        diags = server.wait_diagnostics(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": main_uri}}) or []
        positions = [(d["range"]["start"]["line"], d["range"]["start"]["character"])
                     for d in diags if "missing" in d.get("message", "")]
        expected = position_of(root_error_text, root_error_text.index("missing"))
        check("root-file errors keep their position",
              (expected["line"], expected["character"]) in positions, positions)

        # a missing module file fails loudly on the root document
        missing_text = 'use "nowhere.vix" as gone\nfn main(): i32\n{\n    return 0\n}\n'
        (root / "lonely.vix").write_text(missing_text)
        lonely_uri = (root / "lonely.vix").as_uri()
        server.open(lonely_uri, missing_text)
        diags = server.wait_diagnostics(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": lonely_uri}}) or []
        codes = [d.get("code") for d in diags]
        check("missing module file is reported", "E1000" in codes, codes)

        server.send("shutdown", {})
        server.proc.wait(timeout=10)

    if failures:
        print(f"module tests: {len(failures)} failure(s)")
        return 1
    print("module tests: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

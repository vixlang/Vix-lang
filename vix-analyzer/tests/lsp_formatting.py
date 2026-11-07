#!/usr/bin/env python3
"""Format Document must actually work in an editor.

The language server implemented textDocument/formatting and had tests that sent
the request by hand, but the initialize result never advertised
documentFormattingProvider. A real editor therefore never sent the request and
the Format Document button did nothing.

This test drives a real LSP session and checks the capability, the edit, the
editor's indentation options and the vix-fmt.toml file.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

MESSY = """type    Point = struct {
        x: i32,
      y: i32
}
fn (&Point)   length( ):   i32 {
    return self.x+self.y
}
fn main( ): i32 {
    let p=Point{ x: 3, y: 4 }
   print( p.length( ) )
  return 0
}
"""

LONG = """fn compute(first_argument: i32, second_argument: i32, third_argument: i32): i32 {
    return first_argument + second_argument + third_argument
}
"""


def frame(message):
    body = json.dumps(message).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


def read_message(stream):
    header = b""
    while b"\r\n\r\n" not in header:
        byte = stream.read(1)
        if not byte:
            return None
        header += byte
    length = 0
    for line in header.decode("utf-8").split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    body = stream.read(length)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))


class Session:
    def __init__(self, server):
        self.process = subprocess.Popen(
            [server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )

    def send(self, message):
        self.process.stdin.write(frame(message))
        self.process.stdin.flush()

    def initialize(self):
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        while True:
            message = read_message(self.process.stdout)
            if message is None:
                raise AssertionError("server closed before answering initialize")
            if message.get("id") == 1:
                return message.get("result", {})

    def format(self, uri, text, options):
        self.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "vix", "version": 1, "text": text}}})
        self.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {
            "textDocument": {"uri": uri}, "options": options}})
        while True:
            message = read_message(self.process.stdout)
            if message is None:
                raise AssertionError("server closed before answering formatting")
            if message.get("id") == 2:
                return message.get("result")

    def close(self):
        try:
            self.send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
        except Exception:
            pass
        try:
            self.process.stdin.close()
        except Exception:
            pass
        self.process.terminate()
        self.process.wait(timeout=10)


def main() -> int:
    server = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "vix-analyzer")
    failures = []
    with tempfile.TemporaryDirectory(prefix="vix-fmt-lsp-") as tmp_name:
        tmp = Path(tmp_name)
        (tmp / "messy.vix").write_text(MESSY)
        (tmp / "long.vix").write_text(LONG)
        (tmp / "vix-fmt.toml").write_text("max_width = 46\n")

        session = Session(server)
        result = session.initialize()
        capabilities = result.get("capabilities", {})
        if capabilities.get("documentFormattingProvider") is not True:
            failures.append("initialize did not advertise documentFormattingProvider")

        edits = session.format("file://" + str(tmp / "messy.vix"), MESSY, {"tabSize": 4, "insertSpaces": True})
        if not edits:
            failures.append("formatting a messy document returned no edits")
        else:
            formatted = edits[0]["newText"]
            if "type Point = struct" not in formatted:
                failures.append("spacing around the type declaration was not normalised")
            if "length(): i32" not in formatted:
                failures.append("spacing inside the parameter list was not normalised")
            if "return self.x + self.y" not in formatted:
                failures.append("binary operator spacing was not normalised")
            if "    x: i32," not in formatted:
                failures.append("indentation was not applied")
            second = session.format("file://" + str(tmp / "messy.vix"), formatted, {"tabSize": 4, "insertSpaces": True})
            if second:
                failures.append("formatting a formatted document still produced edits (not idempotent)")

        # The editor's tabSize must win over the default.
        edits = session.format("file://" + str(tmp / "messy.vix"), MESSY, {"tabSize": 2, "insertSpaces": True})
        if not edits:
            failures.append("formatting with tabSize 2 returned no edits")
        elif "\n  x: i32," not in edits[0]["newText"]:
            failures.append("tabSize 2 was ignored")

        # vix-fmt.toml next to the file must be honoured: max_width 46 has to wrap.
        edits = session.format("file://" + str(tmp / "long.vix"), LONG, {"tabSize": 4, "insertSpaces": True})
        if not edits:
            failures.append("formatting with a vix-fmt.toml returned no edits")
        else:
            formatted = edits[0]["newText"]
            if "fn compute(\n" not in formatted:
                failures.append("max_width from vix-fmt.toml did not wrap the signature")

        session.close()

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("lsp: documentFormattingProvider advertised, edits applied, options and vix-fmt.toml honoured")
    return 0


if __name__ == "__main__":
    sys.exit(main())

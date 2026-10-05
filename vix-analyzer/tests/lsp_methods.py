#!/usr/bin/env python3
"""Method aware language server checks.

The analyzer parses and type checks without going through the main compiler
pipeline. When its programs carried no method table the semantic pass reported
every method call as "unknown method", even though the declaration was indexed
and hover could describe it. This test drives a real LSP session over
examples/impl2.vix and asserts the diagnostic is gone.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = ROOT / "examples" / "impl2.vix"
URI = "file://" + str(SOURCE_PATH)


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


def main() -> int:
    server = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "build" / "vix-analyzer")
    source = SOURCE_PATH.read_text()

    process = subprocess.Popen(
        [server],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    def send(message):
        process.stdin.write(frame(message))
        process.stdin.flush()

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": URI, "languageId": "vix", "version": 1, "text": source}}})
    # line 27 column 7 is the "move" selector in "p.move(1, 1)".
    send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": URI}, "position": {"line": 26, "character": 7}}})
    send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": URI}, "position": {"line": 9, "character": 15}}})
    send({"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}})

    diagnostics = []
    definition = None
    hover = None
    while True:
        message = read_message(process.stdout)
        if message is None:
            break
        method = message.get("method")
        if method == "textDocument/publishDiagnostics":
            params = message.get("params", {})
            if params.get("uri") == URI:
                diagnostics.extend(params.get("diagnostics", []))
        elif message.get("id") == 2:
            definition = message.get("result")
        elif message.get("id") == 3:
            hover = message.get("result")
            break
    process.stdin.close()
    process.terminate()
    process.wait(timeout=10)

    failures = []
    for diagnostic in diagnostics:
        text = diagnostic.get("message", "")
        if "unknown method" in text:
            failures.append("false positive diagnostic: " + text)

    if definition:
        start = definition.get("range", {}).get("start", {})
        # fn (&mut Point) move(...) is on line 14, i.e. index 13.
        if start.get("line") != 13:
            failures.append("definition for move pointed at line " + str(start.get("line")) + ", expected 13")
    else:
        failures.append("no definition returned for p.move")

    if hover is None:
        failures.append("no hover returned for the length declaration")
    else:
        rendered = json.dumps(hover)
        if "&Point" not in rendered:
            failures.append("hover did not show the receiver: " + rendered[:200])

    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("lsp: methods resolve with no false positives (" + str(len(diagnostics)) + " diagnostics)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

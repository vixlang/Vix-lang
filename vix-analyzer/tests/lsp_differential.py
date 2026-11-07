#!/usr/bin/env python3
"""Compare full parsing with the experimental declaration-fragment path.
Edits are legal whitespace-only edits inside a function body; each subsequent
range is computed against the text after the preceding edit.
"""
import glob
import io
import json
import subprocess
import sys

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./build/vix-analyzer"
LIMIT = int(sys.argv[2]) if len(sys.argv) > 2 else 40


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
    body = stream.read(length)
    return json.loads(body.decode()) if body else None


def position_of(text, offset):
    line = text.count("\n", 0, offset)
    line_start = text.rfind("\n", 0, offset) + 1
    return {"line": line, "character": offset - line_start}


def lsp_range(text, start, end):
    return {"start": position_of(text, start), "end": position_of(text, end)}


def find_function_body_edit(text):
    # Select an indentation boundary inside a function, not an identifier or
    # delimiter, so the edit changes trivia only and keeps the source parseable.
    lines = text.splitlines(keepends=True)
    offset = 0
    in_function = False
    depth = 0
    candidates = []
    for line in lines:
        stripped = line.lstrip(" \t")
        if stripped.startswith("fn ") or stripped.startswith("pub fn "):
            in_function = True
        if in_function and stripped and not stripped.startswith("//"):
            indent = len(line) - len(stripped)
            if depth > 0 and indent > 0:
                candidates.append(offset + indent)
        # Brace scan is sufficient here because we only use this to choose a
        # whitespace position, not to parse or format source.
        for char in line:
            if char == "{": depth += 1
            elif char == "}" and depth > 0: depth -= 1
        offset += len(line)
        if in_function and depth == 0 and "}" in line:
            in_function = False
    if not candidates:
        return None
    return candidates[len(candidates) // 2]


def build_messages(mode, uri, text, edits):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "vix/setAnalysisMode", "params": {"mode": mode}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "vix", "version": 1, "text": text}}},
    ]
    current = text
    version = 1
    checkpoints = []
    for edit in edits:
        start, end, insert = edit
        version += 1
        messages.append({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"range": lsp_range(current, start, end), "text": insert}]}})
        current = current[:start] + insert + current[end:]
        request_id = 100 + version
        messages.append({"jsonrpc": "2.0", "id": request_id, "method": "textDocument/documentSymbol",
                         "params": {"textDocument": {"uri": uri}}})
        checkpoints.append(request_id)
    messages.append({"jsonrpc": "2.0", "id": 999, "method": "shutdown", "params": {}})
    return messages, checkpoints


def run_mode(mode, text, edits, uri):
    messages, checkpoints = build_messages(mode, uri, text, edits)
    proc = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = proc.communicate(b"".join(frame(m) for m in messages), timeout=120)
    stream = io.BytesIO(out)
    diagnostics = []
    symbols = {}
    stats = []
    while True:
        message = read_message(stream)
        if message is None:
            break
        if message.get("method") == "textDocument/publishDiagnostics" and message["params"]["uri"] == uri:
            diagnostics = sorted((d.get("code", ""), d["severity"], d["range"]["start"]["line"],
                                  d["range"]["start"]["character"], d["message"])
                                 for d in message["params"]["diagnostics"])
        elif message.get("method") == "vix/analysisStats":
            stats.append(message["params"])
        if message.get("id") in checkpoints:
            symbols[message["id"]] = sorted((s["name"], s["kind"], s["range"]["start"]["line"],
                                               s["range"]["end"]["line"])
                                              for s in (message.get("result") or []))
    return diagnostics, symbols, stats, proc.returncode, err


def main():
    files = sorted(glob.glob("tests/*.vix") + glob.glob("examples/*.vix"))[:LIMIT]
    mismatches = 0
    compared = 0
    for path in files:
        try:
            text = open(path, encoding="utf-8").read()
        except Exception:
            continue
        if len(text) < 40 or len(text) > 20000:
            continue
        position = find_function_body_edit(text)
        if position is None:
            continue
        # Same-length trivia edit, then undo it. Both intermediate and final
        # buffers remain parseable; range is recomputed from current text.
        edits = [(position, position, " "), (position, position + 1, "")]
        uri = "file:///tmp/diff-" + str(compared) + ".vix"
        try:
            legacy = run_mode("legacy", text, edits, uri)
            fragments = run_mode("fragments", text, edits, uri)
        except subprocess.TimeoutExpired:
            print("TIMEOUT", path)
            mismatches += 1
            continue
        compared += 1
        if legacy[:2] != fragments[:2] or fragments[3] not in (0, None):
            mismatches += 1
            print("MISMATCH", path, "exit", fragments[3])
            if legacy[0] != fragments[0]: print(" diagnostics:", legacy[0][:2], "vs", fragments[0][:2])
            if legacy[1] != fragments[1]: print(" symbols:", legacy[1], "vs", fragments[1])
            if fragments[3] not in (0, None): print(" stderr:", fragments[4].decode("utf-8", "replace")[-500:])
    print("compared=%d mismatches=%d" % (compared, mismatches))
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())

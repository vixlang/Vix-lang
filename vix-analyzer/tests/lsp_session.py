#!/usr/bin/env python3
"""Minimal LSP client used to smoke-test vix-analyzer over stdio."""
import json
import subprocess
import sys

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./build/vix-analyzer"
URI = "file:///tmp/demo.vix"
BROKEN_URI = "file:///tmp/broken.vix"

SOURCE = """fn add(a: i32, b: i32): i32
{
    return a + b
}

fn main(): i32
{
    let value = add(1, 2)
    return value
}
"""

BROKEN = """fn broken(: i32
{
    return 1
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


def main():
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": URI, "languageId": "vix", "version": 1, "text": SOURCE}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol", "params": {
            "textDocument": {"uri": URI}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {
            "textDocument": {"uri": URI}, "position": {"line": 7, "character": 17}}},
        {"jsonrpc": "2.0", "id": 4, "method": "textDocument/definition", "params": {
            "textDocument": {"uri": URI}, "position": {"line": 7, "character": 17}}},
        {"jsonrpc": "2.0", "id": 5, "method": "textDocument/references", "params": {
            "textDocument": {"uri": URI}, "position": {"line": 0, "character": 4}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": BROKEN_URI, "languageId": "vix", "version": 1, "text": BROKEN}}},
        {"jsonrpc": "2.0", "id": 6, "method": "textDocument/rename", "params": {
            "textDocument": {"uri": URI}, "position": {"line": 7, "character": 17}, "newName": "sum"}},
        {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": URI, "version": 2},
            "contentChanges": [{"range": {"start": {"line": 8, "character": 11},
                                          "end": {"line": 8, "character": 16}},
                                "text": "missing"}]}},
        {"jsonrpc": "2.0", "id": 7, "method": "textDocument/documentSymbol", "params": {
            "textDocument": {"uri": URI}}},
        {"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": {}},
    ]
    payload = b"".join(frame(m) for m in messages)
    proc = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = proc.communicate(payload, timeout=60)
    stream = __import__("io").BytesIO(out)
    count = 0
    while True:
        message = read_message(stream)
        if message is None:
            break
        count += 1
        print(json.dumps(message, ensure_ascii=False))
    print("--- messages:", count)
    if err:
        print("--- stderr:", err.decode("utf-8", "replace")[:800])


if __name__ == "__main__":
    main()
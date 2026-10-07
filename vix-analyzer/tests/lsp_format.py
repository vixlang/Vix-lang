#!/usr/bin/env python3
"""Checks that textDocument/formatting reindents without losing content."""
import io
import json
import subprocess
import sys

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./build/vix-analyzer"
URI = "file:///tmp/format.vix"

SOURCE = """#[no_main]

use "std/io.vix" as io
fn add(a: i32, b: i32): i32
{
return a + b
}


fn main(): i32
{
        let value = add(1, 2)
  // keep this comment
  if (value > 0)
     {
    print("positive")
        }
    return value
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
    for line in header.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    body = stream.read(length)
    return json.loads(body.decode("utf-8")) if body else None


def main():
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": URI, "languageId": "vix", "version": 1, "text": SOURCE}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {
            "textDocument": {"uri": URI}, "options": {"tabSize": 4, "insertSpaces": True}}},
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
    ]
    proc = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, _ = proc.communicate(b"".join(frame(m) for m in messages), timeout=60)
    stream = io.BytesIO(out)
    formatted = None
    while True:
        message = read_message(stream)
        if message is None:
            break
        if message.get("id") == 2:
            edits = message.get("result") or []
            if edits:
                formatted = edits[0]["newText"]
    if formatted is None:
        print("FAIL: no formatting edits returned")
        return 1
    print("=== 格式化结果 ===")
    for index, line in enumerate(formatted.split("\n")):
        print(f"{index:>2}| {line}")
    problems = []
    if "// keep this comment" not in formatted:
        problems.append("注释被丢失")
    if "print(\"positive\")" not in formatted:
        problems.append("字符串内容被破坏")
    if "\n\n\n" in formatted:
        problems.append("多余空行未折叠")
    for line in formatted.split("\n"):
        if line and (len(line) - len(line.lstrip(" "))) % 4 != 0:
            problems.append("缩进不是 4 的倍数: " + repr(line))
            break
    if problems:
        print("FAIL:", "; ".join(problems))
        return 1
    print("PASS: 缩进/空行/注释/字符串全部正确")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

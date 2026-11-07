#!/usr/bin/env python3
"""Formatter safety: refuses broken files, is idempotent."""
import io, json, subprocess, sys
SERVER = sys.argv[1]

def frame(m):
    b = json.dumps(m).encode()
    return b"Content-Length: " + str(len(b)).encode() + b"\r\n\r\n" + b

def read(stream):
    h = b""
    while b"\r\n\r\n" not in h:
        c = stream.read(1)
        if not c: return None
        h += c
    n = 0
    for line in h.decode().split("\r\n"):
        if line.lower().startswith("content-length:"):
            n = int(line.split(":",1)[1].strip())
    body = stream.read(n)
    return json.loads(body.decode()) if body else None

def fmt(text):
    uri = "file:///tmp/f.vix"
    msgs = [
        {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}},
        {"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":uri,"languageId":"vix","version":1,"text":text}}},
        {"jsonrpc":"2.0","id":2,"method":"textDocument/formatting","params":{"textDocument":{"uri":uri},"options":{}}},
        {"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}},
    ]
    p = subprocess.Popen([SERVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, _ = p.communicate(b"".join(frame(m) for m in msgs), timeout=60)
    s = io.BytesIO(out); result = None
    while True:
        m = read(s)
        if m is None: break
        if m.get("id") == 2: result = m.get("result")
    return result or []

broken = "fn main(): i32\n{\n  let x = \n}\n"
edits = fmt(broken)
print("语法错误文件 -> 编辑数:", len(edits), "(期望 0)")
ok1 = len(edits) == 0

messy = "fn main(): i32\n{\nlet a = 1\nif (a)\n{\nreturn a\n}\n}\n"
first = fmt(messy)
once = first[0]["newText"] if first else messy
second = fmt(once)
twice = second[0]["newText"] if second else once
print("第一次格式化编辑数:", len(first))
print("第二次格式化编辑数:", len(second), "(期望 0，说明已稳定)")
ok2 = once == twice and len(second) == 0
print()
print("语法错误拒绝格式化:", "PASS" if ok1 else "FAIL")
print("幂等:", "PASS" if ok2 else "FAIL")
sys.exit(0 if (ok1 and ok2) else 1)

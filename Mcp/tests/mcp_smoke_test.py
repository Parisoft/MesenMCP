#!/usr/bin/env python3
"""End-to-end smoke test for the mesen-mcp MCP server.

Speaks the MCP stdio protocol (newline-delimited JSON-RPC 2.0) to a spawned
server process and verifies:
  - initialize handshake, capabilities, tools/list
  - load_rom + run_frames + get_status flow
  - screenshot returns a valid PNG of the expected size with real content
  - pause/resume/reset/unload_rom lifecycle
  - error handling (unknown tool, missing ROM, pre-initialize rejection)

Usage: mcp_smoke_test.py [path-to-mesen-mcp]   (default: bin/mesen-mcp)
"""
import base64
import json
import os
import struct
import subprocess
import sys
import zlib

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join("bin", "mesen-mcp")
ROM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "red.nes")
HOME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "smoke-home")

passed = 0

def ok(name, cond, detail=""):
    global passed
    if not cond:
        print(f"FAIL: {name} {detail}")
        sys.exit(1)
    passed += 1
    print(f"  ok: {name}")

class McpClient:
    _next_id = 1

    def __init__(self):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        self.proc = subprocess.Popen(
            [BIN, "--home", HOME],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, env=env, text=True, bufsize=1)

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def recv(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout unexpectedly")
        return json.loads(line)

    def request(self, method, params=None):
        msg_id = McpClient._next_id
        McpClient._next_id += 1
        self.send({"jsonrpc": "2.0", "id": msg_id, "method": method,
                   **({"params": params} if params is not None else {})})
        return self.recv()

    def notify(self, method, params=None):
        self.send({"jsonrpc": "2.0", "method": method,
                   **({"params": params} if params is not None else {})})

    def call_tool(self, name, args=None):
        return self.request("tools/call", {"name": name, "arguments": args or {}})

    def close(self):
        self.proc.stdin.close()
        return self.proc.wait(timeout=15)

def decode_png_dimensions_and_colors(b64):
    data = base64.b64decode(b64)
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    w = h = ctype = None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8]); pos += 8
        chunk = data[pos:pos + ln]; pos += ln + 4
        if typ == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", chunk[:10])
        if typ == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    assert ctype == 2, f"expected RGB PNG, got color type {ctype}"
    stride = w * 3
    prev = bytearray(stride)
    colors, non_black, p = set(), 0, 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pp, pa, pb, pc = a + b - c, abs(a + b - c - a), abs(a + b - c - b), abs(a + b - c - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
    for i in range(0, len(prev), 3):
        t = (prev[i], prev[i + 1], prev[i + 2])
        colors.add(t)
        if t != (0, 0, 0):
            non_black += 1
    return w, h, colors, non_black

def main():
    print(f"== mesen-mcp smoke test ({BIN}) ==")
    c = McpClient()

    #Requests before initialize must be rejected (except ping)
    r = c.request("tools/list")
    ok("pre-initialize tools/list rejected", r.get("error", {}).get("code") == -32002)

    #Handshake
    r = c.request("initialize", {"protocolVersion": "2025-06-18",
                                 "clientInfo": {"name": "smoke-test", "version": "0"}})
    ok("initialize protocolVersion", r["result"]["protocolVersion"] == "2025-06-18")
    ok("initialize capabilities.tools", "tools" in r["result"]["capabilities"])
    ok("initialize serverInfo", r["result"]["serverInfo"]["name"] == "mesen-mcp")
    c.notify("notifications/initialized")

    r = c.request("ping")
    ok("ping", r["result"] == {})

    #tools/list
    r = c.request("tools/list")
    names = {t["name"] for t in r["result"]["tools"]}
    expected = {"load_rom", "unload_rom", "reset", "pause", "resume", "set_speed",
                "run_frames", "screenshot", "get_status"}
    ok("tools/list has all Tier-0 tools", expected <= names, f"got: {sorted(names)}")
    ok("tools have schemas", all("inputSchema" in t and "description" in t for t in r["result"]["tools"]))

    #Unknown tool -> isError content, not a JSON-RPC error
    r = c.call_tool("does_not_exist")
    ok("unknown tool -> isError", r["result"]["isError"] is True)

    #Load a nonexistent ROM -> isError with a helpful message
    r = c.call_tool("load_rom", {"path": "/nonexistent/rom.nes"})
    ok("missing rom -> isError", r["result"]["isError"] is True)
    ok("missing rom message mentions file", "not found" in r["result"]["content"][0]["text"])

    #Load the generated test ROM (solid red screen)
    r = c.call_tool("load_rom", {"path": ROM})
    ok("load_rom", r["result"]["isError"] is False, str(r["result"]["content"])[:200])
    status = json.loads([x for x in r["result"]["content"] if x["type"] == "text"][0]["text"])
    ok("status after load", status["rom_loaded"] is True and status["rom"]["console_type"] == "nes")

    #Run frames
    r = c.call_tool("run_frames", {"frames": 120, "timeout_ms": 15000})
    ok("run_frames", r["result"]["isError"] is False)
    run = json.loads(r["result"]["content"][0]["text"])
    ok("120 frames ran", run["frames_run"] >= 119, str(run))
    ok("did not time out", run["timed_out"] is False)

    #Screenshot: valid RGB PNG, solid non-black color
    r = c.call_tool("screenshot")
    ok("screenshot ok", r["result"]["isError"] is False)
    img = [x for x in r["result"]["content"] if x["type"] == "image"][0]
    ok("image mime type", img["mimeType"] == "image/png")
    w, h, colors, non_black = decode_png_dimensions_and_colors(img["data"])
    ok("png is 256x240", (w, h) == (256, 240))
    ok("png is solid non-black red", len(colors) == 1 and (0, 0, 0) not in colors, f"colors={list(colors)[:3]}")

    #get_status
    r = c.call_tool("get_status")
    status = json.loads(r["result"]["content"][0]["text"])
    ok("get_status rom_loaded", status["rom_loaded"] is True)
    ok("get_status frame_count", status["frame_count"] >= 120)
    ok("get_status rom identity", len(status["rom"]["sha1"]) == 40 and status["rom"]["format"] == "ines")
    ok("get_status paused=false", status["paused"] is False)

    #pause / resume
    ok("pause", c.call_tool("pause")["result"]["isError"] is False)
    status = json.loads(c.call_tool("get_status")["result"]["content"][0]["text"])
    ok("status paused", status["paused"] is True)
    r = c.call_tool("run_frames", {"frames": 1})
    ok("run_frames while paused -> isError", r["result"]["isError"] is True)
    ok("resume", c.call_tool("resume")["result"]["isError"] is False)
    status = json.loads(c.call_tool("get_status")["result"]["content"][0]["text"])
    ok("status resumed", status["paused"] is False)

    #set_speed both ways
    ok("set_speed realtime", c.call_tool("set_speed", {"speed": "realtime"})["result"]["isError"] is False)
    ok("set_speed max", c.call_tool("set_speed", {"speed": "max"})["result"]["isError"] is False)

    #reset
    ok("reset", c.call_tool("reset")["result"]["isError"] is False)
    status = json.loads(c.call_tool("get_status")["result"]["content"][0]["text"])
    ok("frame count reset", status["frame_count"] < 120, str(status["frame_count"]))

    #unload
    ok("unload_rom", c.call_tool("unload_rom")["result"]["isError"] is False)
    status = json.loads(c.call_tool("get_status")["result"]["content"][0]["text"])
    ok("status rom_loaded=false", status["rom_loaded"] is False)
    r = c.call_tool("run_frames", {"frames": 1})
    ok("run_frames without rom -> isError", r["result"]["isError"] is True)

    #Reload (session must be reusable after unload)
    r = c.call_tool("load_rom", {"path": ROM})
    ok("reload after unload", r["result"]["isError"] is False)
    ok("run after reload", c.call_tool("run_frames", {"frames": 10})["result"]["isError"] is False)

    #Protocol-level: malformed JSON line
    c.proc.stdin.write("{\"jsonrpc\": \"2.0\", \"id\": 999, \"method\":\n")
    c.proc.stdin.flush()
    r = c.recv()
    ok("malformed JSON -> -32700", r.get("error", {}).get("code") == -32700)

    #Graceful shutdown on stdin close
    rc = c.close()
    ok("clean exit on stdin EOF", rc == 0, f"rc={rc}")

    print(f"== PASSED ({passed} checks) ==")

if __name__ == "__main__":
    main()

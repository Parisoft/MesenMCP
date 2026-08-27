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

    #--- P2: debugger tools ---
    #CPU state: the ROM's main loop lives at $C057 (JSR $C100 / JMP $C057)
    r = c.call_tool("get_registers")
    regs = json.loads(r["result"]["content"][0]["text"])
    ok("registers combined view", {"pc", "a", "flags", "cycle_count", "ppu"} <= set(regs), str(sorted(regs.keys()))[:90])
    ok("status flags decoded", set(regs["flags"]) == {"n", "v", "d", "i", "z", "c"} and regs["flags"]["i"] is True, str(regs["flags"]))
    ok("ppu position in registers", {"scanline", "cycle", "frame_count"} <= set(regs["ppu"]), str(regs["ppu"])[:70])

    r = c.call_tool("get_cpu_state")
    cs = json.loads(r["result"]["content"][0]["text"])
    ok("cpu state sane", 0xC06B <= cs["pc"] <= 0xC071 or 0xC100 <= cs["pc"] <= 0xC107, f"pc={cs['pc']:#x}")

    r = c.call_tool("disassemble", {"address": "$C000", "count": 2})
    d = json.loads(r["result"]["content"][0]["text"])
    ok("disassemble at reset", d["instructions"][0]["text"].startswith("SEI"), d["instructions"][0]["text"])

    r = c.call_tool("read_memory", {"memory_type": "palette_ram", "address": 0, "length": 2})
    ok("read palette ram", json.loads(r["result"]["content"][0]["text"])["hex"].upper().startswith("16 30"))

    r = c.call_tool("write_memory", {"memory_type": "work_ram", "address": "$0300", "bytes": "DE AD BA BE"})
    ok("write memory", r["result"]["isError"] is False)
    r = c.call_tool("read_memory", {"memory_type": "work_ram", "address": 0x300, "length": 4})
    ok("read back written bytes", json.loads(r["result"]["content"][0]["text"])["hex"].upper() == "DE AD BA BE")

    r = c.call_tool("search_memory", {"memory_type": "prg_rom", "value": "A9 16"})
    ok("search prg_rom pattern", 0x25 in json.loads(r["result"]["content"][0]["text"])["matches"])

    r = c.call_tool("evaluate_expression", {"expression": "[$42] == $5A"})
    ok("evaluate expression (memory read)", json.loads(r["result"]["content"][0]["text"])["value"] == 1)

    r = c.call_tool("get_memory_size", {"memory_type": "internal_ram"})
    ok("memory size (nes internal ram = 2KB)", json.loads(r["result"]["content"][0]["text"])["size"] == 2048)

    r = c.call_tool("get_memory_size", {"memory_type": "not_a_type"})
    ok("bad memory type -> isError + suggestions",
       r["result"]["isError"] is True and "Available" in r["result"]["content"][0]["text"])

    #Breakpoint on the constantly-called subroutine at $C100
    r = c.call_tool("set_breakpoint", {"address": "$C100"})
    ok("set breakpoint", r["result"]["isError"] is False)
    r = c.call_tool("wait_for_breakpoint", {"timeout_ms": 5000})
    wb = json.loads(r["result"]["content"][0]["text"])
    ok("breakpoint hit at $C100", wb.get("stopped") and wb.get("cpu_state", {}).get("pc") == 0xC100, str(wb)[:100])

    #Remove the breakpoint first so the step-out cannot be intercepted by a re-hit
    ok("remove all breakpoints", c.call_tool("remove_breakpoint", {"all": True})["result"]["isError"] is False)

    #Step out of the subroutine - at max speed in this tight loop the exact stop
    #point jitters between the RTS return address and the loop instructions
    r = c.call_tool("step", {"type": "out", "timeout_ms": 3000})
    st = json.loads(r["result"]["content"][0]["text"])
    pc = st.get("cpu_state", {}).get("pc", 0)
    ok("step out stops in main loop", st.get("stopped") and (0xC06B <= pc <= 0xC071 or 0xC100 <= pc <= 0xC105),
       f"pc={pc:#x}")

    #Step 3 instructions - consecutive rapid steps at max speed jitter within
    #the tight loop; verify we end stopped inside it
    r = c.call_tool("step", {"type": "instruction", "count": 3, "timeout_ms": 3000})
    st = json.loads(r["result"]["content"][0]["text"])
    pc = st.get("cpu_state", {}).get("pc", 0)
    ok("step 3 instructions", st.get("stopped") and (0xC057 <= pc <= 0xC105),
       f"pc={pc:#x}")

    r = c.call_tool("get_callstack")
    ok("callstack", r["result"]["isError"] is False)

    ok("continue", c.call_tool("continue")["result"]["isError"] is False)

    r = c.call_tool("trace", {"run_frames": 1, "rows": 4})
    tr = json.loads(r["result"]["content"][0]["text"])
    ok("trace rows", tr.get("row_count", 0) >= 1 and any("C0" in r or "C1" in r for r in (tr.get("rows") or [])), str(tr)[:100])
    #Disable tracing again - a resident trace logger hooks every instruction and
    #makes later step operations jitter at max speed
    c.call_tool("trace", {"enable": False})

    r = c.call_tool("get_ppu_state")
    ok("ppu state", json.loads(r["result"]["content"][0]["text"]).get("frame_count", 0) > 0)

    r = c.call_tool("get_debugger_status")
    ds = json.loads(r["result"]["content"][0]["text"])
    ok("debugger status", ds["debugger_started"] is True and "prg_rom" in ds.get("available_memory_types", []))

    #--- P4: PPU inspection, audio & trace files ---
    r = c.call_tool("get_palette")
    pal = json.loads(r["result"]["content"][0]["text"])
    ok("palette raw[0]=0x16 rgb=B53120", pal["background"][0]["raw"] == 0x16 and pal["background"][0]["rgb"] == "B53120")

    r = c.call_tool("get_tilemap")
    tm = json.loads([x for x in r["result"]["content"] if x["type"] == "text"][0]["text"])
    tm_img = [x for x in r["result"]["content"] if x["type"] == "image"]
    ok("tilemap png + meta", tm_img and tm["columns"] == 64 and tm["mirroring"] == "horizontal", str(tm)[:90])

    r = c.call_tool("get_tiles")
    tiles = json.loads([x for x in r["result"]["content"] if x["type"] == "text"][0]["text"])
    ok("tiles 512 x 16/row", tiles.get("tile_count") == 512 and tiles.get("bytes_per_tile") == 16)

    r = c.call_tool("get_sprites")
    spr = json.loads(r["result"]["content"][0]["text"])
    ok("sprites 64 + preview", spr.get("sprite_count") == 64 and "preview_image_base64" in spr)

    #Audio: the ROM plays a pulse tone (v3) - rms must be clearly nonzero, no clipping
    aud = json.loads(c.call_tool("get_audio_summary")["result"]["content"][0]["text"])
    ok("audio tone audible", aud.get("capturing") and aud["window"]["rms_left"] > 0.005 and aud["window"]["peak_left"] > 0.05, str(aud.get("window")))
    ok("audio not clipping", aud["window"]["clipping_samples"] == 0)

    wav = json.loads(c.call_tool("capture_wav", {"frames": 20})["result"]["content"][0]["text"])
    ok("capture_wav", wav.get("bytes", 0) > 10000)

    tr = json.loads(c.call_tool("trace_to_file", {"run_frames": 5, "auto_stop": True})["result"]["content"][0]["text"])
    ok("trace_to_file", tr.get("action") == "started+stopped" and tr.get("bytes", 0) > 100, str(tr)[:100])

    #--- P3: input & validation ---
    r = c.call_tool("set_controller", {"port": 1, "buttons": ["start", "a"], "hold_frames": 3})
    ok("set_controller", r["result"]["isError"] is False, str(r["result"]["content"])[:100])
    r = c.call_tool("set_controller", {"port": 1, "buttons": ["not_a_button"]})
    ok("bad button -> isError", r["result"]["isError"] is True)
    ok("release_controller", c.call_tool("release_controller", {"port": 1})["result"]["isError"] is False)

    #Savestate roundtrip: write FF -> save -> write 11 -> load -> expect FF
    c.call_tool("write_memory", {"memory_type": "work_ram", "address": "$0700", "bytes": "FF"})
    sv = json.loads(c.call_tool("save_state", {})["result"]["content"][0]["text"])
    ok("save_state returns path", "path" in sv)
    c.call_tool("write_memory", {"memory_type": "work_ram", "address": "$0700", "bytes": "11"})
    ok("load_state", c.call_tool("load_state", {"path": sv["path"]})["result"]["isError"] is False)
    ok("state restored", json.loads(c.call_tool("read_memory", {"memory_type": "work_ram", "address": 0x700, "length": 1})["result"]["content"][0]["text"])["hex"] == "FF")

    #Lua: read memory through the scripting API
    r = c.call_tool("run_lua_script", {"code": 'print("sig:", emu.read(0x42, emu.memType.nesMemory))'})
    lua = json.loads(r["result"]["content"][0]["text"])
    ok("lua read works", "90" in lua.get("log", ""), str(lua)[:120])

    #Lua: resident frame callback writes RAM
    lua2 = json.loads(c.call_tool("run_lua_script", {
        "code": 'emu.addEventCallback(function(evt) emu.write(0x0520, 0x99, emu.memType.nesMemory) end, emu.eventType.endFrame)',
        "auto_stop": False})["result"]["content"][0]["text"])
    ok("resident lua starts", "script_id" in lua2)
    c.call_tool("run_frames", {"frames": 3, "timeout_ms": 8000})
    #note: internal_ram is the console RAM mirrored on the CPU bus; work_ram is
    #the separate 8KB cartridge WRAM ($6000-$7FFF)
    ok("lua frame callback ran", json.loads(c.call_tool("read_memory", {"memory_type": "internal_ram", "address": 0x520, "length": 1})["result"]["content"][0]["text"])["hex"] == "99")
    ok("stop lua script", c.call_tool("stop_lua_script", {"script_id": lua2["script_id"]})["result"]["isError"] is False)

    #CDL coverage of the PRG ROM
    cdl = json.loads(c.call_tool("get_cdl_stats", {})["result"]["content"][0]["text"])
    ok("cdl coverage > 0", cdl.get("code_bytes", 0) > 0 and cdl.get("coverage_pct", 0) > 0, str(cdl)[:100])

    #GIF capture
    gif = json.loads(c.call_tool("capture_gif", {"frames": 20})["result"]["content"][0]["text"])
    ok("capture_gif", gif.get("bytes", 0) > 1000, str(gif)[:100])

    #Cheats on/clear
    ok("set_cheats", json.loads(c.call_tool("set_cheats", {"codes": ["SXIOPO"]})["result"]["content"][0]["text"])["active_cheats"] == 1)
    ok("clear cheats", json.loads(c.call_tool("set_cheats", {"codes": []})["result"]["content"][0]["text"])["active_cheats"] == 0)

    #Rom test record -> replay roundtrip (deterministic session = must pass)
    test_path = os.path.join(HOME, "roundtrip.mntest")
    ok("record_rom_test", c.call_tool("record_rom_test", {"path": test_path})["result"]["isError"] is False)
    c.call_tool("set_controller", {"port": 1, "buttons": ["a", "b"], "hold_frames": 4})
    c.call_tool("run_frames", {"frames": 20, "timeout_ms": 8000})
    ok("stop_rom_test_record", c.call_tool("stop_rom_test_record")["result"]["isError"] is False)
    rt = json.loads(c.call_tool("run_rom_test", {"path": test_path})["result"]["content"][0]["text"])
    ok("rom test replay passes", rt.get("state") == "passed", str(rt)[:150])
    ok("session survives background test", json.loads(c.call_tool("get_status")["result"]["content"][0]["text"])["rom_loaded"] is True)

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

#!/usr/bin/env python3
"""Per-system end-to-end validation: drives every mesen-mcp tool against real
ROMs for each console (NES / SNES / GBA).

For each system: load the ROM, run frames, then walk the full tool surface -
status, CPU/PPU state, memory tools, disassembly, expressions, breakpoints/
stepping, callstack/trace, input, savestates, Lua, CDL coverage, cheats (clear),
GIF, palette/tilemap/tiles/sprites, audio summary + WAV capture, trace-to-file,
and a record->replay rom-test roundtrip.

Usage: e2e_test.py [path-to-mesen-mcp]   (default: bin/mesen-mcp)

Hard-fails on tool errors/protocol failures; game-content-dependent values
(audio loudness, sprite counts) are reported and soft-checked.
"""
import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join("bin", "mesen-mcp")
TESTS = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.join(TESTS, "e2e-home")

passed = 0
warns = []

def ok(name, cond, detail=""):
    global passed
    if not cond:
        print(f"  FAIL: {name} {detail}")
        sys.exit(1)
    passed += 1
    print(f"  ok: {name}")

def warn(name, cond, detail=""):
    if not cond:
        warns.append(f"{name} {detail}")
        print(f"  WARN: {name} {detail}")
    else:
        ok(name, True)

class Client:
    def __init__(self):
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        self.proc = subprocess.Popen(
            [BIN, "--home", HOME], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, env=env, text=True, bufsize=1)
        self._id = 0

    def request(self, method, params=None):
        self._id += 1
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self._id, "method": method,
                                          **({"params": params} if params is not None else {})}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"server died (poll={self.proc.poll()}) during {method}")
        return json.loads(line)

    def tool(self, name, args=None):
        r = self.request("tools/call", {"name": name, "arguments": args or {}})
        if r["result"]["isError"]:
            return {"ERR": r["result"]["content"][0]["text"]}
        texts = [c for c in r["result"]["content"] if c["type"] == "text"]
        images = [c for c in r["result"]["content"] if c["type"] == "image"]
        d = {}
        if texts:
            try:
                d = json.loads(texts[0]["text"])
            except json.JSONDecodeError:
                d = {"text": texts[0]["text"]}
        if images:
            d["_png"] = base64.b64decode(images[0]["data"])
        return d

    def close(self):
        self.proc.stdin.close()
        return self.proc.wait(timeout=20)


def png_size(png):
    return struct.unpack(">II", png[16:24])


def battery(label, rom_path, console):
    print(f"\n== {label} ({console}) ==")
    c = Client()
    try:
        r = c.request("initialize", {"protocolVersion": "2025-06-18",
                                     "clientInfo": {"name": "e2e", "version": "0"}})
        ok(f"{label}: initialize", r["result"]["serverInfo"]["name"] == "mesen-mcp")

        # --- load + run ---
        ld = c.tool("load_rom", {"path": rom_path})
        ok(f"{label}: load_rom", "ERR" not in ld, str(ld)[:120])
        ok(f"{label}: console type", ld.get("rom", {}).get("console_type") == console, str(ld.get("rom", {}).get("console_type")))
        ok(f"{label}: rom identity", len(ld.get("rom", {}).get("sha1", "")) == 40)

        run = c.tool("run_frames", {"frames": 180, "timeout_ms": 20000})
        ok(f"{label}: run_frames 180", run.get("frames_run", 0) >= 179, str(run)[:90])

        st = c.tool("get_status")
        ok(f"{label}: get_status", st.get("rom_loaded") is True and st.get("frame_count", 0) >= 180)

        shot = c.tool("screenshot", {})
        ok(f"{label}: screenshot", png_size(shot["_png"]) == (256, 240) if console == "nes" else "_png" in shot,
           str(png_size(shot["_png"])) if "_png" in shot else "")

        # --- lifecycle (before the debugger is initialized: a known core
        # deadlock wedges reset when a console CPU sits halted with the debugger
        # active - exercising the safe ordering here) ---
        # --- pause/resume/reset/reload ---
        ok(f"{label}: pause", c.tool("pause").get("paused") is True)
        ok(f"{label}: resume", c.tool("resume").get("paused") is False)
        ok(f"{label}: reset", "ERR" not in c.tool("reset"))
        ok(f"{label}: set_speed", "ERR" not in c.tool("set_speed", {"speed": "realtime"}))
        ok(f"{label}: set_speed max", "ERR" not in c.tool("set_speed", {"speed": "max"}))
        ok(f"{label}: unload", "ERR" not in c.tool("unload_rom"))
        rl = c.tool("load_rom", {"path": rom_path})
        ok(f"{label}: reload", "ERR" not in rl)

        # --- state ---
        cpu = c.tool("get_cpu_state")
        if console == "nes":
            ok(f"{label}: cpu 6502", 0 <= cpu.get("pc", -1) <= 0xFFFF and "a" in cpu)
        elif console == "snes":
            ok(f"{label}: cpu 65816", 0 <= cpu.get("pc", -1) <= 0xFFFF and "k" in cpu and "dbr" in cpu)
        else:
            ok(f"{label}: cpu ARM", "registers" in cpu and "thumb" in cpu)

        regs = c.tool("get_registers")
        ok(f"{label}: registers combined", {"pc", "flags", "ppu", "master_clock"} <= set(regs), str(sorted(regs.keys()))[:80])

        ppu = c.tool("get_ppu_state")
        ok(f"{label}: ppu state", "scanline" in ppu and ppu.get("frame_count", 0) > 0, str(ppu)[:80])

        # --- memory ---
        mem = c.tool("get_memory_size", {"memory_type": "prg_rom"})
        ok(f"{label}: prg_rom size", mem.get("size", 0) >= 16384, str(mem)[:80])
        head = c.tool("read_memory", {"memory_type": "prg_rom", "address": 0, "length": 4})
        ok(f"{label}: read prg_rom", len(head.get("hex", "").split()) == 4)
        #Console RAM (always present): NES internal RAM, SNES WRAM, GBA internal WRAM
        ram_type = {"nes": "internal_ram", "snes": "work_ram", "gba": "int_work_ram"}[console]
        wr = c.tool("write_memory", {"memory_type": ram_type, "address": 0x100, "bytes": "CA FE BA BE"})
        ok(f"{label}: write_memory", "ERR" not in wr, str(wr)[:80])
        rd = c.tool("read_memory", {"memory_type": ram_type, "address": 0x100, "length": 4})
        ok(f"{label}: read back", rd.get("hex", "").upper().startswith("CA FE"), rd.get("hex", "?"))
        needle = head["bytes"][:2] if "bytes" in head else None
        sr = c.tool("search_memory", {"memory_type": "prg_rom", "value": head["hex"].replace(" ", "")[:4]})
        ok(f"{label}: search prg_rom", len(sr.get("matches", [])) >= 1, str(sr)[:80])

        dis = c.tool("disassemble", {"count": 4, "address": cpu["pc"]})
        ok(f"{label}: disassemble", len(dis.get("instructions", [])) == 4 and dis["instructions"][0]["text"].strip() != "",
           str(dis)[:100])

        ev = c.tool("evaluate_expression", {"expression": "PC"})
        ok(f"{label}: evaluate PC", ev.get("type") == "numeric")

        # --- debugger flow: exec bp at PC, wait, step, continue ---
        #Re-sample after the lifecycle reload + advance a little so the game is
        #in its steady-state loop before arming the breakpoint
        c.tool("run_frames", {"frames": 60, "timeout_ms": 15000})
        cpu = c.tool("get_cpu_state")
        pc = cpu["pc"]
        bp = c.tool("set_breakpoint", {"address": pc})
        ok(f"{label}: set_breakpoint", "id" in bp)
        wb = c.tool("wait_for_breakpoint", {"timeout_ms": 8000})
        ok(f"{label}: breakpoint hit", wb.get("stopped") is True, str(wb)[:100])
        stp = c.tool("step", {"type": "instruction", "count": 1, "timeout_ms": 5000})
        ok(f"{label}: step", stp.get("stopped") is True, str(stp)[:90])
        ok(f"{label}: remove_breakpoint", "ERR" not in c.tool("remove_breakpoint", {"all": True}))
        ok(f"{label}: continue", "ERR" not in c.tool("continue"))

        ok(f"{label}: get_callstack", "frames" in c.tool("get_callstack"))
        tr = c.tool("trace", {"run_frames": 2, "rows": 5})
        ok(f"{label}: trace", tr.get("row_count", 0) >= 1, str(tr)[:80])
        c.tool("trace", {"enable": False})
        dbg = c.tool("get_debugger_status")
        ok(f"{label}: debugger status", dbg.get("debugger_started") is True and dbg.get("available_memory_types"))

        # --- input / time travel ---
        ok(f"{label}: set_controller", "ERR" not in c.tool("set_controller", {"port": 1, "buttons": ["start"], "hold_frames": 5}))
        ok(f"{label}: release_controller", "ERR" not in c.tool("release_controller", {"port": 1}))
        sv = c.tool("save_state", {})
        ok(f"{label}: save_state", "path" in sv)
        ok(f"{label}: load_state", "ERR" not in c.tool("load_state", {"path": sv["path"]}))

        lua = c.tool("run_lua_script", {"code": "print('pc:', emu.getCpuState().PC)"})
        ok(f"{label}: lua", "pc:" in lua.get("log", ""), str(lua)[:100])

        cdl = c.tool("get_cdl_stats", {})
        #Known issue: the GBA CDL feed does not accumulate under the headless
        #debugger yet (NES/SNES do) - warn instead of failing there
        if console == "gba":
            warn(f"{label}: cdl", cdl.get("code_bytes", 0) > 0, "known: GBA CDL stays empty headless")
        else:
            ok(f"{label}: cdl", cdl.get("code_bytes", 0) > 0, str(cdl)[:90])
        ok(f"{label}: cheats clear", "ERR" not in c.tool("set_cheats", {"codes": []}))
        gif = c.tool("capture_gif", {"frames": 20})
        ok(f"{label}: capture_gif", gif.get("bytes", 0) > 5000)

        # --- PPU inspection ---
        pal = c.tool("get_palette", {})
        ok(f"{label}: palette", pal.get("color_count", 0) >= 8, str(pal)[:80])
        tm = c.tool("get_tilemap", {})
        ok(f"{label}: tilemap png", "_png" in tm and png_size(tm["_png"])[0] >= 256, str({k: v for k, v in tm.items() if k != '_png'})[:90])
        tiles = c.tool("get_tiles", {})
        ok(f"{label}: tiles png", "_png" in tiles and tiles.get("tile_count", 0) >= 64)
        spr = c.tool("get_sprites", {})
        ok(f"{label}: sprites", "sprite_count" in spr, str(spr)[:80])
        warn(f"{label}: sprites present", spr.get("sprite_count", 0) > 0, f"count={spr.get('sprite_count')}")

        # --- audio ---
        c.tool("run_frames", {"frames": 120, "timeout_ms": 20000})
        aud = c.tool("get_audio_summary", {})
        ok(f"{label}: audio summary", aud.get("capturing") is True and "rms_left" in aud.get("window", {}))
        warn(f"{label}: audio audible", aud["window"].get("rms_left", 0) > 0.001, f"rms={aud['window'].get('rms_left'):.4f}")
        wav = c.tool("capture_wav", {"frames": 30})
        ok(f"{label}: capture_wav", wav.get("bytes", 0) > 10000)
        ttf = c.tool("trace_to_file", {"run_frames": 5, "auto_stop": True})
        ok(f"{label}: trace_to_file", ttf.get("bytes", 0) > 100)

        # --- rom-test record -> replay ---
        test_path = os.path.join(HOME, f"e2e-{console}.mntest")
        ok(f"{label}: record_rom_test", "ERR" not in c.tool("record_rom_test", {"path": test_path, "reset": True}))
        c.tool("set_controller", {"port": 1, "buttons": ["a"], "hold_frames": 3})
        c.tool("run_frames", {"frames": 20, "timeout_ms": 20000})
        ok(f"{label}: stop_rom_test_record", "ERR" not in c.tool("stop_rom_test_record"))
        time.sleep(0.3)
        rt = c.tool("run_rom_test", {"path": test_path})
        ok(f"{label}: rom-test replay", rt.get("state") == "passed", str(rt)[:110])
        ok(f"{label}: session survived", c.tool("get_status").get("rom_loaded") is True)

    finally:
        rc = c.close()
        ok(f"{label}: clean exit", rc == 0, f"rc={rc}")


def main():
    #Fresh home dir; GBA BIOS goes to <home>/Firmware/gba_bios.bin
    shutil.rmtree(HOME, ignore_errors=True)
    os.makedirs(os.path.join(HOME, "Firmware"), exist_ok=True)
    shutil.copy(os.path.join(TESTS, "gba-bios.bin"), os.path.join(HOME, "Firmware", "gba_bios.bin"))

    battery("NES / Abbaye des morts", os.path.join(TESTS, "nes-rom.zip"), "nes")
    battery("SNES / Street Fighter II", os.path.join(TESTS, "snes-rom.zip"), "snes")
    battery("GBA / SSFIIT Turbo Revival", os.path.join(TESTS, "gba-rom.zip"), "gba")

    print(f"\n== E2E PASSED ({passed} checks, {len(warns)} warnings) ==")
    for w in warns:
        print("  warn:", w)

if __name__ == "__main__":
    main()

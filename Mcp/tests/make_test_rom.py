#!/usr/bin/env python3
"""Generates a tiny NES test ROM for the MesenMCP headless smoke test.

The ROM (mapper 0 / NROM, 16KB PRG + 8KB CHR) runs the standard NES init
sequence, clears nametable 0, sets the backdrop palette entry to color $16
(bright red) and enables background rendering. CHR tile 0 is left as all
zeros, so every pixel on screen uses palette index 0 -> the entire screen
renders as a solid red rectangle.

Run with no arguments: writes red.nes next to this script.
Run with a path argument: writes the ROM to that path.

All code bytes below are hand-assembled 6502 and loaded at $C000.
"""
import os
import sys

def build_rom() -> bytes:
    code = bytes([
        0x78,                         # $C000: SEI
        0xD8,                         # $C001: CLD
        0xA2, 0x40,                   # $C002: LDX #$40
        0x8E, 0x17, 0x40,             # $C004: STX $4017 (disable APU frame IRQ)
        0xA2, 0xFF,                   # $C007: LDX #$FF
        0x9A,                         # $C009: TXS
        0xE8,                         # $C00A: INX (X = 0)
        0x8E, 0x00, 0x20,             # $C00B: STX $2000 (PPUCTRL = 0, NMI off)
        0x8E, 0x01, 0x20,             # $C00E: STX $2001 (PPUMASK = 0, rendering off)

        # Wait for two vblanks (PPU warm-up)
        0x2C, 0x02, 0x20,             # $C011: BIT $2002
        0x10, 0xFB,                   # $C014: BPL $C011
        0x2C, 0x02, 0x20,             # $C016: BIT $2002
        0x10, 0xFB,                   # $C019: BPL $C016

        # Palette: $3F00 = $16 (red, backdrop), $3F01 = $30 (white)
        0xA9, 0x3F,                   # $C01B: LDA #$3F
        0x8D, 0x06, 0x20,             # $C01D: STA $2006
        0xA9, 0x00,                   # $C020: LDA #$00
        0x8D, 0x06, 0x20,             # $C022: STA $2006
        0xA9, 0x16,                   # $C025: LDA #$16
        0x8D, 0x07, 0x20,             # $C027: STA $2007
        0xA9, 0x30,                   # $C02A: LDA #$30
        0x8D, 0x07, 0x20,             # $C02C: STA $2007

        # Clear nametable 0 ($2000-$23FF, 1024 bytes incl. attribute table) to tile 0
        0xA9, 0x20,                   # $C02F: LDA #$20
        0x8D, 0x06, 0x20,             # $C031: STA $2006
        0xA9, 0x00,                   # $C034: LDA #$00
        0x8D, 0x06, 0x20,             # $C036: STA $2006
        0xA2, 0x04,                   # $C039: LDX #$04 (4 x 256 bytes)
        0xA0, 0x00,                   # $C03B: LDY #$00
        0xA9, 0x00,                   # $C03D: LDA #$00
        # clearloop ($C03F):
        0x8D, 0x07, 0x20,             # $C03F: STA $2007
        0xC8,                         # $C042: INY
        0xD0, 0xFA,                   # $C043: BNE $C03F
        0xCA,                         # $C045: DEX
        0xD0, 0xF7,                   # $C046: BNE $C03F

        # Enable background rendering
        0xA9, 0x08,                   # $C048: LDA #$08
        0x8D, 0x01, 0x20,             # $C04A: STA $2001

        0x40,                         # $C04D: RTI (NMI/IRQ handler - both are disabled)
        0x4C, 0x4E, 0xC0,             # $C04E: JMP $C04E (halt)
    ])

    prg = bytearray(16 * 1024)        # 16KB PRG, mapped at $C000-$FFFF
    prg[0:len(code)] = code

    # Vectors at $FFFA (PRG offset $3FFA): NMI, RESET, IRQ
    prg[0x3FFA:0x3FFC] = (0xC04D).to_bytes(2, "little")  # NMI -> RTI
    prg[0x3FFC:0x3FFE] = (0xC000).to_bytes(2, "little")  # RESET
    prg[0x3FFE:0x4000] = (0xC04D).to_bytes(2, "little")  # IRQ -> RTI

    chr_rom = bytearray(8 * 1024)     # 8KB CHR, all zeros (tile 0 = blank, palette idx 0)

    header = bytes([
        0x4E, 0x45, 0x53, 0x1A,       # "NES\x1a"
        1,                            # PRG ROM: 1 x 16KB bank
        1,                            # CHR ROM: 1 x 8KB bank
        0x00,                         # flags6: mapper 0, horizontal mirroring
        0x00,                         # flags7: mapper 0, iNES format
        0, 0, 0, 0, 0, 0, 0, 0        # padding
    ])

    return header + bytes(prg) + bytes(chr_rom)

def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "red.nes")
    rom = build_rom()
    with open(out, "wb") as f:
        f.write(rom)
    print(f"wrote {out} ({len(rom)} bytes)")

if __name__ == "__main__":
    main()

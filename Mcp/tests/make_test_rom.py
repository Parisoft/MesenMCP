#!/usr/bin/env python3
"""Generates a tiny NES test ROM for the MesenMCP smoke tests.

ROM v2 (mapper 0 / NROM, 16KB PRG + 8KB CHR). Designed to be robust for
debugger tooling:

- Uses a cycle-counted delay loop for the PPU warm-up instead of polling
  $2002 (tight $2002 polls can phase-lock into Mesen's vblank-flag read
  suppression race under debugger instrumentation).
- Sets the backdrop palette to $16 (bright red) and enables background
  rendering -> solid red screen for screenshot tests.
- Writes a signature ($5A) to zero page $42-$45 for search/read tests.
- Starts a pulse-1 tone (constant volume, halted length) so audio tools have a
  nonzero signal to verify.
- Main loop calls a subroutine at $C100 (main loop at $C06B) which stores $5A at $0500 - a
  reliably-hitting execute breakpoint target.
- NMI/IRQ handlers are RTI; both interrupt sources are disabled.

Run with no arguments: writes red.nes next to this script.
Run with a path argument: writes the ROM to that path.

All code bytes below are hand-assembled 6502, loaded at $C000.
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

        # PPU warm-up delay: ~64k cycles (~2.2 frames), no $2002 reads
        0xA2, 0x32,                   # $C011: LDX #$50 (outer)
        0xA0, 0x00,                   # $C013: LDY #$00        <- outer loop
        0x88,                         # $C015: DEY             <- inner loop
        0xD0, 0xFD,                   # $C016: BNE $C015
        0xCA,                         # $C018: DEX
        0xD0, 0xF8,                   # $C019: BNE $C013

        # Palette: $3F00 = $16 (red, backdrop), $3F01 = $30 (white)
        0xA9, 0x3F,                   # $C01B: LDA #$3F
        0x8D, 0x06, 0x20,             # $C01D: STA $2006
        0xA9, 0x00,                   # $C020: LDA #$00
        0x8D, 0x06, 0x20,             # $C022: STA $2006
        0xA9, 0x16,                   # $C025: LDA #$16
        0x8D, 0x07, 0x20,             # $C027: STA $2007
        0xA9, 0x30,                   # $C02A: LDA #$30
        0x8D, 0x07, 0x20,             # $C02C: STA $2007

        # Zero-page signature $42-$45 = $5A (for memory tool tests)
        0xA9, 0x5A,                   # $C02F: LDA #$5A
        0x85, 0x42,                   # $C031: STA $42
        0x85, 0x43,                   # $C033: STA $43
        0x85, 0x44,                   # $C035: STA $44
        0x85, 0x45,                   # $C037: STA $45

        # Clear nametable 0 ($2000-$23FF, 1024 bytes) to tile 0
        0xA9, 0x20,                   # $C039: LDA #$20
        0x8D, 0x06, 0x20,             # $C03B: STA $2006
        0xA9, 0x00,                   # $C03E: LDA #$00
        0x8D, 0x06, 0x20,             # $C040: STA $2006
        0xA2, 0x04,                   # $C043: LDX #$04 (4 x 256 bytes)
        0xA0, 0x00,                   # $C045: LDY #$00
        0xA9, 0x00,                   # $C047: LDA #$00
        # clearloop ($C049):
        0x8D, 0x07, 0x20,             # $C049: STA $2007
        0xC8,                         # $C04C: INY
        0xD0, 0xFA,                   # $C04D: BNE $C049
        0xCA,                         # $C04F: DEX
        0xD0, 0xF7,                   # $C050: BNE $C049

        # Enable background rendering
        0xA9, 0x08,                   # $C052: LDA #$08
        0x8D, 0x01, 0x20,             # $C054: STA $2001

        # Start a pulse-1 tone (real CPU register writes - audio fixtures)
        0xA9, 0x0F,                   # $C057: LDA #$0F
        0x8D, 0x15, 0x40,             # $C059: STA $4015 (enable pulse 1+2+tri+noise)
        0xA9, 0xBF,                   # $C05C: LDA #$BF
        0x8D, 0x00, 0x40,             # $C05E: STA $4000 (duty 2, halt, const vol 15)
        0xA9, 0x20,                   # $C061: LDA #$20
        0x8D, 0x02, 0x40,             # $C063: STA $4002 (timer low)
        0xA9, 0x40,                   # $C066: LDA #$40
        0x8D, 0x03, 0x40,             # $C068: STA $4003 (timer high + length load)

        # main ($C06B): repeatedly call the $C100 subroutine
        0x20, 0x00, 0xC1,             # $C06B: JSR $C100
        0x4C, 0x6B, 0xC0,             # $C06E: JMP $C06B
    ])

    subroutine = bytes([
        0xA9, 0x5A,                   # $C100: LDA #$5A
        0x8D, 0x00, 0x05,             # $C102: STA $0500
        0x60,                         # $C105: RTS
    ])

    handlers = bytes([
        0x40,                         # $C110: RTI (NMI/IRQ - both disabled)
    ])

    prg = bytearray(16 * 1024)        # 16KB PRG, mapped at $C000-$FFFF
    prg[0:len(code)] = code
    prg[0x100:0x100 + len(subroutine)] = subroutine
    prg[0x110:0x110 + len(handlers)] = handlers

    # Vectors at $FFFA (PRG offset $3FFA): NMI, RESET, IRQ
    prg[0x3FFA:0x3FFC] = (0xC110).to_bytes(2, "little")  # NMI -> RTI
    prg[0x3FFC:0x3FFE] = (0xC000).to_bytes(2, "little")  # RESET
    prg[0x3FFE:0x4000] = (0xC110).to_bytes(2, "little")  # IRQ -> RTI

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

#!/usr/bin/env python3
"""Generates a NES test ROM that echoes both controller ports into RAM.

The ROM polls the joypads in a tight loop (many times per frame) and stores
what it read, so an MCP client can verify with read_memory that a button press
actually reached the *emulated game*:

    $00F0 - last byte read from port 1 ($4016)
    $00F1 - last byte read from port 2 ($4017)
    $00F2 - poll counter (proves the loop is running / the ROM is alive)
    $00F3 - sticky OR of every byte ever read from port 1 (never cleared by the
            ROM - a test can zero it with write_memory to watch for a short press)
    $00F4 - sticky OR of every byte ever read from port 2
    $00F5/$00F6 - scratch bytes used while a poll is being assembled

$00F0/$00F1 are only ever written with a complete byte, so they can be sampled
at any moment with read_memory.

Bit order of $00F0/$00F1 (the NES shift order: first bit read = bit 0):

    bit 0 = A          bit 4 = Up
    bit 1 = B          bit 5 = Down
    bit 2 = Select     bit 6 = Left
    bit 3 = Start      bit 7 = Right

Mapper 0 (NROM): 16KB PRG at $C000 + 8KB CHR. All code is hand-assembled 6502.

Run with no arguments: writes input-test.nes next to this script.
Run with a path argument: writes the ROM to that path.
"""
import os
import sys


def build_rom() -> bytes:
    ORG = 0xC000
    code = bytearray()

    def emit(*b):
        code.extend(b)

    def imm(opcode, value):          #LDA #nn / LDX #nn
        emit(opcode, value & 0xFF)

    def absolute(opcode, addr):      #LDA $nnnn / STA $nnnn / INC $nnnn
        emit(opcode, addr & 0xFF, (addr >> 8) & 0xFF)

    def zeropage(opcode, addr):      #LDA $nn / STA $nn
        emit(opcode, addr & 0xFF)

    #--- init ----------------------------------------------------------------
    emit(0x78)                       #SEI
    emit(0xD8)                       #CLD
    imm(0xA2, 0xFF)                  #LDX #$FF
    emit(0x9A)                       #TXS
    emit(0xE8)                       #INX           (X = 0)
    absolute(0x8E, 0x2000)           #STX $2000     PPUCTRL  = 0 (NMI off)
    absolute(0x8E, 0x2001)           #STX $2001     PPUMASK  = 0 (rendering off)
    absolute(0x8E, 0x4016)           #STX $4016     strobe off
    zeropage(0x86, 0xF0)             #STX $F0
    zeropage(0x86, 0xF1)             #STX $F1
    zeropage(0x86, 0xF2)             #STX $F2
    zeropage(0x86, 0xF3)             #STX $F3
    zeropage(0x86, 0xF4)             #STX $F4

    loop = ORG + len(code)

    absolute(0xEE, 0x00F2)           #INC $F2       poll counter

    #Latch both controllers: $4016 <- 1, then $4016 <- 0 (standard joypad read)
    imm(0xA9, 0x01)                  #LDA #$01
    absolute(0x8D, 0x4016)           #STA $4016
    imm(0xA9, 0x00)                  #LDA #$00
    absolute(0x8D, 0x4016)           #STA $4016

    def read_port(reg, dest, scratch):
        """Unrolled 8-bit read of a controller port, LSB first.

        The byte is assembled in a scratch byte and copied to `dest` with a
        single store, so `dest` never holds a half-read value: read_memory can
        sample it at any instant and always get a complete poll result.
        """
        imm(0xA9, 0x00)              #LDA #$00
        zeropage(0x85, scratch)      #STA scratch
        for i in range(8):
            absolute(0xAD, reg)      #LDA reg
            imm(0x29, 0x01)          #AND #$01      keep the reported button bit
            for _ in range(i):
                emit(0x0A)           #ASL A         shift it into place
            zeropage(0x05, scratch)  #ORA scratch
            zeropage(0x85, scratch)  #STA scratch
        zeropage(0xA5, scratch)      #LDA scratch
        zeropage(0x85, dest)         #STA dest      publish the finished byte

    read_port(0x4016, 0xF0, 0xF5)    #port 1 -> $F0 (scratch $F5)
    read_port(0x4017, 0xF1, 0xF6)    #port 2 -> $F1 (scratch $F6)

    #Sticky OR of everything ever seen (lets a test prove that a short press was
    #actually sampled, even after the buttons were released again)
    zeropage(0xA5, 0xF0)             #LDA $F0
    zeropage(0x05, 0xF3)             #ORA $F3
    zeropage(0x85, 0xF3)             #STA $F3
    zeropage(0xA5, 0xF1)             #LDA $F1
    zeropage(0x05, 0xF4)             #ORA $F4
    zeropage(0x85, 0xF4)             #STA $F4

    emit(0x4C, loop & 0xFF, (loop >> 8) & 0xFF)   #JMP loop

    prg = bytearray(16 * 1024)       #16KB PRG, mapped at $C000-$FFFF
    prg[0:len(code)] = code

    handlers = bytes([0x40])         #RTI (NMI/IRQ - both disabled)
    prg[0x110:0x110 + len(handlers)] = handlers

    #Vectors at $FFFA (PRG offset $3FFA): NMI, RESET, IRQ
    prg[0x3FFA:0x3FFC] = (0xC110).to_bytes(2, "little")  #NMI   -> RTI
    prg[0x3FFC:0x3FFE] = (ORG).to_bytes(2, "little")     #RESET -> init
    prg[0x3FFE:0x4000] = (0xC110).to_bytes(2, "little")  #IRQ   -> RTI

    chr_rom = bytearray(8 * 1024)    #8KB CHR (unused, rendering stays off)

    header = bytes([
        0x4E, 0x45, 0x53, 0x1A,      #"NES\x1a"
        1,                           #PRG ROM: 1 x 16KB bank
        1,                           #CHR ROM: 1 x 8KB bank
        0x00,                        #flags6: mapper 0, horizontal mirroring
        0x00,                        #flags7: mapper 0, iNES format
        0, 0, 0, 0, 0, 0, 0, 0       #padding
    ])

    return header + bytes(prg) + bytes(chr_rom)


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "input-test.nes")
    rom = build_rom()
    with open(out, "wb") as f:
        f.write(rom)
    print(f"wrote {out} ({len(rom)} bytes)")


if __name__ == "__main__":
    main()

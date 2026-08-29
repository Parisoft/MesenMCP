; Test ROM for MesenMCP .dbg support
.segment "HEADER"
    .byte $4E,$45,$53,$1A,2,0,0,0,0,0,0,0,0,0,0,0

.segment "ZP"
ptr1:       .res 2
counter:    .res 1

.segment "BSS"
shadow_oam: .res 256
frame_count: .res 2

.segment "CODE"
.proc nmi_handler
    inc frame_count
    bne :+
    inc frame_count+1
:
    rti
.endproc

.proc main
    sei
    ldx #$FF
    txs
    cld
    lda #$00
    sta $2000
    sta $2001
    sta counter
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    lda #$16
    sta $2007
loop:
    jsr update_hud
    inc counter
    jmp loop
.endproc

.proc update_hud
    lda counter
    sta shadow_oam
    rts
.endproc

.segment "VECTORS"
    .word nmi_handler, main, nmi_handler

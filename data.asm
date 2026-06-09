; Rotatron SNES - ROM data (header + converted graphics).
.include "hdr.asm"

; Converted assets get .incbin'd here as the gfx pipeline lands, e.g.:
;   .section ".rodata_board" superfree
;   board_pic: .incbin "res/board.pic"
;   board_pal: .incbin "res/board.pal"
;   .ends

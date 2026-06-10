;== Rotatron SNES - LoROM header / interrupt vectors ==

.MEMORYMAP
  SLOTSIZE $8000
  DEFAULTSLOT 0
  SLOT 0 $8000
  SLOT 1 $0 $2000
  SLOT 2 $2000 $E000
  SLOT 3 $0 $10000
.ENDME

.ROMBANKSIZE $8000
.ROMBANKS 16                    ; 4 Mbit (512 KB): title + logo + backdrop + soundbank

.SNESHEADER
  ID "SNES"

  NAME "ROTATRON             "  ; exactly 21 bytes
  ;    "123456789012345678901"

  LOROM
  FASTROM                       ; documentation only: this WLA-DX still emits $20;
                                ; the Makefile patches $7FD5 -> $30 post-link

  CARTRIDGETYPE $00             ; ROM only (SRAM hiscore comes later)
  ROMSIZE $09                   ; 4 Megabits (512 KB)
  SRAMSIZE $00
  COUNTRY $01                   ; USA / NTSC
  LICENSEECODE $00
  VERSION $00
.ENDSNES

.SNESNATIVEVECTOR
  COP EmptyHandler
  BRK EmptyHandler
  ABORT EmptyHandler
  NMI VBlank
  IRQ EmptyHandler
.ENDNATIVEVECTOR

.SNESEMUVECTOR
  COP EmptyHandler
  ABORT EmptyHandler
  NMI EmptyHandler
  RESET tcc__start
  IRQBRK EmptyHandler
.ENDEMUVECTOR

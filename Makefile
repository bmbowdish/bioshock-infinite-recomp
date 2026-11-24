# Claude Fable - 2026-11-24: the mac32 framework now lives inside this repo (./mac32), copied from Recomp-Framework-main/mac32.
FW        = $(CURDIR)
GAME_BIN  = orig.bin
TEXT_LO   = 2cd0
TEXT_HI   = 13ec120
GEN_DIR   = gen2
HLE       = hle.txt
APP_NAME  = BioShock3
GAME_SRC  = game32.c
include $(FW)/mac32/mac32.mk

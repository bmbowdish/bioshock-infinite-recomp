# A 32-bit Mac game on Recomp-Framework (mac32). Copy this folder, fill in game32.c and hle.txt, then:
#   ln -s "/path/to/MyGame.app/Contents/MacOS/MyGame" orig.bin
#   make gen && make app && build/MyGame orig.bin "/path/to/MyGame.app/Contents/Resources"
FW        = ../..
GAME_BIN  = orig.bin
TEXT_LO   = 1000        # the __text section's range (otool -l orig.bin)
TEXT_HI   = 2000
HLE       = hle.txt
APP_NAME  = MyGame
GAME_SRC  = game32.c
include $(FW)/mac32/mac32.mk

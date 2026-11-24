# Recomp-Framework mac32: build rules for 32-bit Intel Mac games (Mach-O i386 -> C -> arm64, OpenGL on CGL).
# A game project's Makefile sets these and includes this file:
#
#   FW        = path/to/Recomp-Framework
#   GAME_BIN  = orig.bin                  # the original game's executable (a symlink into the user's copy)
#   TEXT_LO   = 2600                      # translated address range (hex): normally the whole __text section
#   TEXT_HI   = 49d140
#   HLE       = hle.txt                   # guest functions replaced by native ones (address name [outside])
#   APP_NAME  = MyGame                    # the .app name, and the executable inside it
#   GAME_SRC  = game32.c                  # Rt32Game settings + the game32_* hooks + the game's H_* functions
#   include $(FW)/mac32/mac32.mk
#
# Targets: gen (translate), app (build/<APP_NAME>), bundle (build/<BUNDLE_NAME>.app), memcheck (a build that reports
# guest writes to a chosen range, see rt.c), runtime (compile check), clean.
# Optional: BUNDLE_NAME (default APP_NAME), INFO_PLIST (the game's own Info.plist; default: generated),
# GAME_ICON (an .icns from the user's copy of the game), BUNDLE_ID, EXTRA_CFLAGS.

CC        ?= clang
PYTHON    ?= python3
MACOS_MIN ?= 11.0
GEN_DIR   ?= gen
BUILD     ?= build
APP_NAME  ?= Game
GAME_SRC  ?= game32.c
HLE       ?=
M32       = $(FW)/mac32

BUNDLE_NAME ?= $(APP_NAME)
CFLAGS ?= -O1 -g -arch arm64 -mmacosx-version-min=$(MACOS_MIN) -Wno-unused-variable -Wno-unused-but-set-variable \
          -Wno-deprecated-declarations -I$(M32)/runtime $(EXTRA_CFLAGS)

gen:
	$(PYTHON) $(M32)/tools/recomp32.py $(GAME_BIN) $(TEXT_LO) $(TEXT_HI) $(GEN_DIR) $(if $(HLE),--hle $(HLE))

$(BUILD)/gl_gen.c: $(M32)/tools/glgen.py
	@mkdir -p $(BUILD)
	$(PYTHON) $(M32)/tools/glgen.py $@

GEN_SRC = $(wildcard $(GEN_DIR)/code_*.c) $(GEN_DIR)/tables.c
GEN_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(GEN_SRC))
M32_RT  = rt.c shims.c sys.c cxx.c cxxstr.c libc2.c cf.c steam.c fmod.c vorbis.c gl.c objc.c eh.c bink.c
# Bink movies decode through FFmpeg when it is installed (brew install ffmpeg); otherwise they play as black frames
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavformat libavutil 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs libavcodec libavformat libavutil 2>/dev/null)
ifneq ($(FFMPEG_LIBS),)
FFMPEG_CFLAGS += -DHAVE_FFMPEG
endif
RT_OBJ  = $(patsubst %.c,$(BUILD)/m32/%.o,$(M32_RT)) $(BUILD)/m32/host.o $(BUILD)/gl_gen.o
GAME_OBJ = $(patsubst %.c,$(BUILD)/game/%.o,$(GAME_SRC))
M32_HDR = $(wildcard $(M32)/runtime/*.h)

$(BUILD)/$(GEN_DIR)/%.o: $(GEN_DIR)/%.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/m32/vorbis.o: $(M32)/runtime/vorbis.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(M32)/third_party -c $< -o $@
$(BUILD)/m32/%.o: $(M32)/runtime/%.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/m32/bink.o: CFLAGS += $(FFMPEG_CFLAGS)
$(BUILD)/m32/host.o: $(M32)/runtime/host.m $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fobjc-arc -c $< -o $@
$(BUILD)/gl_gen.o: $(BUILD)/gl_gen.c $(M32_HDR)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/game/%.o: %.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/libgame32.a: $(GEN_OBJ)
	rm -f $@; ar rcs $@ $^

# Claude Fable - 2026-11-24: Metal + MetalFX + IOSurface + QuartzCore were added for the MetalFX upscaler in runtime/host.m
# (IOSurface = memory shared between OpenGL and Metal; QuartzCore = CAMetalLayer). MetalFX exists on macOS 13+ only;
# with MACOS_MIN below 13 the linker weak-links it and host.m checks @available before using it.
FRAMEWORKS = -framework CoreFoundation -framework ApplicationServices -framework Cocoa -framework Carbon -framework OpenGL \
             -framework AudioToolbox -framework CoreAudio \
             -framework Metal -framework MetalFX -framework IOSurface -framework QuartzCore
$(BUILD)/$(APP_NAME): $(BUILD)/libgame32.a $(RT_OBJ) $(GAME_OBJ)
	$(CC) $(CFLAGS) $(RT_OBJ) $(GAME_OBJ) $(BUILD)/libgame32.a -lz -lc++ $(FFMPEG_LIBS) $(FRAMEWORKS) -o $@
app: $(BUILD)/$(APP_NAME)
# the runtime alone (no translated game): a quick compile check
runtime: $(RT_OBJ) $(GAME_OBJ)

# a double-clickable bundle (the game's data and icon come from the user's own copy at run time / build time)
APP_DIR = $(BUILD)/$(BUNDLE_NAME).app
bundle: $(BUILD)/$(APP_NAME)
	rm -rf "$(APP_DIR)"
	mkdir -p "$(APP_DIR)/Contents/MacOS" "$(APP_DIR)/Contents/Resources"
	cp $(BUILD)/$(APP_NAME) "$(APP_DIR)/Contents/MacOS/$(APP_NAME)"
ifneq ($(INFO_PLIST),)
	cp "$(INFO_PLIST)" "$(APP_DIR)/Contents/Info.plist"
else
	/usr/libexec/PlistBuddy -c "Add :CFBundleExecutable string $(APP_NAME)" -c "Add :CFBundleName string $(BUNDLE_NAME)" \
	    -c "Add :CFBundlePackageType string APPL" -c "Add :CFBundleIdentifier string $(or $(BUNDLE_ID),com.example.game32)" \
	    -c "Add :NSHighResolutionCapable bool true" -c "Add :CFBundleIconFile string GameApp" "$(APP_DIR)/Contents/Info.plist"
endif
	-$(if $(GAME_ICON),cp "$(GAME_ICON)" "$(APP_DIR)/Contents/Resources/GameApp.icns")
	codesign --force --sign - "$(APP_DIR)"

# MEMCHECK build in build-mc/: reports guest writes to [RT32_MC_LO, RT32_MC_HI) once a function matching RT32_MC_ARM ran
memcheck:
	$(MAKE) app BUILD=build-mc EXTRA_CFLAGS=-DMEMCHECK

clean:
	rm -rf $(BUILD)

.PHONY: gen app runtime bundle memcheck clean

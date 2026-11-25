// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * game32_window_size(): reports the resolution the engine is really rendering (learned in the runtime's gl.c),
//    so the window and the MetalFX upscaler follow the resolution picked in the game's menu.
//  * game32_windowed(): follows the game's Fullscreen option (runtime cf.c, rt_game_fullscreen).
//  * Texture pool: the engine is started with -ReadTexturePoolFromIni so [TextureStreaming] PoolSize in XEngine.ini
//    is used. Without it the engine sizes the pool from a "detected video memory" of 768 MB (728 MB pool), which
//    made textures flip between blurry and sharp at 4K. See the [pool] block after PreInit and in game32_frame.
//  * RT32_GAME_ARGS="...": extra engine command-line switches for experiments.
//  * GAME_EXE_SHA256 / rt32_game.exe_sha256: the one build of the game this port fits (checked at launch and by
//    the build scripts).
//  * H_XInputGetState / H_XInputSetState: the game's own controller functions, replaced through hle.txt and
//    answered from the runtime's controller support (mac32/runtime/pad.m).
// =====================================================================
// BioShock Infinite (Aspyr 2013 Mac build, UE3) on Recomp-Framework mac32.
// The Aspyr launcher (main @0x5b3a2) is Carbon-based; we skip it and drive UE3's GuardedMain steps ourselves:
//   0x67a787 = WinMain-like  ->  0x67a570 = GuardedMain, which does
//   PreInit 0x67caaa(&GEngineLoop, cmdline); Init 0x67dce0(&GEngineLoop); while(!GIsRequestingExit) Tick 0x67e7a6(&GEngineLoop);
//   Exit 0x67e1ea.   GEngineLoop = 0x1bdd0f8,  GIsRequestingExit pointer slot = 0x1a3d510.
// ⚠ First draft: addresses read from disassembly, not yet verified at runtime.
#include <stdio.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "game32.h"
#include "tables.h"

// Claude Fable - 2026-11-24: the one build of the game this port fits (Steam, Mac, Aspyr; executable BioShock3, 43,656,724 bytes).
// starts.txt, hle.txt and every address in this file are positions inside that executable. regen.sh checks the
// user's copy against this before translating, and the app checks it again at launch.
#define GAME_EXE_SHA256 "ed65b2c936d8b4d92a851e7bb676cfc0e9f9e88617564263d62651c2f68ce74b"

const Rt32Game rt32_game = {
    .name = "BioShock Infinite",
    .app_bundle = "BioShock3.app",
    .exe_name = "BioShock3",
    .steam_folder = "BioShock Infinite",
    .prefs_domain = "com.aspyr.bioshock3", // ⚠ guess, not verified
    .support_dir = "BioShock Infinite",    // the game's own user-data folder (Steam Cloud stand-in, offline stats)
    .log_name = "BioShock3-recomp",
    .width = 1280,
    .height = 720,
    .exe_sha256 = GAME_EXE_SHA256,
    .exe_version = "BioShock Infinite for Mac (Aspyr), Steam build",
};

#define ENGINE_LOOP 0x1bdd0f8u
#define FN_CMDLINE  0x3f23bu  // builds the command line (wide string) - same call WinMain makes
#define FN_STRIPEXE 0x8414ccu // strips the program name (as GuardedMain does)
// The Aspyr renderer asks for a 3.2 Core context (NSOpenGLPFAOpenGLProfile 0x3200); the host creates one to match.
int game32_gl_profile = 0x3200;

#define FN_PREINIT           0x67caaau
#define FN_ASPYR_INIT        0x2cd0u
#define FN_REGISTER_VF_TYPES 0x8e9432u // GuardedMain's first step: construct+register every FVertexFactoryType
#define FN_INIT              0x67dce0u
#define FN_TICK              0x67e7a6u
#define FN_EXIT              0x67e1eau
#define SLOT_EXITFLAG        0x1a3d510u

static int g_started;

// ---- Thread-local variables (Darwin TLV) ----
// The game has a __thread_vars descriptor {thunk, key, offset} at 0x1bce4c4 whose thunk dyld normally fills with
// tlv_get_addr. We install our own: it returns a zero-filled per-thread block + offset.
// ⚠ Only correct while the template is all-zero (__thread_data is empty in this binary; __thread_bss is 4 bytes).
#include <pthread.h>
typedef void (*gfn_t)(CPU *c);
uint32_t rt_host_fn(gfn_t f);
uint32_t gmalloc(uint32_t n);
#define TLV_DESC 0x1bce4c4u
static pthread_key_t g_tlv_key;
static pthread_once_t g_tlv_once = PTHREAD_ONCE_INIT;
static void tlv_mk(void) {
    pthread_key_create(&g_tlv_key, NULL);
}
static void tlv_get_addr(CPU *c) { // eax = descriptor; returns eax = address of this thread's variable
    pthread_once(&g_tlv_once, tlv_mk);
    uint32_t base = (uint32_t)(uintptr_t)pthread_getspecific(g_tlv_key);
    if (!base) {
        base = gmalloc(4096);
        memset(G2H(base), 0, 4096);
        pthread_setspecific(g_tlv_key, (void *)(uintptr_t)base);
    }
    c->eax = base + rd32(c->eax + 8);
}

static int exit_requested(void) {
    uint32_t p = rd32(SLOT_EXITFLAG); // slot holds the address of GIsRequestingExit
    return p ? (rd32(p) != 0) : 0;
}

// Debug: walk the engine's vertex-factory type registry (TLinkedList head at 0x1cbdd30; node = {element, next, ...};
// element+4 = wide-char name). Prints when it is healthy and where it breaks.
static void dump_vf_list(CPU *c, const char *tag) {
    (void)c;
    uint32_t node = rd32(0x1cbdd30u);
    int n = 0;
    fprintf(stderr, "[vf] %s: head 0x%x:", tag, node);
    while (node && n < 64) {
        if (node < 0x1000 || node >= 0xF0000000u) {
            fprintf(stderr, " <bad node 0x%x>", node);
            break;
        }
        uint32_t el = rd32(node), name = (el > 0x1000 && el < 0xF0000000u) ? rd32(el + 4) : 0;
        char b[48];
        int k = 0;
        for (uint32_t i = 0; name && name < 0xF0000000u && i < 40; i++) {
            uint32_t ch = rd32(name + 4 * i);
            if (!ch || ch > 0x7f)
                break;
            b[k++] = (char)ch;
        }
        b[k] = 0;
        fprintf(stderr, " %s", k ? b : "?");
        node = rd32(node + 4);
        n++;
    }
    fprintf(stderr, " (%d)\n", n);
}

static void dump_loc_globals(const char *tag) { // Localize() bails out early if any of these is 0
    fprintf(stderr, "[loc] %s: flag[1bef788]=%x GConfig[1bdf720]=%x GSys[1bdf738]=%x\n", tag, rd32(0x1bef788u),
            rd32(0x1bdf720u), rd32(0x1bdf738u));
}
int game32_start(CPU *c) {
    host_pad_read(0, NULL); // Claude Fable - 2026-11-24: start watching for controllers before the engine first asks
    // The engine builds its base directory from getcwd() + "Binaries/Win32" (UE3 convention) and finds XGame/Config two levels up.
    // Aspyr's data lives in <app>/Contents/GameData, so that is the working directory the game expects. ⚠ inferred, not verified.
    {
        char gd[2048];
        snprintf(gd, sizeof gd, "%s", host_resources_dir());
        char *q = strstr(gd, "/Contents/Resources");
        if (q) {
            strcpy(q, "/Contents/GameData");
        }
        if (chdir(gd) != 0)
            fprintf(stderr, "[game32] chdir(%s) failed\n", gd);
        else
            fprintf(stderr, "[game32] cwd = %s\n", gd);
    }
    wr32(TLV_DESC, rt_host_fn(tlv_get_addr));
    // Aspyr's platform init (normally run by their main(), which we bypass): sets the "initialized" flag that their
    // Win32-style CreateThread checks, reads Info.plist / localization, sets the locale.
    dump_vf_list(c, "before Aspyr init");
    dump_loc_globals("start");
    guest_call(c, FN_ASPYR_INIT, 0, NULL);
    fprintf(stderr, "[game32] Aspyr init done\n");
    dump_vf_list(c, "after Aspyr init");
    // Replay Aspyr's GuardedMain wrapper (sub_67a787), which our launcher bypasses (it runs the whole frame loop itself).
    {
        uint32_t a[3];
        a[0] = 0x67a765u;
        guest_call(c, 0x524d0u, 1, a); // install a callback
        a[0] = rd32(0x1a3d51cu);
        guest_call(c, 0x4e0b2u, 1, a);     // store a global pointer
        guest_call(c, 0x863e06u, 0, NULL); // appInitTiming
        a[0] = rd32(0x1a3d508u);
        a[1] = 0x40;
        a[2] = 0x16918dcu;
        guest_call(c, 0x52c11u, 3, a); // GGameName = "None"
        wr32(rd32(0x1a3d514u), 1);     // GIsStarted = 1 (Localize() returns nothing without it)
        wr32(rd32(0x1a3d574u), 0);     // the wrapper's argument (⚠ meaning unknown; 0)
    }
    uint32_t cmd = guest_call(c, FN_CMDLINE, 0, NULL);
    {
        uint32_t a[3];
        a[0] = rd32(0x1a3d508u);
        a[1] = 0x40;
        a[2] = 0x16918f0u;
        guest_call(c, 0x52c11u, 3, a); // GGameName = "X"
        a[0] = 0x16918f8u;
        guest_call(c, 0xa7304bu, 1, a); // splash image path
        wr32(rd32(0x1a3d50cu), 1);      // GIsGuarded = 1 while the engine runs
    }
    uint32_t a1[1] = {cmd};
    cmd = guest_call(c, FN_STRIPEXE, 1, a1);
    { // Claude Fable - 2026-11-24: RT32_GAME_ARGS="..." appends switches to the engine command line (wide string, 4-byte chars)
        // Default: -ReadTexturePoolFromIni, so [TextureStreaming] PoolSize in XEngine.ini is honoured (otherwise the
        // engine uses "detected video memory" = 768 MB -> 728 MB pool, which thrashes at 4K). RT32_NO_INI_POOL=1 reverts.
        static char argbuf[512];
        const char *e0 = getenv("RT32_GAME_ARGS");
        snprintf(argbuf, sizeof argbuf, "%s%s%s", getenv("RT32_NO_INI_POOL") ? "" : "-ReadTexturePoolFromIni",
                 (e0 && *e0) ? " " : "", (e0 && *e0) ? e0 : "");
        const char *extra = argbuf;
        if (extra && *extra && cmd) {
            uint32_t n = 0;
            while (rd32(cmd + 4 * n))
                n++;
            uint32_t m = (uint32_t)strlen(extra), nb = gmalloc(4 * (n + m + 2));
            for (uint32_t i = 0; i < n; i++)
                wr32(nb + 4 * i, rd32(cmd + 4 * i));
            wr32(nb + 4 * n, ' ');
            for (uint32_t j = 0; j < m; j++)
                wr32(nb + 4 * (n + 1 + j), (uint8_t)extra[j]);
            wr32(nb + 4 * (n + 1 + m), 0);
            cmd = nb;
            fprintf(stderr, "[game] command line extended with: %s\n", extra);
        }
    }
    uint32_t a2[2] = {ENGINE_LOOP, cmd};
    guest_call(c, FN_REGISTER_VF_TYPES, 0,
               NULL); // skipped along with Aspyr's launcher; shader maps resolve VF types by name
    dump_vf_list(c, "after VF registration");
    uint32_t err = guest_call(c, FN_PREINIT, 2, a2);
    { // Claude Fable - 2026-11-24: texture pool = int64 bytes at *(0x1A3D688), set by PreInit from the ini PoolSize.
        uint32_t pg = rd32(0x1A3D688u);
        int64_t pool = (int64_t)(((uint64_t)rd32(pg + 4) << 32) | rd32(pg));
        uint64_t ram = 0;
        size_t rl = sizeof ram;
        sysctlbyname("hw.memsize", &ram, &rl, NULL, 0);
        if (getenv("RT32_SIM_RAM_MB")) // testing: pretend the Mac has this much memory
            ram = (uint64_t)atoll(getenv("RT32_SIM_RAM_MB")) << 20;
        const char *f = getenv("RT32_TEXPOOL_MB");
        // Claude Fable - 2026-11-24 (revised): pick the pool by installed memory, using the ini value as a floor.
        //   <= 8 GB RAM : 800 MB, never more than 1000 MB (the pool is a ceiling, not a reservation: at the title
        //                 scene the footprint was ~1.6-1.7 GB with a 300 MB and with a 1000 MB pool alike)
        //   more RAM    : 1200 MB
        // The first version only capped at 500 MB, which left an 8 GB Mac on the ini default of 400 MB: smaller
        // than the 728 MB the engine picks by itself, and textures thrashed there. RT32_TEXPOOL_MB=N overrides.
        // Verified: forcing 300 MB through this write makes the title scene thrash (~265 textures created and
        // deleted per 100 frames), so the engine does honour a value written after PreInit.
        int small = ram && ram <= (9ull << 30);
        int64_t floor_mb = small ? 800 : 1200, cap_mb = small ? 1000 : 4096;
        int64_t want = pool;
        if (f && atoi(f) > 0) {
            want = (int64_t)atoi(f) << 20;
        } else {
            if (want < (floor_mb << 20))
                want = floor_mb << 20;
            if (want > (cap_mb << 20))
                want = cap_mb << 20;
        }
        fprintf(stderr, "[pool] RAM %llu MB | engine texture pool after PreInit: %lld MB%s\n",
                (unsigned long long)(ram >> 20), (long long)(pool >> 20), want != pool ? " (overriding)" : "");
        if (want != pool) {
            wr32(pg, (uint32_t)want);
            wr32(pg + 4, (uint32_t)((uint64_t)want >> 32));
            fprintf(stderr, "[pool] texture pool set to %lld MB\n", (long long)(want >> 20));
        }
    }
    fprintf(stderr, "[game32] PreInit returned %u\n", err);
    dump_vf_list(c, "after PreInit");
    dump_loc_globals("after PreInit");
    if (err)
        return 0;
    uint32_t a3[1] = {ENGINE_LOOP};
    err = guest_call(c, FN_INIT, 1, a3);
    fprintf(stderr, "[game32] Init returned %u\n", err);
    dump_loc_globals("after Init");
    if (err)
        return 0;
    g_started = 1;
    return 1;
}
// Claude Fable - 2026-11-24: the size the engine actually draws to the window (learned by the GL layer), 1280x720 until known.
void game32_window_size(CPU *c, float *w, float *h) {
    extern volatile int gl_engine_w, gl_engine_h;
    if (gl_engine_w >= 320 && gl_engine_h >= 200) {
        *w = (float)gl_engine_w;
        *h = (float)gl_engine_h;
    } else {
        *w = 1280;
        *h = 720;
    }
}
int game32_windowed(CPU *c) {
    extern volatile int rt_game_fullscreen;
    return !rt_game_fullscreen;
} // Claude Fable - 2026-11-24: follow the game's Fullscreen option
void game32_frame(CPU *c) {
    if (!g_started || exit_requested())
        return;
    { // Claude Fable - 2026-11-24: the engine's texture budget. PreInit (0x67d4dd..0x67d651) sets GTexturePoolSize (int64 at
        // 0x1BDD130) from the detected video memory in MB (int64 at 0x1BDCE28) minus TexturePoolSizeReductionMB, and only
        // reads PoolSize from the ini when that is 0 or -ReadTexturePoolFromIni is given.
        static int k;
        k++;
        if (k == 300 || (k % 6000) == 0) {
            uint32_t pg = rd32(0x1A3D688u);
            fprintf(stderr, "[pool] texture pool size now: %lld MB\n",
                    (long long)((int64_t)(((uint64_t)rd32(pg + 4) << 32) | rd32(pg)) >> 20));
        }
    }
    uint32_t a[1] = {ENGINE_LOOP};
    guest_call(c, FN_TICK, 1, a);
}
int game32_quit_requested(CPU *c) {
    return g_started && exit_requested();
}
void game32_mouse_move(CPU *c, float x, float y) {
}
void game32_mouse_button(CPU *c, int button, int down, int clicks) {
}
void game32_wheel(CPU *c, int delta) {
}
void game32_key(CPU *c, int down, uint32_t ch, uint32_t mods) {
}
void game32_modifier(CPU *c, int which, int down) {
}

// ---- Aspyr's message-box routine (guest function 0x5a08b), replaced so we can read what it says ----
// Args as the guest passes them: (?, message, secondary text/button, flags). ⚠ Meaning of arg0 and the return value is a guess.
static const char *gstr(uint32_t a) {
    return (a > 0x1000 && a < 0xF0000000u) ? (const char *)G2H(a) : "(not a string)";
}
// Claude Fable - 2026-11-24: controllers. The Mac port carries its own copy of Windows' XInput: 0x82b45 is
// XInputGetState(index, XINPUT_STATE *) and 0x82cd6 is XInputSetState(index, XINPUT_VIBRATION *). The originals find
// Xbox 360 / PS3 pads by talking to USB and HID devices through 2013 system interfaces that the runtime does not
// provide (they were answered "no devices", so no controller ever appeared). hle.txt replaces both functions with
// the two below, which answer from the host's controller support (mac32/runtime/pad.m) instead. The engine calls
// GetState for each of the four slots; 0x48F is Windows' ERROR_DEVICE_NOT_CONNECTED.
// XINPUT_STATE layout: +0 packet number, +4 buttons, +6 left trigger, +7 right trigger, +8 LX, +10 LY, +12 RX, +14 RY.
void H_XInputGetState(CPU *c) {
    uint32_t idx = ARG(0), st = ARG(1);
    Rt32Pad p;
    static int polled, answered; // one log line each, so the log shows the engine really reads controllers here
    if (!polled++)
        fprintf(stderr, "[pad] the game is polling controllers (first call: slot %u)\n", idx);
    if (!st || !host_pad_read((int)idx, &p)) {
        c->eax = 0x48F;
        return;
    }
    if (!answered++)
        fprintf(stderr, "[pad] first controller state handed to the game (slot %u)\n", idx);
    wr32(st, p.packet);
    wr16(st + 4, p.buttons);
    wr8(st + 6, p.lt);
    wr8(st + 7, p.rt);
    wr16(st + 8, (uint16_t)p.lx);
    wr16(st + 10, (uint16_t)p.ly);
    wr16(st + 12, (uint16_t)p.rx);
    wr16(st + 14, (uint16_t)p.ry);
    c->eax = 0;
}
// XINPUT_VIBRATION: +0 left (low-frequency) motor 0..65535, +2 right (high-frequency) motor 0..65535.
void H_XInputSetState(CPU *c) {
    uint32_t idx = ARG(0), vb = ARG(1);
    float lo = vb ? (float)(rd32(vb) & 0xFFFF) / 65535.0f : 0,
          hi = vb ? (float)((rd32(vb) >> 16) & 0xFFFF) / 65535.0f : 0;
    c->eax = host_pad_rumble((int)idx, lo, hi) ? 0 : 0x48F;
}
void H_MsgBox(CPU *c) {
    fprintf(stderr, "[ALERT] arg0=0x%x\n[ALERT] msg : %.600s\n[ALERT] arg2: %.200s\n[ALERT] flags=0x%x\n", ARG(0),
            gstr(ARG(1)), gstr(ARG(2)), ARG(3));
    c->eax = 0;
}

// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * Handle table: 4x larger (NCF/HSZ) + a "census" that logs who creates handles that are never released.
//  * USB watcher leak: IOServiceAddMatchingNotification / IOServiceGetMatchingServices now consume the matching
//    dictionary like the real calls (the leak filled the table after ~105,000 frames and aborted the game).
//  * Display sizes in real pixels (modes_px, modes_copy_all, CGDisplayBounds, CGDisplayPixelsHigh, mode getters):
//    lets a HiDPI 4K display offer 3840x2160 in the game's resolution menu.
//  * Fullscreen following (rt_game_fullscreen, fs_note, SetWindowBounds): the game's Fullscreen option now works.
//  * CGLDescribeRenderer: video-memory answers clamped to what a 32-bit game can hold.
// Background: a "handle" is a small 32-bit ticket the 32-bit game holds in place of a 64-bit CoreFoundation pointer.
// =====================================================================
// CoreFoundation bridge (guest handles <-> host CF objects), CoreGraphics display queries on the current API,
// and a Bink stub.
#include "rt.h"
#include "tables.h"
#include "game32.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#define S(n) ((char *)G2H(n))
#define RET(v) (c->eax = (uint32_t)(v))
// Display IDs shown to the guest are offset. Apple Silicon's main display is ID 1, and Aspyr's D3D wrapper uses 1 as an
// internal "same display as previous" sentinel (2013 Macs never had ID 1), which made it read garbage before its array.
#define DID_OFS 0x4000u
#define GDID(h) ((uint32_t)(h) + DID_OFS)
#define HDID(g) ((CGDirectDisplayID)((uint32_t)(g) >= DID_OFS ? (uint32_t)(g) - DID_OFS : (uint32_t)(g)))

// The original game's preference domain (rt32_game.prefs_domain), so settings are shared 1:1 with it.
static CFStringRef app_id(void) {
    static CFStringRef id;
    if (!id) id = CFStringCreateWithCString(NULL, rt32_game.prefs_domain ? rt32_game.prefs_domain : "local.recomp32", kCFStringEncodingUTF8);
    return id;
}

// ---------- handles ----------
// guest handle = 16-byte guest block {magic 'CFOB', index}; index into host table
// Handles are reference counted: CFRetain/CFRelease on the guest side adjust the count, and a handle whose count reaches
// zero releases the host object and goes back on a free list (its 16-byte guest block is reused). Host object -> slot
// lookups go through a hash table. (The old table grew forever and overflowed after 65536 handles.)
// Claude Fable - 2026-11-24: was 1<<18 handles / 1<<20 hash slots. Raised 4x as headroom; the real fix for the table filling
// up was the leak described at I__IOServiceAddMatchingNotification. HSZ stays 4x NCF so the hash table is never
// more than a quarter full.
#define NCF (1 << 20)
#define HSZ (1 << 22)
static CFTypeRef cfobj[NCF];
static uint32_t cfhandle[NCF];
static int32_t cfrc[NCF];
static int ncf = 1;
static int cffree[NCF], ncffree;
static int32_t cfhash[HSZ];                 // 0 empty, -1 tombstone, else slot index
static pthread_mutex_t cflock = PTHREAD_MUTEX_INITIALIZER;
extern const uint32_t gcfstr_lo, gcfstr_hi;
#define CF_MAGIC 0x43464F42u
#define CF_FREED 0x44455246u
static uint32_t cfh(CFTypeRef o) { uintptr_t x = (uintptr_t)o; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return (uint32_t)x & (HSZ - 1); }
static int cf_lookup(CFTypeRef o) {   // caller holds cflock
    for (uint32_t h = cfh(o), n = 0; n < HSZ; h = (h + 1) & (HSZ - 1), n++) {
        int32_t v = cfhash[h];
        if (v == 0) return 0;
        if (v > 0 && cfobj[v] == o) return v;
    }
    return 0;
}
static void cf_hash_put(CFTypeRef o, int i) {
    for (uint32_t h = cfh(o);; h = (h + 1) & (HSZ - 1)) if (cfhash[h] <= 0) { cfhash[h] = i; return; }
}
static void cf_hash_del(CFTypeRef o, int i) {
    for (uint32_t h = cfh(o), n = 0; n < HSZ; h = (h + 1) & (HSZ - 1), n++) {
        if (cfhash[h] == 0) return;
        if (cfhash[h] == i) { cfhash[h] = -1; return; }
    }
}
// ---- census (Claude Fable - 2026-11-24): the table filled after ~105k frames; report who creates never-released handles ----
// How to read the output: every 8192 brand-new slots (reused slots do not count) it prints
//   "new slots by guest caller": the game functions whose calls took those slots (identified by return address), and
//   "live objects by type":      what is sitting in the table right now, with one example of any type above 1000.
// A healthy game stops printing after loading. A line every few minutes means something leaks; the caller list says who.
// cf_callers is a tiny fixed hash table (128 entries, linear probing); it is cleared after each dump.
static struct { uint32_t ret, n; } cf_callers[128];
static void cf_census_dump(const char *why) {   // caller holds cflock
    fprintf(stderr, "[cf] census (%s): %d slots used, %d on the free list\n[cf]   new slots by guest caller:", why, ncf, ncffree);
    for (int k = 0; k < 8; k++) { int b = -1; for (int j = 0; j < 128; j++) if (cf_callers[j].n && (b < 0 || cf_callers[j].n > cf_callers[b].n)) b = j;
        if (b < 0) break; fprintf(stderr, " %s x%u", rt_func_name(cf_callers[b].ret), cf_callers[b].n); cf_callers[b].n = 0; }
    static struct { CFTypeID t; uint32_t n; CFTypeRef ex; } ty[32]; memset(ty, 0, sizeof ty);
    for (int i = 1; i < ncf; i++) if (cfobj[i]) { CFTypeID t = CFGetTypeID(cfobj[i]); int j = 0; while (j < 31 && ty[j].n && ty[j].t != t) j++; ty[j].t = t; ty[j].n++; ty[j].ex = cfobj[i]; }
    fprintf(stderr, "\n[cf]   live objects by type:");
    for (int j = 0; j < 32 && ty[j].n; j++) { CFStringRef d = CFCopyTypeIDDescription(ty[j].t); char b[64] = "?"; if (d) { CFStringGetCString(d, b, sizeof b, kCFStringEncodingUTF8); CFRelease(d); } fprintf(stderr, " %s x%u", b, ty[j].n); }
    fprintf(stderr, "\n");
    for (int j = 0; j < 32 && ty[j].n; j++) if (ty[j].n > 1000) { CFStringRef d = CFCopyDescription(ty[j].ex); char b[200] = "?"; if (d) { CFStringGetCString(d, b, sizeof b, kCFStringEncodingUTF8); CFRelease(d); } fprintf(stderr, "[cf]   example of a >1000 type: %.180s\n", b); }
    memset(cf_callers, 0, sizeof cf_callers);
}
// Called once per brand-new slot. rd32(esp) is the return address of the import call = the guest code that asked.
static void cf_census_note(void) {   // caller holds cflock; called when a brand-new slot is taken
    extern _Thread_local CPU *rt_cur; CPU *cc = rt_cur; uint32_t ret = cc ? rd32(cc->esp) : 0;
    int j = (int)((ret * 2654435761u) >> 25); for (int k = 0; k < 128; k++, j = (j + 1) & 127) { if (cf_callers[j].ret == ret || !cf_callers[j].n) { cf_callers[j].ret = ret; cf_callers[j].n++; break; } }
    if ((ncf & 8191) == 0) cf_census_dump("every 8192 new slots");
}
static uint32_t handle_for(CFTypeRef o) {   // takes ownership of one reference
    if (!o) return 0;
    pthread_mutex_lock(&cflock);
    int i = cf_lookup(o);
    if (i) { uint32_t h = cfhandle[i]; cfrc[i]++; pthread_mutex_unlock(&cflock); CFRelease(o); return h; }
    uint32_t g;
    if (ncffree) { i = cffree[--ncffree]; g = cfhandle[i]; }
    else {
        if (ncf >= NCF) { cf_census_dump("table full"); pthread_mutex_unlock(&cflock); fprintf(stderr, "[cf] handle table full (%d live CF objects)\n", NCF); abort(); }
        i = ncf++; g = gmalloc(16);
        cf_census_note();   /* Claude Fable - 2026-11-24: who grows the table */
    }
    wr32(g, CF_MAGIC); wr32(g + 4, (uint32_t)i);
    cfobj[i] = o; cfhandle[i] = g; cfrc[i] = 1;
    cf_hash_put(o, i);
    pthread_mutex_unlock(&cflock);
    return g;
}
static int cf_slot(uint32_t g) {   // valid live handle -> slot, else 0
    if (g < 0x1000 || g >= 0xF0000000u || rd32(g) != CF_MAGIC) return 0;
    uint32_t i = rd32(g + 4);
    return (i > 0 && i < (uint32_t)ncf && cfobj[i] && cfhandle[i] == g) ? (int)i : 0;
}
static void cf_retain_h(uint32_t g) { pthread_mutex_lock(&cflock); int i = cf_slot(g); if (i) cfrc[i]++; pthread_mutex_unlock(&cflock); }
static void cf_release_h(uint32_t g) {
    if (g >= gcfstr_lo && g < gcfstr_hi) return;   // compile-time CFSTR constants are never freed
    pthread_mutex_lock(&cflock);
    int i = cf_slot(g); CFTypeRef o = NULL;
    if (i && --cfrc[i] <= 0) {
        o = cfobj[i]; cf_hash_del(o, i); cfobj[i] = NULL; wr32(g, CF_FREED); cffree[ncffree++] = i;
    }
    pthread_mutex_unlock(&cflock);
    if (o) CFRelease(o);
}
static CFTypeRef cf(uint32_t g) {
    if (!g) return NULL;
    if (g >= gcfstr_lo && g < gcfstr_hi) {   // compile-time CFSTR("...") constant in the guest image
        static CFStringRef consts[64]; static uint32_t keys[64];
        int slot = (int)((g - gcfstr_lo) / 16);
        if (slot < 64 && keys[slot] == g) return consts[slot];
        CFStringRef s = CFStringCreateWithBytes(NULL, M + rd32(g + 8), rd32(g + 12), kCFStringEncodingUTF8, false);
        if (slot < 64) { consts[slot] = s; keys[slot] = g; }
        return s;
    }
    if (rd32(g) == CF_MAGIC) { uint32_t i = rd32(g + 4); if (i < (uint32_t)ncf && cfobj[i]) return cfobj[i]; }
    if (rd32(g) == CF_FREED) {
        extern _Thread_local CPU *rt_cur; CPU *cc = rt_cur;
        fprintf(stderr, "[cf] handle 0x%x used after CFRelease (guest caller %s)\n", g, cc ? rt_func_name(rd32(cc->esp)) : "?");
        return NULL;
    }
    { extern _Thread_local CPU *rt_cur; CPU *cc = rt_cur;
      fprintf(stderr, "[cf] unknown CF handle 0x%x (guest caller %s, word0=%08x word1=%08x)\n", g,
              cc ? rt_func_name(rd32(cc->esp)) : "?", g < 0xF0000000u ? rd32(g) : 0, g < 0xF0000000u ? rd32(g + 4) : 0); }
    return NULL;
}

// ---------- strings ----------
void I__CFStringCreateMutable(CPU *c) { RET(handle_for(CFStringCreateMutable(NULL, (CFIndex)ARG(1)))); }
void I__CFStringAppendCString(CPU *c) { CFStringAppendCString((CFMutableStringRef)cf(ARG(0)), S(ARG(1)), (CFStringEncoding)ARG(2)); }
void I__CFStringGetCString(CPU *c) {
    CFTypeRef o = cf(ARG(0));
    if (!o || CFGetTypeID(o) != CFStringGetTypeID()) { RET(0); return; }
    RET(CFStringGetCString((CFStringRef)o, S(ARG(1)), (CFIndex)ARG(2), (CFStringEncoding)ARG(3)));
}
void I__CFRelease(CPU *c) { cf_release_h(ARG(0)); }
void I__CFShow(CPU *c) { CFShow(cf(ARG(0))); }

// ---------- numbers / booleans / data ----------
void I__CFNumberGetValue(CPU *c) {
    CFNumberType t = (CFNumberType)ARG(1);
    if (t == kCFNumberLongType || t == kCFNumberSInt32Type || t == kCFNumberIntType) {
        int32_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt32Type, &v)); wr32(ARG(2), (uint32_t)v);
    } else if (t == kCFNumberSInt64Type || t == kCFNumberLongLongType) {
        int64_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt64Type, &v)); wr64(ARG(2), (uint64_t)v);
    } else if (t == kCFNumberFloatType || t == kCFNumberFloat32Type) {
        float v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberFloat32Type, &v)); wrf(ARG(2), v);
    } else if (t == kCFNumberDoubleType || t == kCFNumberFloat64Type) {
        double v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberFloat64Type, &v)); wrd(ARG(2), v);
    } else { int32_t v = 0; RET(CFNumberGetValue((CFNumberRef)cf(ARG(0)), kCFNumberSInt32Type, &v)); wr32(ARG(2), (uint32_t)v); }
}
void I__CFBooleanGetValue(CPU *c) { RET(CFBooleanGetValue((CFBooleanRef)cf(ARG(0)))); }
void I__CFDataGetBytes(CPU *c) {   // (data, CFRange{loc,len} by value, buf)
    CFDataGetBytes((CFDataRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), (UInt8 *)S(ARG(3)));
}

// ---------- arrays / dictionaries (values are guest handles) ----------
void I__CFArrayCreateMutable(CPU *c) { RET(handle_for(CFArrayCreateMutable(NULL, (CFIndex)ARG(1), &kCFTypeArrayCallBacks))); }
void I__CFArrayAppendValue(CPU *c) { CFArrayAppendValue((CFMutableArrayRef)cf(ARG(0)), cf(ARG(1))); }
void I__CFArrayGetCount(CPU *c) { RET(CFArrayGetCount((CFArrayRef)cf(ARG(0)))); }
void I__CFArrayGetValueAtIndex(CPU *c) {
    CFTypeRef v = CFArrayGetValueAtIndex((CFArrayRef)cf(ARG(0)), (CFIndex)ARG(1));
    RET(v ? handle_for(CFRetain(v)) : 0);
}
void I__CFDictionaryCreateMutable(CPU *c) {
    RET(handle_for(CFDictionaryCreateMutable(NULL, (CFIndex)ARG(1), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)));
}
void I__CFDictionarySetValue(CPU *c) { CFDictionarySetValue((CFMutableDictionaryRef)cf(ARG(0)), cf(ARG(1)), cf(ARG(2))); }
// A NULL dictionary would crash CoreFoundation; report the caller and key instead (a missing resource upstream)
static int null_dict(CPU *c, const char *fn) {
    if (cf(ARG(0))) return 0;
    char k[256] = "?"; CFTypeRef key = cf(ARG(1));
    if (key && CFGetTypeID(key) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)key, k, sizeof k, kCFStringEncodingUTF8);
    static int n; if (n++ < 20) fprintf(stderr, "[cf] %s on NULL dictionary 0x%x (key \"%s\", from %s)\n", fn, ARG(0), k, rt_func_name(rd32(c->esp)));
    return 1;
}
void I__CFDictionaryGetValue(CPU *c) {
    if (null_dict(c, "CFDictionaryGetValue")) { RET(0); return; }
    CFTypeRef v = CFDictionaryGetValue((CFDictionaryRef)cf(ARG(0)), cf(ARG(1)));
    RET(v ? handle_for(CFRetain(v)) : 0);
}

// ---------- preferences ----------
void I__CFPreferencesCopyAppValue(CPU *c) { RET(handle_for(CFPreferencesCopyAppValue((CFStringRef)cf(ARG(0)), app_id()))); }
void I__CFPreferencesSetAppValue(CPU *c) { CFPreferencesSetAppValue((CFStringRef)cf(ARG(0)), cf(ARG(1)), app_id()); }
void I__CFPreferencesAppSynchronize(CPU *c) { RET(CFPreferencesAppSynchronize(app_id())); }

// ---------- UUID ----------
void I__CFUUIDCreate(CPU *c) { RET(handle_for(CFUUIDCreate(NULL))); }
void I__CFUUIDCreateString(CPU *c) { RET(handle_for(CFUUIDCreateString(NULL, (CFUUIDRef)cf(ARG(1))))); }

// ---------- CoreGraphics bits that work natively ----------
void I__CGEventSourceKeyState(CPU *c) { RET(CGEventSourceKeyState((CGEventSourceStateID)ARG(0), (CGKeyCode)ARG(1))); }
void I__CGMainDisplayID(CPU *c) { RET(GDID(CGMainDisplayID())); }
void I__CGDisplayCurrentMode(CPU *c);
// Height of the display. In pixel mode this is the current mode's pixel height (2160 on a 4K HiDPI display), not the
// point height (1080) that CGDisplayPixelsHigh returns on a Retina-style desktop.
static int modes_px(void);
void I__CGDisplayPixelsHigh(CPU *c) {
    if (modes_px()) { CGDisplayModeRef m = CGDisplayCopyDisplayMode(HDID(ARG(0))); if (m) { size_t h = CGDisplayModeGetPixelHeight(m); CGDisplayModeRelease(m); RET((uint32_t)h); return; } }
    RET(CGDisplayPixelsHigh(HDID(ARG(0))));
}
// Claude Fable - 2026-11-24: follow the game's own Fullscreen option. game32_windowed() used to be a constant, so the option
// did nothing. Now the host (host.m followEngineFullscreen) reads rt_game_fullscreen each frame and toggles macOS
// fullscreen to match.
// Which call tells us? First guess was SetSystemUIMode / CGCaptureAllDisplays (how a 2013 Mac game normally goes
// fullscreen): wrong, this game never makes them in a way that distinguishes the two states. Measured signal: in
// Fullscreen the game moves its window to the screen's top-left corner (SetWindowBounds with top 0, left 0); in
// windowed mode it centres the window. I__SetWindowBounds below is therefore the call that actually drives this;
// the other callers of fs_note are kept because they are harmless and correct for games that do use them.
volatile int rt_game_fullscreen;
static void fs_note(const char *who, int fs) {
    if (fs != rt_game_fullscreen) fprintf(stderr, "[cf] game display mode -> %s (%s)\n", fs ? "FULLSCREEN" : "windowed", who);
    rt_game_fullscreen = fs;
}
void I__SetSystemUIMode(CPU *c) { fprintf(stderr, "[cf] SetSystemUIMode(mode %u, options 0x%x)\n", ARG(0), ARG(1)); fs_note("SetSystemUIMode", ARG(0) != 0); RET(0); }

// ---------- legacy CGDirectDisplay mode dictionaries (10.6 API) on top of CGDisplayMode ----------
static int modes_px(void);   // defined below: report sizes in real pixels and include HiDPI native modes (Claude Fable - 2026-11-24)
// The list of display modes handed to the game. kCGDisplayShowDuplicateLowResolutionModes makes macOS include the
// modes it normally hides on HiDPI displays (the full-pixel-resolution ones), which is where 3840x2160 comes from.
// The block in the middle only logs a one-time summary (⚠ its "16:9 sizes" list is formatted clumsily; cosmetic).
static CFArrayRef modes_copy_all(CGDirectDisplayID d) {
    CFDictionaryRef opt = NULL;
    if (modes_px()) { const void *k[1] = { kCGDisplayShowDuplicateLowResolutionModes }, *v[1] = { kCFBooleanTrue };
                      opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); }
    CFArrayRef a = CGDisplayCopyAllDisplayModes(d, opt);
    if (opt) CFRelease(opt);
    { static int n; if (n++ < 2) { long mw = 0, mh = 0, big = 0; char list[600]; size_t o = 0; list[0] = 0;
        for (CFIndex i = 0; a && i < CFArrayGetCount(a); i++) { CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(a, i);
            long w = (long)CGDisplayModeGetPixelWidth(m), h = (long)CGDisplayModeGetPixelHeight(m); if (w * h > mw * mh) { mw = w; mh = h; } if (w >= 3840) big++;
            if (w * 9 == h * 16 && o < sizeof list - 16 && !strstr(list, (snprintf(list + o, 0, ""), ""))) { char t[24]; snprintf(t, sizeof t, " %ldx%ld", w, h); if (!strstr(list, t)) o += (size_t)snprintf(list + o, sizeof list - o, "%s", t); } }
        fprintf(stderr, "[cf] display modes: %ld from macOS (%s), largest %ldx%ld px, %ld at 3840+ wide; 16:9 sizes:%s\n", a ? (long)CFArrayGetCount(a) : 0, modes_px() ? "reported in pixels" : "reported in points", mw, mh, big, list); } }
    return a;
}
static CFDictionaryRef mode_dict(CGDisplayModeRef m) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    long w = (long)(modes_px() ? CGDisplayModeGetPixelWidth(m) : CGDisplayModeGetWidth(m)), h = (long)(modes_px() ? CGDisplayModeGetPixelHeight(m) : CGDisplayModeGetHeight(m)), bpp = 32, io = (long)CGDisplayModeGetIOFlags(m);
    long mode = (long)CGDisplayModeGetIODisplayModeID(m);
    double hz = CGDisplayModeGetRefreshRate(m); if (hz <= 0) hz = 60;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberLongType, &w), nh = CFNumberCreate(NULL, kCFNumberLongType, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberLongType, &bpp), nr = CFNumberCreate(NULL, kCFNumberDoubleType, &hz);
    CFNumberRef ni = CFNumberCreate(NULL, kCFNumberLongType, &io), nm = CFNumberCreate(NULL, kCFNumberLongType, &mode);
    CFDictionarySetValue(d, CFSTR("Width"), nw); CFDictionarySetValue(d, CFSTR("Height"), nh);
    CFDictionarySetValue(d, CFSTR("BitsPerPixel"), nb); CFDictionarySetValue(d, CFSTR("RefreshRate"), nr);
    CFDictionarySetValue(d, CFSTR("IOFlags"), ni); CFDictionarySetValue(d, CFSTR("Mode"), nm);
    CFDictionarySetValue(d, CFSTR("UsableForDesktopGUI"), CGDisplayModeIsUsableForDesktopGUI(m) ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(d, CFSTR("kCGDisplayModeIsSafeForHardware"), kCFBooleanTrue);
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nr); CFRelease(ni); CFRelease(nm);
    return d;
}
void I__CGDisplayAvailableModes(CPU *c) {
    CFArrayRef modes = modes_copy_all(HDID(ARG(0)));   // legacy list: this is the one the game's resolution menu reads
    CFMutableArrayRef out = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CFDictionaryRef d = mode_dict((CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i));
        CFArrayAppendValue(out, d); CFRelease(d);
    }
    { static int once; if (!once++) { long mw = 0, mh = 0; for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) { CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
          long w = (long)(modes_px() ? CGDisplayModeGetPixelWidth(m) : CGDisplayModeGetWidth(m)), h = (long)(modes_px() ? CGDisplayModeGetPixelHeight(m) : CGDisplayModeGetHeight(m)); if (w * h > mw * mh) { mw = w; mh = h; } }
        fprintf(stderr, "[cf] CGDisplayAvailableModes: %ld modes reported to the game, largest %ldx%ld (%s)\n", modes ? (long)CFArrayGetCount(modes) : 0, mw, mh, modes_px() ? "pixels" : "points"); } }
    if (modes) CFRelease(modes);
    RET(handle_for(out));
}
void I__CGDisplayCurrentMode(CPU *c) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(HDID(ARG(0)));
    if (!m) { RET(0); return; }
    CFDictionaryRef d = mode_dict(m); CGDisplayModeRelease(m);
    RET(handle_for(d));
}
void I__CGDisplayBestModeForParameters(CPU *c) {   // (display, bpp, width, height, boolean_t *exactMatch)
    if (ARG(4)) wr32(ARG(4), 0);
    I__CGDisplayCurrentMode(c);
}
void I__CGDisplaySwitchToMode(CPU *c) { static int n; if (n++ < 20) fprintf(stderr, "[cf] CGDisplaySwitchToMode(display %u, mode 0x%x) ignored\n", ARG(0), ARG(1)); RET(0); }   // the port never changes the desktop resolution
void I__CGDisplayIDToOpenGLDisplayMask(CPU *c) { RET(CGDisplayIDToOpenGLDisplayMask(HDID(ARG(0)))); }
void I__CGCaptureAllDisplays(CPU *c) { fprintf(stderr, "[cf] CGCaptureAllDisplays\n"); fs_note("CGCaptureAllDisplays", 1); RET(0); }
void I__CGReleaseAllDisplays(CPU *c) { fprintf(stderr, "[cf] CGReleaseAllDisplays\n"); fs_note("CGReleaseAllDisplays", 0); RET(0); }
void I__CGDisplayMoveCursorToPoint(CPU *c) { RET(0); }

// CFArrayReplaceValues(array, CFRange{loc,len}, const void **newValues, CFIndex newCount)
void I__CFArrayReplaceValues(CPU *c) {
    CFIndex n = (CFIndex)(int32_t)ARG(4);
    const void **vals = n > 0 ? calloc((size_t)n, sizeof *vals) : NULL;
    for (CFIndex i = 0; i < n; i++) vals[i] = cf(rd32(ARG(3) + 4 * (uint32_t)i));
    CFArrayReplaceValues((CFMutableArrayRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), vals, n);
    free(vals);
}
// CFArraySortValues(array, CFRange, CFComparatorFunction (guest), void *context)
typedef struct { CPU *c; uint32_t fn, ctx; } SortCtx;
static CFComparisonResult guest_cmp(const void *a, const void *b, void *p) {
    SortCtx *s = p;
    uint32_t args[3] = { handle_for(CFRetain(a)), handle_for(CFRetain(b)), s->ctx };
    return (CFComparisonResult)(int32_t)guest_call(s->c, s->fn, 3, args);
}
void I__CFArraySortValues(CPU *c) {
    SortCtx s = { c, ARG(3), ARG(4) };
    CFArraySortValues((CFMutableArrayRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), guest_cmp, &s);
}

// ---------- Bink: see bink.c ----------

// ---- added for BioShock Infinite: core CoreFoundation surface used by the Aspyr wrapper ----
// Objects are host CF objects reached through guest handles (handle_for / cf above). CFRange and CFIndex are 32-bit in the
// guest, so a CFRange argument arrives as two stack words (loc, len) and a returned CFRange comes back in eax:edx.
#define CFR(a) CFRangeMake((CFIndex)(int32_t)ARG(a), (CFIndex)(int32_t)ARG((a) + 1))
void I__CFRetain(CPU *c) { cf_retain_h(ARG(0)); RET(ARG(0)); }
void I__CFMakeCollectable(CPU *c) { RET(ARG(0)); }
void I__CFAllocatorGetDefault(CPU *c) { RET(0); }
void I__CFGetTypeID(CPU *c) { CFTypeRef o = cf(ARG(0)); RET(o ? (uint32_t)CFGetTypeID(o) : 0); }
void I__CFStringGetTypeID(CPU *c) { RET((uint32_t)CFStringGetTypeID()); }
void I__CFArrayGetTypeID(CPU *c) { RET((uint32_t)CFArrayGetTypeID()); }
void I__CFDictionaryGetTypeID(CPU *c) { RET((uint32_t)CFDictionaryGetTypeID()); }
void I__CFNumberGetTypeID(CPU *c) { RET((uint32_t)CFNumberGetTypeID()); }
void I__CFDataGetTypeID(CPU *c) { RET((uint32_t)CFDataGetTypeID()); }

void I__CFStringCreateWithCString(CPU *c) { RET(handle_for(CFStringCreateWithCString(NULL, ARG(1) ? S(ARG(1)) : "", (CFStringEncoding)ARG(2)))); }
void I__CFStringCreateWithCharacters(CPU *c) { RET(handle_for(CFStringCreateWithCharacters(NULL, (const UniChar *)S(ARG(1)), (CFIndex)ARG(2)))); }
void I__CFStringCreateWithBytes(CPU *c) {   // (alloc, bytes, numBytes, encoding, isExternalRepresentation)
    RET(handle_for(CFStringCreateWithBytes(NULL, (const UInt8 *)S(ARG(1)), (CFIndex)ARG(2), (CFStringEncoding)ARG(3), (Boolean)(ARG(4) & 0xFF))));
}
void I__CFStringCreateCopy(CPU *c) { RET(handle_for(CFStringCreateCopy(NULL, (CFStringRef)cf(ARG(1))))); }
void I__CFStringCreateMutableCopy(CPU *c) { RET(handle_for(CFStringCreateMutableCopy(NULL, (CFIndex)ARG(1), (CFStringRef)cf(ARG(2))))); }
void I__CFStringGetLength(CPU *c) { CFTypeRef o = cf(ARG(0)); RET(o ? (uint32_t)CFStringGetLength((CFStringRef)o) : 0); }
void I__CFStringGetCharacters(CPU *c) {   // (str, CFRange, UniChar *buffer)
    // ⚠ Old CoreFoundation clamped an over-long range; modern macOS throws an NSException for it (the game asks for len+1 chars,
    // expecting the terminator). Clamp to the string and zero-fill the rest.
    CFStringRef str = (CFStringRef)cf(ARG(0)); CFRange r = CFR(1); CFIndex len = CFStringGetLength(str);
    UniChar *buf = (UniChar *)S(ARG(3));
    CFIndex avail = r.location >= len ? 0 : (r.location + r.length > len ? len - r.location : r.length);
    if (avail > 0) CFStringGetCharacters(str, CFRangeMake(r.location, avail), buf);
    for (CFIndex i = (avail > 0 ? avail : 0); i < r.length; i++) buf[i] = 0;
}
void I__CFStringGetBytes(CPU *c) {   // (str, CFRange, enc, lossByte, isExternalRep, buffer, maxBufLen, CFIndex *usedBufLen)
    CFIndex used = 0;
    CFIndex n = CFStringGetBytes((CFStringRef)cf(ARG(0)), CFR(1), (CFStringEncoding)ARG(3), (UInt8)(ARG(4) & 0xFF),
                                 (Boolean)(ARG(5) & 0xFF), ARG(6) ? (UInt8 *)S(ARG(6)) : NULL, (CFIndex)ARG(7), &used);
    if (ARG(8)) wr32(ARG(8), (uint32_t)used);
    RET(n);
}
void I__CFStringGetMaximumSizeForEncoding(CPU *c) { RET((uint32_t)CFStringGetMaximumSizeForEncoding((CFIndex)ARG(0), (CFStringEncoding)ARG(1))); }
void I__CFStringGetSystemEncoding(CPU *c) { RET(kCFStringEncodingMacRoman); }
void I__CFStringGetIntValue(CPU *c) { RET((uint32_t)CFStringGetIntValue((CFStringRef)cf(ARG(0)))); }
void I__CFStringCompare(CPU *c) { RET((uint32_t)CFStringCompare((CFStringRef)cf(ARG(0)), (CFStringRef)cf(ARG(1)), (CFStringCompareFlags)ARG(2))); }
void I__CFStringFind(CPU *c) {   // returns CFRange by value in eax:edx
    CFRange r = CFStringFind((CFStringRef)cf(ARG(0)), (CFStringRef)cf(ARG(1)), (CFStringCompareFlags)ARG(2));
    c->eax = (uint32_t)r.location; c->edx = (uint32_t)r.length;
}
void I__CFStringAppend(CPU *c) { CFStringAppend((CFMutableStringRef)cf(ARG(0)), (CFStringRef)cf(ARG(1))); }
void I__CFStringReplace(CPU *c) { CFStringReplace((CFMutableStringRef)cf(ARG(0)), CFR(1), (CFStringRef)cf(ARG(3))); }
void I__CFStringTrimWhitespace(CPU *c) { CFStringTrimWhitespace((CFMutableStringRef)cf(ARG(0))); }

void I__CFNumberCreate(CPU *c) {   // (alloc, type, const void *value); guest long is 32-bit
    CFNumberType t = (CFNumberType)ARG(1); uint32_t p = ARG(2);
    if (t == kCFNumberLongType || t == kCFNumberSInt32Type || t == kCFNumberIntType || t == kCFNumberCFIndexType || t == kCFNumberNSIntegerType) {
        int32_t v = (int32_t)rd32(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberSInt32Type, &v)));
    } else if (t == kCFNumberSInt64Type || t == kCFNumberLongLongType) {
        int64_t v = (int64_t)rd64(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberSInt64Type, &v)));
    } else if (t == kCFNumberFloatType || t == kCFNumberFloat32Type) {
        float v = rdf(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberFloat32Type, &v)));
    } else if (t == kCFNumberDoubleType || t == kCFNumberFloat64Type) {
        double v = rdd(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberFloat64Type, &v)));
    } else if (t == kCFNumberSInt8Type || t == kCFNumberCharType) {
        int8_t v = (int8_t)rd8(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberSInt8Type, &v)));
    } else if (t == kCFNumberSInt16Type || t == kCFNumberShortType) {
        int16_t v = (int16_t)rd16(p); RET(handle_for(CFNumberCreate(NULL, kCFNumberSInt16Type, &v)));
    } else { fprintf(stderr, "[cf] CFNumberCreate: unsupported type %d\n", (int)t); RET(0); }
}
void I__CFDataCreate(CPU *c) { RET(handle_for(CFDataCreate(NULL, (const UInt8 *)S(ARG(1)), (CFIndex)ARG(2)))); }
void I__CFDataGetLength(CPU *c) { CFTypeRef o = cf(ARG(0)); RET(o ? (uint32_t)CFDataGetLength((CFDataRef)o) : 0); }

void I__CFArrayCreate(CPU *c) {   // (alloc, const void **values (guest handles), count, callbacks)
    CFIndex n = (CFIndex)ARG(2); CFTypeRef *v = malloc((n ? n : 1) * sizeof *v);
    for (CFIndex i = 0; i < n; i++) v[i] = cf(rd32(ARG(1) + 4 * (uint32_t)i));
    CFArrayRef a = CFArrayCreate(NULL, (const void **)v, n, &kCFTypeArrayCallBacks); free(v);
    RET(handle_for(a));
}
void I__CFArraySetValueAtIndex(CPU *c) { CFArraySetValueAtIndex((CFMutableArrayRef)cf(ARG(0)), (CFIndex)ARG(1), cf(ARG(2))); }
void I__CFDictionaryCreate(CPU *c) {   // (alloc, keys**, values**, count, keyCallbacks, valueCallbacks)
    CFIndex n = (CFIndex)ARG(3); CFTypeRef *k = malloc((n ? n : 1) * sizeof *k), *v = malloc((n ? n : 1) * sizeof *v);
    for (CFIndex i = 0; i < n; i++) { k[i] = cf(rd32(ARG(1) + 4 * (uint32_t)i)); v[i] = cf(rd32(ARG(2) + 4 * (uint32_t)i)); }
    CFDictionaryRef d = CFDictionaryCreate(NULL, (const void **)k, (const void **)v, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    free(k); free(v);
    RET(handle_for(d));
}
void I__CFDictionaryAddValue(CPU *c) { CFDictionaryAddValue((CFMutableDictionaryRef)cf(ARG(0)), cf(ARG(1)), cf(ARG(2))); }
void I__CFDictionaryGetCount(CPU *c) { if (null_dict(c, "CFDictionaryGetCount")) { RET(0); return; } RET((uint32_t)CFDictionaryGetCount((CFDictionaryRef)cf(ARG(0)))); }
void I__CFDictionaryGetValueIfPresent(CPU *c) {   // (dict, key, const void **value)
    const void *v = NULL;
    Boolean ok = CFDictionaryGetValueIfPresent((CFDictionaryRef)cf(ARG(0)), cf(ARG(1)), &v);
    if (ok && ARG(2)) wr32(ARG(2), v ? handle_for(CFRetain(v)) : 0);
    RET(ok);
}
// ---- end additions ----

// ---------- CoreGraphics display/mouse batch (BioShock Infinite) ----------
// ⚠ Simple answers (single display, no mirroring, never captured); untested at runtime.
void I__CGDisplayIsInMirrorSet(CPU *c) { RET(0); }
void I__CGDisplayIsCaptured(CPU *c) { RET(0); }
void I__CGDisplayIOServicePort(CPU *c) { RET(0); }
void I__CGDisplayRegisterReconfigurationCallback(CPU *c) { RET(0); }
void I__CGDisplayRemoveReconfigurationCallback(CPU *c) { RET(0); }
void I__CGAcquireDisplayFadeReservation(CPU *c) { if (ARG(1)) wr32(ARG(1), 1); RET(0); }   // (seconds(double: 2 words)? -> token*)
void I__CGReleaseDisplayFadeReservation(CPU *c) { RET(0); }
void I__CGDisplayFade(CPU *c) { RET(0); }
void I__CGCursorIsVisible(CPU *c) { RET(1); }
volatile int rt_mouse_detached;   // read by the host: while set, it keeps the pointer inside the game window
void I__CGAssociateMouseAndMouseCursorPosition(CPU *c) {   // false while the game uses the mouse for looking
    static int last = -1; int on = ARG(0) & 1; rt_mouse_detached = !on;
    if (on != last) { fprintf(stderr, "[mouse] cursor %s\n", on ? "follows the mouse" : "detached (mouse look)"); last = on; }
    RET(CGAssociateMouseAndMouseCursorPosition(on));
}
void I__CGWarpMouseCursorPosition(CPU *c) { CGPoint p = { ARGF(0), ARGF(1) }; RET(CGWarpMouseCursorPosition(p)); }
void I__CGSetLocalEventsSuppressionInterval(CPU *c) { RET(0); }
void I__CGGetDisplayTransferByTable(CPU *c) { RET(1001); }     // kCGErrorFailure: no gamma tables
void I__CGSetDisplayTransferByTable(CPU *c) { RET(1001); }
// 32-bit CGRect = 4 floats, returned through a hidden pointer that the callee pops.
void I__CGDisplayBounds(CPU *c) {
    uint32_t out = ARG(0); CGRect r = CGDisplayBounds(HDID(ARG(1)));
    if (modes_px()) {   // Claude Fable - 2026-11-24: the game caps its resolution list at the desktop size it reads here; report pixels, not points
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(HDID(ARG(1)));
        if (m) { double k = r.size.width > 0 ? (double)CGDisplayModeGetPixelWidth(m) / r.size.width : 1; CGDisplayModeRelease(m);
                 if (k > 1.01) { r.origin.x *= k; r.origin.y *= k; r.size.width *= k; r.size.height *= k; }
                 static int once; if (!once++) fprintf(stderr, "[cf] CGDisplayBounds: reporting %gx%g (pixel scale %.2f)\n", r.size.width, r.size.height, k); }
    }
    float f[4] = { (float)r.origin.x, (float)r.origin.y, (float)r.size.width, (float)r.size.height };
    memcpy(G2H(out), f, 16); c->esp += 4; RET(out);
}
static void display_list(CPU *c) {   // (maxDisplays, CGDirectDisplayID *ids, uint32_t *count)
    CGDirectDisplayID ids[16]; uint32_t n = 0, max = ARG(0); if (max > 16) max = 16;
    CGGetActiveDisplayList(max, ids, &n);
    if (ARG(1)) for (uint32_t i = 0; i < n; i++) wr32(ARG(1) + 4 * i, GDID(ids[i]));
    if (ARG(2)) wr32(ARG(2), n);
    RET(0);
}
void I__CGGetActiveDisplayList(CPU *c) { display_list(c); }
void I__CGGetOnlineDisplayList(CPU *c) { display_list(c); }
void I__CGGetDisplaysWithOpenGLDisplayMask(CPU *c) {   // (mask, maxDisplays, ids*, count*)
    CGDirectDisplayID id = CGMainDisplayID();
    if (ARG(2)) wr32(ARG(2), GDID(id));
    if (ARG(3)) wr32(ARG(3), 1);
    RET(0);
}
void I__CGDisplayBestModeForParametersAndRefreshRate(CPU *c) {   // (display, bpp, w, h, refresh(double), boolean_t *exact)
    if (ARG(6)) wr32(ARG(6), 0);
    I__CGDisplayCurrentMode(c);
}

// ---------- CFPreferences getters, CFBundle, CFURL (BioShock Infinite) ----------
// ⚠ Untested at runtime. The "main bundle" is the user's real BioShock3.app (found via host_resources_dir()),
// not our recomp executable, so Info.plist keys and Resources lookups behave like the original.
void I__CFPreferencesGetAppIntegerValue(CPU *c) {   // (key, appID, Boolean *validFormat) -> CFIndex
    Boolean ok = false; CFIndex v = CFPreferencesGetAppIntegerValue((CFStringRef)cf(ARG(0)), app_id(), &ok);
    if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = ok; RET((uint32_t)v);
}
void I__CFPreferencesGetAppBooleanValue(CPU *c) {
    Boolean ok = false; Boolean v = CFPreferencesGetAppBooleanValue((CFStringRef)cf(ARG(0)), app_id(), &ok);
    if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = ok; RET(v ? 1 : 0);
}
static CFBundleRef game_bundle(void) {
    static CFBundleRef b;
    if (!b) {
        char p[2048]; snprintf(p, sizeof p, "%s", host_resources_dir());
        char *q = strstr(p, "/Contents/Resources"); if (q) *q = 0;
        CFStringRef s = CFStringCreateWithCString(NULL, p, kCFStringEncodingUTF8);
        CFURLRef u = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, true);
        b = CFBundleCreate(NULL, u); CFRelease(u); CFRelease(s);
    }
    return b;
}
static uint32_t hret(CFTypeRef o) { if (o) CFRetain(o); return handle_for(o); }   // for "Get" (unretained) results
void I__CFBundleGetMainBundle(CPU *c) { RET(hret(game_bundle())); }
void I__CFBundleCreate(CPU *c) { RET(hret(game_bundle())); }                     // (alloc, url): we only ever have the one bundle
void I__CFBundleCopyBundleURL(CPU *c) { RET(handle_for(CFBundleCopyBundleURL(game_bundle()))); }
void I__CFBundleCopyResourcesDirectoryURL(CPU *c) { RET(handle_for(CFBundleCopyResourcesDirectoryURL(game_bundle()))); }
void I__CFBundleGetIdentifier(CPU *c) { RET(hret(CFBundleGetIdentifier(game_bundle()))); }
void I__CFBundleGetDevelopmentRegion(CPU *c) { RET(hret(CFBundleGetDevelopmentRegion(game_bundle()))); }
void I__CFBundleGetValueForInfoDictionaryKey(CPU *c) { RET(hret(CFBundleGetValueForInfoDictionaryKey(game_bundle(), (CFStringRef)cf(ARG(1))))); }
void I__CFBundleCopyResourceURL(CPU *c) {   // (bundle, name, type, subDirName)
    RET(handle_for(CFBundleCopyResourceURL(game_bundle(), (CFStringRef)cf(ARG(1)), (CFStringRef)cf(ARG(2)), (CFStringRef)cf(ARG(3)))));
}
void I__CFURLCopyFileSystemPath(CPU *c) { RET(handle_for(CFURLCopyFileSystemPath((CFURLRef)cf(ARG(0)), (CFURLPathStyle)ARG(1)))); }
void I__CFURLGetFileSystemRepresentation(CPU *c) {   // (url, Boolean resolveAgainstBase, UInt8 *buf, CFIndex maxLen)
    RET(CFURLGetFileSystemRepresentation((CFURLRef)cf(ARG(0)), (ARG(1) & 0xFF) != 0, (UInt8 *)G2H(ARG(2)), (CFIndex)ARG(3)) ? 1 : 0);
}
void I__CFURLCreateWithFileSystemPath(CPU *c) {   // (alloc, path, style, isDirectory)
    RET(handle_for(CFURLCreateWithFileSystemPath(NULL, (CFStringRef)cf(ARG(1)), (CFURLPathStyle)ARG(2), (ARG(3) & 0xFF) != 0)));
}
void I__CFURLCreateWithString(CPU *c) { RET(handle_for(CFURLCreateWithString(NULL, (CFStringRef)cf(ARG(1)), (CFURLRef)cf(ARG(2))))); }
void I__CFURLCreateCopyAppendingPathComponent(CPU *c) {   // (alloc, url, component, isDirectory)
    RET(handle_for(CFURLCreateCopyAppendingPathComponent(NULL, (CFURLRef)cf(ARG(1)), (CFStringRef)cf(ARG(2)), (ARG(3) & 0xFF) != 0)));
}
void I__CFURLCreateCopyDeletingLastPathComponent(CPU *c) { RET(handle_for(CFURLCreateCopyDeletingLastPathComponent(NULL, (CFURLRef)cf(ARG(1))))); }
void I__CFURLGetString(CPU *c) { RET(hret(CFURLGetString((CFURLRef)cf(ARG(0))))); }

// ---------- CGRect helpers (32-bit CGRect = 4 floats; by-value args are 4 stack words, results via hidden ptr) ----------
// ⚠ Untested at runtime.
typedef struct { float x, y, w, h; } R32;
static R32 rarg(CPU *c, int slot) { R32 r = { ARGF(slot), ARGF(slot + 1), ARGF(slot + 2), ARGF(slot + 3) }; return r; }
static R32 rstd(R32 r) { if (r.w < 0) { r.x += r.w; r.w = -r.w; } if (r.h < 0) { r.y += r.h; r.h = -r.h; } return r; }
static void rret(CPU *c, R32 r) { uint32_t out = ARG(0); memcpy(G2H(out), &r, 16); c->esp += 4; RET(out); }
void I__CGRectGetMinX(CPU *c) { R32 r = rstd(rarg(c, 0)); FPUSH(r.x); }
void I__CGRectGetMaxX(CPU *c) { R32 r = rstd(rarg(c, 0)); FPUSH(r.x + r.w); }
void I__CGRectGetMinY(CPU *c) { R32 r = rstd(rarg(c, 0)); FPUSH(r.y); }
void I__CGRectGetMaxY(CPU *c) { R32 r = rstd(rarg(c, 0)); FPUSH(r.y + r.h); }
void I__CGRectContainsPoint(CPU *c) {   // (rect, point)
    R32 r = rstd(rarg(c, 0)); float px = ARGF(4), py = ARGF(5);
    RET((px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) ? 1 : 0);
}
void I__CGRectContainsRect(CPU *c) {
    R32 a = rstd(rarg(c, 0)), b = rstd(rarg(c, 4));
    RET((b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h) ? 1 : 0);
}
void I__CGRectUnion(CPU *c) {   // (hidden ret, r1, r2)
    R32 a = rstd(rarg(c, 1)), b = rstd(rarg(c, 5));
    float x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    float x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    R32 r = { x0, y0, x1 - x0, y1 - y0 }; rret(c, r);
}
void I__CGRectInset(CPU *c) {   // (hidden ret, rect, dx, dy)
    R32 r = rstd(rarg(c, 1)); float dx = ARGF(5), dy = ARGF(6);
    r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy; rret(c, r);
}
void I__CGRectOffset(CPU *c) {   // (hidden ret, rect, dx, dy)
    R32 r = rarg(c, 1); r.x += ARGF(5); r.y += ARGF(6); rret(c, r);
}

// ---------- Carbon window / process / time / event-target fakes (BioShock Infinite) ----------
// ⚠ Untested. The host (host.m) owns the real window and GL view; these only give the engine believable answers.
// Input is NOT delivered through Carbon events here: it will come through the game32_* hooks.
#include <time.h>
#include <sys/time.h>
#define FAKE_WINDOW   0x7100u
#define FAKE_TARGET   0x7200u
#define FAKE_QUEUE    0x7300u
// Installed Carbon handlers (see SendEventToEventTarget)
typedef struct { uint32_t target, handler, user, n; uint32_t types[32][2]; } CHandler;
static CHandler chand[32]; static int nchand;
static int16_t g_win_rect[4];   // top, left, bottom, right (Carbon Rect order, int16)
static void win_rect_default(void) { if (!g_win_rect[3]) { g_win_rect[2] = (int16_t)rt32_game.height; g_win_rect[3] = (int16_t)rt32_game.width; } }
void I__GetMBarHeight(CPU *c) { RET(0); }
void I__IsMenuBarVisible(CPU *c) { RET(1); }
void I__ShowMenuBar(CPU *c) { RET(0); }
void I__HideMenuBar(CPU *c) { RET(0); }
void I__TickCount(CPU *c) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); RET((uint32_t)(t.tv_sec * 60 + t.tv_nsec / 16666667)); }
void I__GetDateTime(CPU *c) { if (ARG(0)) wr32(ARG(0), (uint32_t)(time(NULL) + 2082844800u)); }
void I__UpdateSystemActivity(CPU *c) { RET(0); }
void I__GetCurrentProcess(CPU *c) { if (ARG(0)) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 2); } RET(0); }   // kCurrentProcess
void I__GetFrontProcess(CPU *c) { if (ARG(0)) { wr32(ARG(0), 0); wr32(ARG(0) + 4, 2); } RET(0); }
void I__SetFrontProcess(CPU *c) { RET(0); }
void I__GetProcessPID(CPU *c) { if (ARG(1)) wr32(ARG(1), (uint32_t)getpid()); RET(0); }
// Polled mouse state from the host window (engine coordinates, top-left origin; buttons 1 left, 2 right, 4 middle)
static volatile float mouse_lx, mouse_ly; static volatile uint32_t mouse_buttons;
void rt_mouse_state(float lx, float ly, uint32_t buttons) { mouse_lx = lx; mouse_ly = ly; mouse_buttons = buttons; }
void I__GetGlobalMouse(CPU *c) {   // (Point *out): QuickDraw Point {v, h} in global coordinates
    win_rect_default();
    if (ARG(0)) { wr16(ARG(0), (uint16_t)(int16_t)(g_win_rect[0] + (int)mouse_ly)); wr16(ARG(0) + 2, (uint16_t)(int16_t)(g_win_rect[1] + (int)mouse_lx)); }
}
static volatile uint8_t host_keymap[16];   // classic KeyMap: bit (code & 7) of byte (code >> 3), by macOS virtual key code
void I__GetKeys(CPU *c) { if (ARG(0)) { uint8_t k[16]; for (int i = 0; i < 16; i++) k[i] = host_keymap[i]; memcpy(G2H(ARG(0)), k, 16); } }
void I__GetCurrentButtonState(CPU *c) { RET(0); }
void I__GetCurrentEventButtonState(CPU *c) { RET(mouse_buttons); }
void I__LMGetKbdType(CPU *c) { RET(40); }
void I__LMGetKeyThresh(CPU *c) { RET(15); }
void I__LMGetKeyRepThresh(CPU *c) { RET(4); }
void I__GetCurrentEventTime(CPU *c) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); FPUSH((double)t.tv_sec + t.tv_nsec / 1e9); }
void I__GetEventTime(CPU *c) { I__GetCurrentEventTime(c); }
void I__GetApplicationEventTarget(CPU *c) { RET(FAKE_TARGET); }
void I__GetEventDispatcherTarget(CPU *c) { RET(FAKE_TARGET + 1); }
void I__GetWindowEventTarget(CPU *c) { RET(FAKE_TARGET + 2); }
void I__GetControlEventTarget(CPU *c) { RET(FAKE_TARGET + 3); }
void I__GetMainEventQueue(CPU *c) { RET(FAKE_QUEUE); }
void I__GetCurrentEventQueue(CPU *c) { RET(FAKE_QUEUE); }
void I__GetCurrentEventLoop(CPU *c) { RET(FAKE_QUEUE + 1); }
void I__InstallEventHandler(CPU *c) {   // (target, handler, numTypes, typeList, userData, EventHandlerRef *out) - handlers are recorded nowhere yet
    fprintf(stderr, "[carbon] InstallEventHandler(target 0x%x, handler %s, user 0x%x):", ARG(0), rt_func_name(ARG(1)), ARG(4));
    for (uint32_t i = 0; i < ARG(2) && i < 32; i++) { uint32_t cls = rd32(ARG(3) + 8 * i), kind = rd32(ARG(3) + 8 * i + 4);
        fprintf(stderr, " %c%c%c%c/%u", cls >> 24, (cls >> 16) & 255, (cls >> 8) & 255, cls & 255, kind); }
    fprintf(stderr, "\n");
    if (nchand < 32) { CHandler *h = &chand[nchand++]; h->target = ARG(0); h->handler = ARG(1); h->user = ARG(4); h->n = ARG(2) < 32 ? ARG(2) : 32;
        for (uint32_t i = 0; i < h->n; i++) { h->types[i][0] = rd32(ARG(3) + 8 * i); h->types[i][1] = rd32(ARG(3) + 8 * i + 4); } }
    if (ARG(5)) wr32(ARG(5), 0x7400u + (uint32_t)nchand); RET(0);
}
void I__RemoveEventHandler(CPU *c) { RET(0); }
void I__RemoveEventLoopTimer(CPU *c) { RET(0); }
void I__CreateNewWindow(CPU *c) {   // (class, attributes, const Rect *bounds, WindowRef *out)
    if (ARG(2)) memcpy(g_win_rect, G2H(ARG(2)), 8); win_rect_default();
    if (ARG(3)) wr32(ARG(3), FAKE_WINDOW); RET(0);
}
void I__DisposeWindow(CPU *c) {}
void I__ShowWindow(CPU *c) {}
void I__HideWindow(CPU *c) {}
void I__SelectWindow(CPU *c) {}
void I__SendBehind(CPU *c) {}
void I__IsWindowVisible(CPU *c) { RET(1); }
void I__IsWindowActive(CPU *c) { RET(1); }
void I__IsWindowCollapsed(CPU *c) { RET(0); }
void I__GetWindowBounds(CPU *c) {   // (window, region, Rect *out)
    win_rect_default(); if (ARG(2)) memcpy(G2H(ARG(2)), g_win_rect, 8); RET(0);
}
void I__SetWindowBounds(CPU *c) { if (ARG(2)) { memcpy(g_win_rect, G2H(ARG(2)), 8); static int n; if (n++ < 20) { int16_t r[4]; memcpy(r, G2H(ARG(2)), 8); fprintf(stderr, "[cf] SetWindowBounds: top %d left %d bottom %d right %d\n", r[0], r[1], r[2], r[3]); }
        // measured 02:33: in Fullscreen the wrapper puts the window at 0,0 (after CGDisplaySwitchToMode); windowed windows are centred
        { int16_t q[4]; memcpy(q, G2H(ARG(2)), 8); if (q[2] > q[0] && q[3] > q[1]) fs_note("SetWindowBounds", q[0] == 0 && q[1] == 0); } } RET(0); }
void I__SetWindowTitleWithCFString(CPU *c) { RET(0); }
// Window properties: games store their own per-window data here (BioShock Infinite keeps its Win32-style
// window handle under tag 'HWND', and maps Carbon events back to that handle).
typedef struct { uint32_t win, creator, tag, size; uint8_t data[64]; } WinProp;
static WinProp wprops[64]; static int nwprops;
static WinProp *wprop(uint32_t win, uint32_t creator, uint32_t tag, int create) {
    for (int i = 0; i < nwprops; i++) if (wprops[i].win == win && wprops[i].creator == creator && wprops[i].tag == tag) return &wprops[i];
    if (!create || nwprops >= 64) return NULL;
    WinProp *w = &wprops[nwprops++]; w->win = win; w->creator = creator; w->tag = tag; w->size = 0; return w;
}
void I__SetWindowProperty(CPU *c) {   // (window, creator, tag, size, const void *data)
    WinProp *w = wprop(ARG(0), ARG(1), ARG(2), 1); uint32_t n = ARG(3) > 64 ? 64 : ARG(3);
    if (!w) { RET((uint32_t)-108); return; }
    w->size = n; if (ARG(4)) memcpy(w->data, G2H(ARG(4)), n);
    fprintf(stderr, "[carbon] SetWindowProperty(win 0x%x, '%c%c%c%c', size %u, first word 0x%x)\n", ARG(0), ARG(2) >> 24, (ARG(2) >> 16) & 255,
            (ARG(2) >> 8) & 255, ARG(2) & 255, n, n >= 4 ? *(uint32_t *)w->data : 0);
    RET(0);
}
void I__GetWindowProperty(CPU *c) {   // (window, creator, tag, bufferSize, ByteCount *actualSize, void *out)
    WinProp *w = wprop(ARG(0), ARG(1), ARG(2), 0);
    if (!w) { RET((uint32_t)-5604); return; }   // errWindowPropertyNotFound
    uint32_t n = w->size < ARG(3) ? w->size : ARG(3);
    if (ARG(5)) memcpy(G2H(ARG(5)), w->data, n);
    if (ARG(4)) wr32(ARG(4), w->size);
    RET(0);
}
void I__SetWindowActivationScope(CPU *c) { RET(0); }
void I__SetWindowContentColor(CPU *c) { RET(0); }
void I__SetWindowModified(CPU *c) { RET(0); }
void I__SetWindowProxyCreatorAndType(CPU *c) { RET(0); }

// ---------- FSRef (legacy file references) on top of POSIX paths ----------
// ⚠ Untested. An FSRef is 80 opaque bytes in the guest; we store {magic, index into a path table}.
#include <sys/stat.h>
#define NFSPATH 8192
static char *fspaths[NFSPATH]; static int nfspaths = 1;
static void fsref_set(uint32_t addr, const char *path) {
    int i = 0;
    for (int k = 1; k < nfspaths; k++) if (!strcmp(fspaths[k], path)) { i = k; break; }
    if (!i) { if (nfspaths >= NFSPATH) { fprintf(stderr, "[fs] FSRef table full\n"); abort(); } i = nfspaths++; fspaths[i] = strdup(path); }
    memset(G2H(addr), 0, 80); wr32(addr, 0x46535246); wr32(addr + 4, (uint32_t)i);
}
static const char *fsref_path(uint32_t addr) {
    if (!addr || rd32(addr) != 0x46535246) return NULL;
    uint32_t i = rd32(addr + 4); return (i > 0 && i < (uint32_t)nfspaths) ? fspaths[i] : NULL;
}
static void mkdirs(const char *p) {
    char t[2048]; snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    mkdir(t, 0755);
}
void I__FSFindFolder(CPU *c) {   // (short vRefNum, OSType folderType, Boolean createFolder, FSRef *out)
    uint32_t t = ARG(1); int create = (ARG(2) & 0xFF) != 0;
    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    char p[2048];
    switch (t) {
        case 0x61737570: snprintf(p, sizeof p, "%s/Library/Application Support", home); break;   // 'asup'
        case 0x70726566: snprintf(p, sizeof p, "%s/Library/Preferences", home); break;           // 'pref'
        case 0x646F6373: snprintf(p, sizeof p, "%s/Documents", home); break;                     // 'docs'
        case 0x64657368: snprintf(p, sizeof p, "%s/Desktop", home); break;                       // 'desk'
        case 0x63616368: snprintf(p, sizeof p, "%s/Library/Caches", home); break;                // 'cach'
        case 0x6672616D: snprintf(p, sizeof p, "/System/Library/Frameworks"); break;               // 'fram'
        case 0x74656D70: snprintf(p, sizeof p, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"); break;   // 'temp'
        default: snprintf(p, sizeof p, "%s", home); fprintf(stderr, "[fs] FSFindFolder type 0x%08x -> home (guess)\n", t); break;
    }
    if (create) mkdirs(p);
    if (ARG(3)) fsref_set(ARG(3), p);
    RET(0);
}
void I__FSRefMakePath(CPU *c) {   // (const FSRef *, UInt8 *path, UInt32 maxPathSize)
    const char *p = fsref_path(ARG(0));
    if (!p) { RET((uint32_t)-43); return; }
    if (strlen(p) + 1 > ARG(2)) { RET((uint32_t)-50); return; }   // paramErr
    memcpy(G2H(ARG(1)), p, strlen(p) + 1); RET(0);
}
void I__FSPathMakeRef(CPU *c) {   // (const UInt8 *path, FSRef *out, Boolean *isDirectory)
    struct stat st; const char *p = (const char *)G2H(ARG(0));
    int sr = stat(p, &st);
    if (getenv("RT32_TRACE_FILES")) fprintf(stderr, "[file] FSPathMakeRef %s -> %d\n", p, sr);
    if (sr != 0) { RET((uint32_t)-43); return; }       // fnfErr
    fsref_set(ARG(1), p);
    if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = S_ISDIR(st.st_mode) ? 1 : 0;
    RET(0);
}
void I__FSMakeFSRefUnicode(CPU *c) {   // (const FSRef *parent, UniCharCount len, const UniChar *name, TextEncoding, FSRef *out)
    const char *par = fsref_path(ARG(0)); if (!par) { RET((uint32_t)-43); return; }
    uint32_t n = ARG(1); char name[1024]; uint32_t k = 0;
    for (uint32_t i = 0; i < n && k < sizeof name - 4; i++) {     // UTF-16 -> UTF-8 (BMP only)
        uint16_t u; memcpy(&u, M + ARG(2) + 2 * i, 2);
        if (u < 0x80) name[k++] = (char)u;
        else if (u < 0x800) { name[k++] = (char)(0xC0 | (u >> 6)); name[k++] = (char)(0x80 | (u & 0x3F)); }
        else { name[k++] = (char)(0xE0 | (u >> 12)); name[k++] = (char)(0x80 | ((u >> 6) & 0x3F)); name[k++] = (char)(0x80 | (u & 0x3F)); }
    }
    name[k] = 0;
    char p[2048]; snprintf(p, sizeof p, "%s/%s", par, name);
    struct stat st; if (stat(p, &st) != 0) { RET((uint32_t)-43); return; }
    fsref_set(ARG(4), p); RET(0);
}
void I__FSGetDataForkName(CPU *c) { if (ARG(0)) memset(G2H(ARG(0)), 0, 2); }   // empty HFSUniStr255 (data fork name is "")

// ---------- process bundle location, FSRef <-> CFURL, alias queries ----------
// ⚠ Untested.
static const char *app_bundle_path(void) {
    static char p[2048];
    if (!p[0]) { snprintf(p, sizeof p, "%s", host_resources_dir()); char *q = strstr(p, "/Contents/Resources"); if (q) *q = 0; }
    return p;
}
void I__GetProcessBundleLocation(CPU *c) {   // (const ProcessSerialNumber *, FSRef *location)
    if (ARG(1)) fsref_set(ARG(1), app_bundle_path()); RET(0);
}
void I__CFURLCreateFromFSRef(CPU *c) {   // (alloc, const FSRef *)
    const char *p = fsref_path(ARG(1)); if (!p) { RET(0); return; }
    struct stat st; int dir = (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    CFStringRef s = CFStringCreateWithCString(NULL, p, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, dir); CFRelease(s);
    RET(handle_for(u));
}
void I__CFURLGetFSRef(CPU *c) {   // (url, FSRef *out) -> Boolean
    char p[2048];
    if (!CFURLGetFileSystemRepresentation((CFURLRef)cf(ARG(0)), true, (UInt8 *)p, sizeof p)) { RET(0); return; }
    if (ARG(1)) fsref_set(ARG(1), p); RET(1);
}
void I__FSIsAliasFile(CPU *c) {   // (const FSRef *, Boolean *isAlias, Boolean *isFolder)
    const char *p = fsref_path(ARG(0)); struct stat st;
    if (!p || stat(p, &st) != 0) { RET((uint32_t)-43); return; }
    if (ARG(1)) *(uint8_t *)G2H(ARG(1)) = 0;
    if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = S_ISDIR(st.st_mode) ? 1 : 0;
    RET(0);
}
void I__FSResolveAliasFile(CPU *c) {   // (FSRef *, Boolean resolveChains, Boolean *targetIsFolder, Boolean *wasAliased)
    const char *p = fsref_path(ARG(0)); struct stat st;
    if (!p || stat(p, &st) != 0) { RET((uint32_t)-43); return; }
    if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = S_ISDIR(st.st_mode) ? 1 : 0;
    if (ARG(3)) *(uint8_t *)G2H(ARG(3)) = 0;
    RET(0);
}

// ---------- CFBundle function lookup (the game fetches libc entry points from System.framework by name) ----------
// ⚠ Untested. Only the names the game is known to ask for (fopen, open, unlink, stat$INODE64, mkdir, chdir) are mapped.
uint32_t rt_host_fn(gfn f);
void I__fopen(CPU *c); void I__open_UNIX2003(CPU *c); void I__unlink(CPU *c);
void I__stat_INODE64(CPU *c); void I__mkdir(CPU *c); void I__chdir(CPU *c);
void I__CFBundleLoadExecutable(CPU *c) { RET(1); }
void I__CFBundleGetFunctionPointerForName(CPU *c) {   // (bundle, CFStringRef name) -> void *
    char nm[128]; nm[0] = 0;
    CFStringGetCString((CFStringRef)cf(ARG(1)), nm, sizeof nm, kCFStringEncodingUTF8);
    const char *n = nm[0] == '_' ? nm + 1 : nm;
    static const struct { const char *name; gfn fn; } tab[] = {
        { "fopen", I__fopen }, { "open", I__open_UNIX2003 }, { "unlink", I__unlink },
        { "stat$INODE64", I__stat_INODE64 }, { "mkdir", I__mkdir }, { "chdir", I__chdir },
    };
    static uint32_t addr[sizeof tab / sizeof tab[0]];
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(n, tab[i].name)) { if (!addr[i]) addr[i] = rt_host_fn(tab[i].fn); RET(addr[i]); return; }
    fprintf(stderr, "[cf] CFBundleGetFunctionPointerForName: no mapping for \"%s\"\n", nm);
    RET(0);
}

// ---------- FSFork (legacy Carbon file I/O) on host file descriptors ----------
// ⚠ Untested. A fork ref number is simply the host fd (must stay < 32768). Only the data fork exists.
#include <fcntl.h>
#include <errno.h>
#define FS_EOF  (-39)
static void put16(uint32_t a, uint16_t v) { memcpy(G2H(a), &v, 2); }
static void put64(uint32_t a, int64_t v) { memcpy(G2H(a), &v, 8); }
static int fork_seek(int fd, uint32_t mode, int64_t off, int *ok) {   // positions the fd; mode: 0 atMark, 1 fromStart, 2 fromLEOF, 3 fromMark
    *ok = 1; mode &= 0xFFFF;
    if (mode == 0) return 1;
    off_t r = (mode == 1) ? lseek(fd, off, SEEK_SET) : (mode == 2) ? lseek(fd, off, SEEK_END) : lseek(fd, off, SEEK_CUR);
    if (r == (off_t)-1) *ok = 0; return 1;
}
static int32_t fs_errno(void) { return errno == ENOENT ? -43 : errno == EACCES || errno == EPERM ? -54 : errno == ENOSPC ? -34 : -36; }  // fnfErr, permErr, dskFulErr, ioErr
void I__FSOpenFork(CPU *c) {   // (const FSRef *, UniCharCount nameLen, const UniChar *name, SInt8 permissions, FSIORefNum *out)
    const char *p = fsref_path(ARG(0)); if (!p) { RET((uint32_t)-43); return; }
    int perm = (int8_t)(ARG(3) & 0xFF);
    int flags = (perm == 1) ? O_RDONLY : (perm == 2) ? O_WRONLY : O_RDWR;
    int fd = open(p, flags);
    if (fd < 0 && perm != 1 && errno == EACCES) fd = open(p, O_RDONLY);       // read-only game files opened read/write
    if (getenv("RT32_TRACE_FILES")) fprintf(stderr, "[file] FSOpenFork %s -> %d\n", p, fd);
    if (fd < 0) { fprintf(stderr, "[file] FSOpenFork %s failed: %s\n", p, strerror(errno)); RET((uint32_t)fs_errno()); return; }
    if (fd > 32767) { close(fd); RET((uint32_t)-36); return; }
    if (ARG(4)) put16(ARG(4), (uint16_t)fd);
    RET(0);
}
void I__FSCloseFork(CPU *c) { int fd = (int16_t)(ARG(0) & 0xFFFF); RET(close(fd) == 0 ? 0 : (uint32_t)fs_errno()); }
void I__FSFlushFork(CPU *c) { int fd = (int16_t)(ARG(0) & 0xFFFF); fsync(fd); RET(0); }
void I__FSGetForkSize(CPU *c) {   // (ref, SInt64 *size)
    struct stat st; int fd = (int16_t)(ARG(0) & 0xFFFF);
    if (fstat(fd, &st) != 0) { RET((uint32_t)fs_errno()); return; }
    if (ARG(1)) put64(ARG(1), (int64_t)st.st_size); RET(0);
}
void I__FSSetForkSize(CPU *c) {   // (ref, positionMode, SInt64 offset)
    int fd = (int16_t)(ARG(0) & 0xFFFF); uint32_t mode = ARG(1) & 0xFFFF; int64_t off = (int64_t)(((uint64_t)ARG(3) << 32) | ARG(2));
    struct stat st; if (fstat(fd, &st) != 0) { RET((uint32_t)fs_errno()); return; }
    int64_t n = mode == 1 ? off : mode == 2 ? st.st_size + off : lseek(fd, 0, SEEK_CUR) + off;
    RET(ftruncate(fd, n) == 0 ? 0 : (uint32_t)fs_errno());
}
void I__FSReadFork(CPU *c) {   // (ref, positionMode, SInt64 offset, ByteCount request, void *buf, ByteCount *actual)
    int fd = (int16_t)(ARG(0) & 0xFFFF); int64_t off = (int64_t)(((uint64_t)ARG(3) << 32) | ARG(2)); uint32_t req = ARG(4); int ok;
    fork_seek(fd, ARG(1), off, &ok);
    if (!ok) { if (ARG(6)) wr32(ARG(6), 0); RET((uint32_t)fs_errno()); return; }
    MC(ARG(5), req);
    { extern void rt_touch_write(uint32_t, uint32_t); rt_touch_write(ARG(5), req); }   // see gl.c: protected GL shadows
    uint8_t *buf = (uint8_t *)G2H(ARG(5)); uint32_t got = 0;
    extern volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes; uint64_t io_t0 = mach_absolute_time();
    while (got < req) { ssize_t r = read(fd, buf + got, req - got); if (r < 0) { if (ARG(6)) wr32(ARG(6), got); RET((uint32_t)fs_errno()); return; } if (r == 0) break; got += (uint32_t)r; }
    rt_hitch_io_ticks += mach_absolute_time() - io_t0; rt_hitch_io_bytes += got;
    if (ARG(6)) wr32(ARG(6), got);
    RET(got < req ? (uint32_t)FS_EOF : 0);
}
void I__FSWriteFork(CPU *c) {   // (ref, positionMode, SInt64 offset, ByteCount request, const void *buf, ByteCount *actual)
    int fd = (int16_t)(ARG(0) & 0xFFFF); int64_t off = (int64_t)(((uint64_t)ARG(3) << 32) | ARG(2)); uint32_t req = ARG(4); int ok;
    fork_seek(fd, ARG(1), off, &ok);
    if (!ok) { if (ARG(6)) wr32(ARG(6), 0); RET((uint32_t)fs_errno()); return; }
    const uint8_t *buf = (const uint8_t *)G2H(ARG(5)); uint32_t put = 0;
    while (put < req) { ssize_t r = write(fd, buf + put, req - put); if (r <= 0) break; put += (uint32_t)r; }
    if (ARG(6)) wr32(ARG(6), put);
    RET(put < req ? (uint32_t)fs_errno() : 0);
}

// ---------- CFTimeZone / absolute time / Gregorian date ----------
// ⚠ Untested. 32-bit CFGregorianDate = {int32 year; int8 month,day,hour,minute; double seconds} (16 bytes, same layout as arm64);
// CFGregorianUnits = {int32 years,months,days,hours,minutes; double seconds} is 28 bytes in the guest vs 32 on the host, so it is converted.
void I__CFAbsoluteTimeGetCurrent(CPU *c) { FPUSH(CFAbsoluteTimeGetCurrent()); }
void I__CFTimeZoneCopyDefault(CPU *c) { RET(handle_for(CFTimeZoneCopyDefault())); }
void I__CFTimeZoneGetSecondsFromGMT(CPU *c) {   // (tz, CFAbsoluteTime at) -> double
    FPUSH((double)CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)cf(ARG(0)), ARGD(1)));
}
void I__CFAbsoluteTimeGetGregorianDate(CPU *c) {   // (hidden ret, at, tz) -> CFGregorianDate by hidden pointer
    uint32_t out = ARG(0); CFGregorianDate d = CFAbsoluteTimeGetGregorianDate(ARGD(1), (CFTimeZoneRef)cf(ARG(3)));
    memcpy(G2H(out), &d, 16); c->esp += 4; RET(out);
}
void I__CFAbsoluteTimeGetDayOfWeek(CPU *c) { RET((uint32_t)CFAbsoluteTimeGetDayOfWeek(ARGD(0), (CFTimeZoneRef)cf(ARG(2)))); }
void I__CFGregorianDateGetAbsoluteTime(CPU *c) {   // (CFGregorianDate by value (4 words), tz) -> double
    CFGregorianDate d; memcpy(&d, M + c->esp + 4, 16);
    FPUSH(CFGregorianDateGetAbsoluteTime(d, (CFTimeZoneRef)cf(ARG(4))));
}
void I__CFAbsoluteTimeAddGregorianUnits(CPU *c) {   // (at, tz, CFGregorianUnits by value: 5 x int32 + double)
    CFGregorianUnits u; u.years = (int32_t)ARG(3); u.months = (int32_t)ARG(4); u.days = (int32_t)ARG(5);
    u.hours = (int32_t)ARG(6); u.minutes = (int32_t)ARG(7); u.seconds = ARGD(8);
    FPUSH(CFAbsoluteTimeAddGregorianUnits(ARGD(0), (CFTimeZoneRef)cf(ARG(2)), u));
}
void I__CFLocaleCreateCanonicalLanguageIdentifierFromString(CPU *c) {
    RET(handle_for(CFLocaleCreateCanonicalLanguageIdentifierFromString(NULL, (CFStringRef)cf(ARG(1)))));
}

// ---------- FSCreateFileUnicode / FSDeleteObject ----------
// ⚠ Untested. Catalog info (type/creator, dates, permissions) is ignored: an empty file is created.
static void u16_to_u8(uint32_t addr, uint32_t n, char *out, size_t cap) {
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 4 < cap; i++) {   // BMP only
        uint16_t u; memcpy(&u, M + addr + 2 * i, 2);
        if (u < 0x80) out[k++] = (char)u;
        else if (u < 0x800) { out[k++] = (char)(0xC0 | (u >> 6)); out[k++] = (char)(0x80 | (u & 0x3F)); }
        else { out[k++] = (char)(0xE0 | (u >> 12)); out[k++] = (char)(0x80 | ((u >> 6) & 0x3F)); out[k++] = (char)(0x80 | (u & 0x3F)); }
    }
    out[k] = 0;
}
void I__FSCreateFileUnicode(CPU *c) {   // (const FSRef *parent, UniCharCount len, const UniChar *name, bitmap, const FSCatalogInfo *, FSRef *newRef, FSSpec *newSpec)
    const char *par = fsref_path(ARG(0)); if (!par) { RET((uint32_t)-43); return; }
    char name[1024]; u16_to_u8(ARG(2), ARG(1), name, sizeof name);
    char p[2048]; snprintf(p, sizeof p, "%s/%s", par, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { RET((uint32_t)(errno == EEXIST ? -48 : (errno == EACCES ? -54 : -36))); return; }   // dupFNErr, permErr, ioErr
    close(fd);
    if (ARG(5)) fsref_set(ARG(5), p);
    RET(0);
}
void I__FSDeleteObject(CPU *c) {
    const char *p = fsref_path(ARG(0)); if (!p) { RET((uint32_t)-43); return; }
    struct stat st; if (stat(p, &st) != 0) { RET((uint32_t)-43); return; }
    int r = S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p);
    RET(r == 0 ? 0 : (uint32_t)(errno == EACCES ? -54 : -36));
}

// ---------- FSIterator (directory listing) and FSGetCatalogInfo ----------
// ⚠ Untested. FSCatalogInfo layout (144 bytes) and bitmap values are from memory of the Carbon headers; verify if sizes/dates/flags look wrong.
//   offsets: nodeFlags 0, volume 2, parentDirID 4, nodeID 8, sharingFlags 12, userPrivs 13, createDate 16, contentModDate 24,
//   attributeModDate 32, accessDate 40, backupDate 48, permissions 56 (uid, gid, ., access, ., mode@68), finderInfo 72, extFinderInfo 88,
//   dataLogicalSize 104, dataPhysicalSize 112, rsrcLogicalSize 120, rsrcPhysicalSize 128, valence 136, textEncodingHint 140.
#include <dirent.h>
static void put_utcdate(uint32_t a, time_t t) { uint64_t s = (uint64_t)t + 2082844800ull; wr16(a, (uint16_t)(s >> 32)); wr32(a + 2, (uint32_t)s); wr16(a + 6, 0); }
static void fill_catinfo(const char *path, const struct stat *st, uint32_t bm, uint32_t d) {
    memset(G2H(d), 0, 144);
    if (bm & 0x2)    wr16(d + 0, S_ISDIR(st->st_mode) ? 0x10 : 0);          // nodeFlags (kFSNodeIsDirectoryMask)
    if (bm & 0x4)    wr16(d + 2, 0);                                         // volume
    if (bm & 0x8)    wr32(d + 4, 2);                                          // parentDirID
    if (bm & 0x10)   wr32(d + 8, (uint32_t)st->st_ino);                       // nodeID
    if (bm & 0x20)   put_utcdate(d + 16, st->st_birthtime);
    if (bm & 0x40)   put_utcdate(d + 24, st->st_mtime);
    if (bm & 0x80)   put_utcdate(d + 32, st->st_ctime);
    if (bm & 0x100)  put_utcdate(d + 40, st->st_atime);
    if (bm & 0x400) { wr32(d + 56, st->st_uid); wr32(d + 60, st->st_gid); wr32(d + 68, st->st_mode); }
    if (bm & 0x2000) wr32(d + 136, 0);                                        // valence
    if (bm & 0x4000) { uint64_t z = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size; memcpy(G2H(d + 104), &z, 8); memcpy(G2H(d + 112), &z, 8); }
    (void)path;
}
static void put_unistr(uint32_t a, const char *utf8) {   // HFSUniStr255: UInt16 length + UniChar[255]
    uint16_t buf[255]; int n = 0; const unsigned char *p = (const unsigned char *)utf8;
    while (*p && n < 255) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { cp = '?'; p++; while ((*p & 0xC0) == 0x80) p++; }
        buf[n++] = (uint16_t)cp;
    }
    wr16(a, (uint16_t)n); memcpy(G2H(a + 2), buf, (size_t)n * 2);
}
void I__FSGetCatalogInfo(CPU *c) {   // (ref, whichInfo, FSCatalogInfo *, HFSUniStr255 *name, FSSpec *, FSRef *parent)
    const char *p = fsref_path(ARG(0)); struct stat st;
    int sr = p ? stat(p, &st) : -1;
    if (getenv("RT32_TRACE_FILES")) fprintf(stderr, "[file] FSGetCatalogInfo %s -> %d\n", p ? p : "(bad ref)", sr);
    if (sr != 0) { RET((uint32_t)-43); return; }
    if (ARG(2)) fill_catinfo(p, &st, ARG(1), ARG(2));
    if (ARG(3)) { const char *b = strrchr(p, '/'); put_unistr(ARG(3), b ? b + 1 : p); }
    if (ARG(5)) { char par[2048]; snprintf(par, sizeof par, "%s", p); char *s = strrchr(par, '/'); if (s && s != par) *s = 0; fsref_set(ARG(5), par); }
    RET(0);
}
void I__FSSetCatalogInfo(CPU *c) { RET(0); }   // dates/permissions are not changed
#define NITER 256
static struct { DIR *d; char path[2048]; } iters[NITER];
void I__FSOpenIterator(CPU *c) {   // (const FSRef *container, FSIteratorFlags, FSIterator *out)
    const char *p = fsref_path(ARG(0)); if (!p) { RET((uint32_t)-43); return; }
    for (int i = 1; i < NITER; i++) if (!iters[i].d) {
        iters[i].d = opendir(p); if (!iters[i].d) { RET((uint32_t)-43); return; }
        snprintf(iters[i].path, sizeof iters[i].path, "%s", p);
        if (getenv("RT32_TRACE_FILES")) fprintf(stderr, "[file] FSOpenIterator %s -> it %d\n", p, i);
        if (ARG(2)) wr32(ARG(2), 0x7500u + (uint32_t)i); RET(0); return;
    }
    RET((uint32_t)-108);   // memFullErr
}
void I__FSCloseIterator(CPU *c) {
    uint32_t i = ARG(0) - 0x7500u; if (i > 0 && i < NITER && iters[i].d) { closedir(iters[i].d); iters[i].d = NULL; } RET(0);
}
void I__FSGetCatalogInfoBulk(CPU *c) {   // (iter, maxObjects, ItemCount *actual, Boolean *changed, bitmap, FSCatalogInfo *infos, FSRef *refs, FSSpec *specs, HFSUniStr255 *names)
    uint32_t i = ARG(0) - 0x7500u; if (!(i > 0 && i < NITER && iters[i].d)) { RET((uint32_t)-50); return; }
    uint32_t max = ARG(1), n = 0; int eof = 0;
    if (ARG(3)) *(uint8_t *)G2H(ARG(3)) = 0;
    while (n < max) {
        struct dirent *e = readdir(iters[i].d);
        if (!e) { eof = 1; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", iters[i].path, e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (ARG(5)) fill_catinfo(p, &st, ARG(4), ARG(5) + 144 * n);
        if (ARG(6)) fsref_set(ARG(6) + 80 * n, p);
        if (ARG(8)) put_unistr(ARG(8) + 512 * n, e->d_name);
        n++;
    }
    if (getenv("RT32_TRACE_FILES")) fprintf(stderr, "[file] FSGetCatalogInfoBulk it %u max %u -> %u items%s (bitmap 0x%x info %x refs %x names %x)\n", i, max, n, eof ? " +eof" : "", ARG(4), ARG(5), ARG(6), ARG(8));
    if (ARG(2)) wr32(ARG(2), n);
    // Items found in this call are returned with noErr; errFSNoMoreItems only comes on an empty call. (Returning both
    // at once made callers that loop "while (err == noErr)" drop the last batch of every directory.)
    RET(eof && n == 0 ? (uint32_t)-1417 : 0);   // errFSNoMoreItems
}

// ---------- IOKit: power sources, registry, HID (BioShock Infinite) ----------
// ⚠ Untested. Everything answers "nothing here": no battery, no matching services, no HID devices (so no gamepad yet).
#define IORET_ERROR 0xE00002BCu
void I__IOPSCopyPowerSourcesInfo(CPU *c) { RET(0); }
void I__IOPSCopyPowerSourcesList(CPU *c) { RET(0); }
void I__IOPSGetPowerSourceDescription(CPU *c) { RET(0); }
void I__IOMasterPort(CPU *c) { if (ARG(1)) wr32(ARG(1), 0x7600u); RET(0); }
static uint32_t empty_dict(void) { return handle_for(CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)); }
void I__IOServiceMatching(CPU *c) { RET(empty_dict()); }
void I__IOServiceNameMatching(CPU *c) { RET(empty_dict()); }
void I__IOServiceGetMatchingServices(CPU *c) { cf_release_h(ARG(1)); if (ARG(2)) wr32(ARG(2), 0); RET(0); }   // empty iterator; like the real call, consumes one reference to the matching dictionary (Claude Fable - 2026-11-24: it leaked ~1.3 handles per frame and filled the table after ~105k frames)
void I__IOIteratorNext(CPU *c) { RET(0); }
void I__IOObjectRelease(CPU *c) { RET(0); }
void I__IOObjectConformsTo(CPU *c) { RET(0); }
void I__IORegistryEntryCreateCFProperty(CPU *c) { RET(0); }
void I__IORegistryEntryCreateCFProperties(CPU *c) { RET(IORET_ERROR); }
void I__IORegistryEntryFromPath(CPU *c) { RET(0); }
void I__IORegistryEntryGetChildEntry(CPU *c) { RET(IORET_ERROR); }
void I__IORegistryEntryGetParentEntry(CPU *c) { RET(IORET_ERROR); }
void I__IOServiceAddInterestNotification(CPU *c) { RET(IORET_ERROR); }
void I__IOServiceAddMatchingNotification(CPU *c) {   // (port, type, matching, callback, refcon, io_iterator_t *out)
    // Claude Fable - 2026-11-24: this returned an error, so the game's USB-device watcher (sub_5bd36f) retried forever, and each
    // retry leaked its matching dictionary (the real call consumes one reference). Now: consume the reference and report
    // "registered, nothing matched" (empty iterator), like a Mac with no such device plugged in.
    cf_release_h(ARG(2));
    if (ARG(5)) wr32(ARG(5), 0);
    RET(0);
}
void I__IONotificationPortCreate(CPU *c) { RET(0x7610u); }
void I__IONotificationPortDestroy(CPU *c) { }
void I__IONotificationPortGetRunLoopSource(CPU *c) { RET(0); }
void I__IOCreatePlugInInterfaceForService(CPU *c) { RET(IORET_ERROR); }
void I__IODestroyPlugInInterface(CPU *c) { RET(0); }
// ---------- IOHIDManager: keyboard from the host window as HID values ----------
// The host reports key presses (macOS virtual key codes); rt_hid_pump(), called on the game thread before
// each frame, hands them to the game's registered input-value callback as (usage page 7, usage, 1/0).
// IOHIDValueRef/IOHIDElementRef are small fake handles that index a ring of recent events.
static uint32_t hid_cb, hid_ctx, hid_mgr;
typedef struct { uint32_t page, usage; int32_t v; } HidEv;
static HidEv hid_ring[256]; static uint32_t hid_ring_n;
static HidEv hid_q[256]; static volatile uint32_t hid_q_head, hid_q_tail; static pthread_mutex_t hid_lock = PTHREAD_MUTEX_INITIALIZER;
#define HID_VAL 0x77000000u
#define HID_ELEM 0x78000000u
// macOS virtual key code -> USB HID keyboard usage (page 7)
static const uint8_t mac2hid[128] = {
    0x04,0x16,0x07,0x09,0x0B,0x0A,0x1D,0x1B,0x06,0x19,0x64,0x05,0x14,0x1A,0x08,0x15,   // 0x00 a s d f h g z x c v § b q w e r
    0x1C,0x17,0x1E,0x1F,0x20,0x21,0x23,0x22,0x2E,0x26,0x24,0x2D,0x25,0x27,0x30,0x12,   // 0x10 y t 1 2 3 4 6 5 = 9 7 - 8 0 ] o
    0x18,0x2F,0x0C,0x13,0x28,0x0F,0x0D,0x34,0x0E,0x33,0x31,0x36,0x38,0x11,0x10,0x37,   // 0x20 u [ i p ret l j ' k ; \ , / n m .
    0x2B,0x2C,0x35,0x2A,0x00,0x29,0x00,0xE3,0xE1,0x39,0xE2,0xE0,0xE5,0xE6,0xE4,0x00,   // 0x30 tab space ` bksp - esc - cmd shift caps opt ctrl rshift ropt rctrl fn
    0x6C,0x63,0x00,0x55,0x00,0x57,0x00,0x53,0x80,0x81,0x7F,0x54,0x58,0x00,0x56,0x6D,   // 0x40 f17 kp. - kp* - kp+ - numlock volup voldn mute kp/ kpenter - kp- f18
    0x6E,0x67,0x62,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,0x6F,0x60,0x61,0x00,0x00,0x00,   // 0x50 f19 kp= kp0..kp7 f20 kp8 kp9
    0x3E,0x3F,0x40,0x3C,0x41,0x42,0x00,0x44,0x68,0x46,0x6A,0x47,0x00,0x43,0x00,0x45,   // 0x60 f5 f6 f7 f3 f8 f9 - f11 f13 prtsc f16 scroll - f10 - f12
    0x00,0x48,0x49,0x4A,0x4B,0x4C,0x3D,0x4D,0x3B,0x4E,0x3A,0x50,0x4F,0x51,0x52,0x00,   // 0x70 - pause help/ins home pgup del f4 end f2 pgdn f1 left right down up
};
void rt_hid_key_event(uint16_t mac_keycode, int down) {   // host thread
    if (mac_keycode < 128) { if (down) host_keymap[mac_keycode >> 3] |= (uint8_t)(1u << (mac_keycode & 7)); else host_keymap[mac_keycode >> 3] &= (uint8_t)~(1u << (mac_keycode & 7)); }
    if (mac_keycode >= 128 || !mac2hid[mac_keycode]) return;
    pthread_mutex_lock(&hid_lock);
    uint32_t n = (hid_q_tail + 1) & 255;
    if (n != hid_q_head) { hid_q[hid_q_tail] = (HidEv){ 7, mac2hid[mac_keycode], down ? 1 : 0 }; hid_q_tail = n; }
    pthread_mutex_unlock(&hid_lock);
}
void rt_hid_pump(CPU *c) {   // game thread
    if (!hid_cb) return;
    for (;;) {
        pthread_mutex_lock(&hid_lock);
        if (hid_q_head == hid_q_tail) { pthread_mutex_unlock(&hid_lock); break; }
        HidEv e = hid_q[hid_q_head]; hid_q_head = (hid_q_head + 1) & 255;
        pthread_mutex_unlock(&hid_lock);
        uint32_t slot = hid_ring_n++ & 255; hid_ring[slot] = e;
        static int logged; if (getenv("RT32_HID_DEBUG") || logged++ < 3) fprintf(stderr, "[hid] key usage 0x%x %s\n", e.usage, e.v ? "down" : "up");
        uint32_t a[4] = { hid_ctx, 0, hid_mgr, HID_VAL | slot };
        guest_call(c, hid_cb, 4, a);
    }
}
void I__IOHIDManagerCreate(CPU *c) { static uint32_t n; fprintf(stderr, "[hid] IOHIDManagerCreate (from %s)\n", rt_func_name(rd32(c->esp))); RET(0x7620u + n++); }
void I__IOHIDManagerOpen(CPU *c) { RET(0); }
void I__IOHIDManagerClose(CPU *c) { RET(0); }
void I__IOHIDManagerRegisterInputValueCallback(CPU *c) {
    fprintf(stderr, "[hid] RegisterInputValueCallback(mgr 0x%x, cb %s, ctx 0x%x)\n", ARG(0), rt_func_name(ARG(1)), ARG(2));
    hid_mgr = ARG(0); hid_cb = ARG(1); hid_ctx = ARG(2);
}
void I__IOHIDManagerScheduleWithRunLoop(CPU *c) { }
void I__IOHIDManagerUnscheduleFromRunLoop(CPU *c) { }
void I__IOHIDManagerSetDeviceMatchingMultiple(CPU *c) { }
void I__IOHIDValueGetElement(CPU *c) { RET((ARG(0) & 0xFF000000u) == HID_VAL ? HID_ELEM | (ARG(0) & 255) : 0); }
void I__IOHIDElementGetUsage(CPU *c) { RET((ARG(0) & 0xFF000000u) == HID_ELEM ? hid_ring[ARG(0) & 255].usage : 0); }
void I__IOHIDElementGetUsagePage(CPU *c) { RET((ARG(0) & 0xFF000000u) == HID_ELEM ? hid_ring[ARG(0) & 255].page : 0); }
void I__IOHIDValueGetIntegerValue(CPU *c) {   // CFIndex (32-bit)
    RET((ARG(0) & 0xFF000000u) == HID_VAL ? (uint32_t)hid_ring[ARG(0) & 255].v : 0);
}

// ---------- CGL: the engine tries to create its own context; we hand it the host's (guest handle 1, see gl.c) ----------
// ⚠ Untested. Renderer info is passed through to the real GPU so VRAM/renderer queries see real values.
#include <OpenGL/OpenGL.h>
#define NREND 16
static CGLRendererInfoObj rendtab[NREND];
void I__CGLQueryRendererInfo(CPU *c) {   // (GLuint displayMask, CGLRendererInfoObj *rend, GLint *nrend)
    CGLRendererInfoObj info = NULL; GLint n = 0;
    CGLError e = CGLQueryRendererInfo(ARG(0), &info, &n);
    if (e != kCGLNoError) { RET((uint32_t)e); return; }
    for (int i = 1; i < NREND; i++) if (!rendtab[i]) { rendtab[i] = info; if (ARG(1)) wr32(ARG(1), 0x7700u + (uint32_t)i); if (ARG(2)) wr32(ARG(2), (uint32_t)n); RET(0); return; }
    CGLDestroyRendererInfo(info); RET((uint32_t)kCGLBadAlloc);
}
void I__CGLDescribeRenderer(CPU *c) {   // (rend, GLint rend_num, CGLRendererProperty, GLint *value)
    uint32_t i = ARG(0) - 0x7700u; GLint v = 0;
    if (!(i > 0 && i < NREND && rendtab[i])) { RET((uint32_t)kCGLBadRendererInfo); return; }
    CGLError e = CGLDescribeRenderer(rendtab[i], (GLint)ARG(1), (CGLRendererProperty)ARG(2), &v);
    // Claude Fable - 2026-11-24: video-memory answers from a modern GPU can overflow a 2013 32-bit game's arithmetic (bytes
    // above 2 GiB go negative in an int). Report at most 2047 MB, and never zero/negative. Logged twice per property.
    // NOTE: this was added while chasing blurry textures at 4K and turned out NOT to be the cause. macOS answered
    // 2147483647 bytes (already positive), and BioShock Infinite's texture budget does not come from this call at all:
    // the engine uses its own "detected video memory" figure (768 MB) unless told to read the ini. That fix lives in
    // the game project (game32.c, -ReadTexturePoolFromIni). The clamp is kept as a safety net for other games.
    GLint raw = v; CGLRendererProperty pr = (CGLRendererProperty)ARG(2);
    if (pr == kCGLRPVideoMemory || pr == kCGLRPTextureMemory) { if (e != kCGLNoError || v <= 0 || v > 2047 * 1048576) { v = 2047 * 1048576; e = kCGLNoError; } }
    else if (pr == kCGLRPVideoMemoryMegabytes || pr == kCGLRPTextureMemoryMegabytes) { if (e != kCGLNoError || v <= 0 || v > 2047) { v = 2047; e = kCGLNoError; } }
    { static int seen[512]; unsigned k = (unsigned)pr & 511; if (seen[k]++ < 2) fprintf(stderr, "[cf] CGLDescribeRenderer(renderer %d, property %d) -> macOS said %d, game gets %d (err %d)\n", (int)ARG(1), (int)pr, (int)raw, (int)v, (int)e); }
    if (ARG(3)) wr32(ARG(3), (uint32_t)v); RET((uint32_t)e);
}
void I__CGLDestroyRendererInfo(CPU *c) {
    uint32_t i = ARG(0) - 0x7700u; if (i > 0 && i < NREND && rendtab[i]) { CGLDestroyRendererInfo(rendtab[i]); rendtab[i] = NULL; } RET(0);
}
void I__CGLChoosePixelFormat(CPU *c) {
    { uint32_t a = ARG(0); fprintf(stderr, "[gl] CGLChoosePixelFormat attributes:");
      for (int i = 0; a && i < 64; i++) { uint32_t v = rd32(a + 4 * i); if (!v) break; fprintf(stderr, " %u", v); }
      fprintf(stderr, "\n"); } if (ARG(1)) wr32(ARG(1), 0x7710u); if (ARG(2)) wr32(ARG(2), 1); RET(0); }
void I__CGLDestroyPixelFormat(CPU *c) { RET(0); }
void I__CGLCreateContext(CPU *c) { if (ARG(2)) wr32(ARG(2), 1); RET(0); }   // the host's context is already current
void *host_cgl_context(void);
void I__CGLSetCurrentContext(CPU *c) {   // guest handle 1 == the host's context; bind it on this thread (the render thread)
    extern void host_note_gl_bind(void); if (ARG(0)) host_note_gl_bind();
    CGLError e = CGLSetCurrentContext(ARG(0) ? (CGLContextObj)host_cgl_context() : NULL);
    static int once; if (!once++) fprintf(stderr, "[gl] CGLSetCurrentContext(%u) on a guest thread -> %d\n", ARG(0), (int)e);
    RET(e);
}
void I__CGLDestroyContext(CPU *c) { RET(0); }
void I__CGLEnable(CPU *c) { RET(0); }
void I__CGLDisable(CPU *c) { RET(0); }
void I__CGLIsEnabled(CPU *c) {   // (ctx, cap, GLint *enable)
    GLint v = 0; CGLError e = CGLIsEnabled(CGLGetCurrentContext(), (CGLContextEnable)ARG(1), &v);
    if (ARG(2)) wr32(ARG(2), (uint32_t)v); RET((uint32_t)e);
}
void I__CGLSetOption(CPU *c) { RET(0); }

// ---------- CGDisplayMode (10.6+ API) passthrough: real host objects behind guest handles ----------
// Claude Fable - 2026-11-24: report display modes in real pixels (and include the native-resolution modes of HiDPI displays),
// so a 4K monitor offers 3840x2160 in the game's list. The engine's resolution is already treated as backing pixels
// by the host window code. RT32_MODES_POINTS=1 restores the old behaviour (sizes in points).
static int modes_px(void) { static int v = -1; if (v < 0) v = getenv("RT32_MODES_POINTS") == NULL; return v; }
void I__CGDisplayCopyAllDisplayModes(CPU *c) {
    RET(handle_for(modes_copy_all(HDID(ARG(0)))));
}
void I__CGDisplayModeGetWidth(CPU *c)  { CGDisplayModeRef m = (CGDisplayModeRef)cf(ARG(0)); RET((uint32_t)(modes_px() ? CGDisplayModeGetPixelWidth(m) : CGDisplayModeGetWidth(m))); }
void I__CGDisplayModeGetHeight(CPU *c) { CGDisplayModeRef m = (CGDisplayModeRef)cf(ARG(0)); RET((uint32_t)(modes_px() ? CGDisplayModeGetPixelHeight(m) : CGDisplayModeGetHeight(m))); }
void I__CGDisplayModeGetIOFlags(CPU *c) { RET(CGDisplayModeGetIOFlags((CGDisplayModeRef)cf(ARG(0)))); }
void I__CGDisplayModeGetRefreshRate(CPU *c) { FPUSH(CGDisplayModeGetRefreshRate((CGDisplayModeRef)cf(ARG(0)))); }
void I__CGDisplayModeCopyPixelEncoding(CPU *c) {   // ⚠ deprecated; modern macOS may return NULL or a different string than 10.7 did
    CFStringRef s = CGDisplayModeCopyPixelEncoding((CGDisplayModeRef)cf(ARG(0)));
    RET(s ? handle_for(s) : 0);
}
void I__CFArrayGetFirstIndexOfValue(CPU *c) {   // (array, CFRange{loc,len} by value, value)
    RET((uint32_t)CFArrayGetFirstIndexOfValue((CFArrayRef)cf(ARG(0)), CFRangeMake((CFIndex)(int32_t)ARG(1), (CFIndex)(int32_t)ARG(2)), cf(ARG(3))));
}
void I__CFDictionaryCreateMutableCopy(CPU *c) { RET(handle_for(CFDictionaryCreateMutableCopy(NULL, (CFIndex)ARG(1), (CFDictionaryRef)cf(ARG(2))))); }

// ---------- CFStringCreateWithFormat: printf-style formatting with %@ (CF object description) ----------
// Arguments are read from guest memory (i386 cdecl varargs: ints 4 bytes, long long / double 8 bytes).
typedef struct { char *p; size_t n, cap; } SB;
static void sb_put(SB *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void sb_cf(SB *b, CFTypeRef o) {
    if (!o) { sb_put(b, "(null)", 6); return; }
    CFStringRef d = CFGetTypeID(o) == CFStringGetTypeID() ? (CFStringRef)CFRetain(o) : CFCopyDescription(o);
    char tmp[4096]; if (!CFStringGetCString(d, tmp, sizeof tmp, kCFStringEncodingUTF8)) tmp[0] = 0;
    sb_put(b, tmp, strlen(tmp)); CFRelease(d);
}
static uint32_t cf_vformat(uint32_t fmt_h, uint32_t va) {
    char fmt[4096]; CFTypeRef fo = cf(fmt_h);
    if (!fo || !CFStringGetCString((CFStringRef)fo, fmt, sizeof fmt, kCFStringEncodingUTF8)) return 0;
    SB b = {0}; sb_put(&b, "", 0);
    for (const char *s = fmt; *s; ) {
        if (*s != '%') { const char *e = strchr(s, '%'); size_t n = e ? (size_t)(e - s) : strlen(s); sb_put(&b, s, n); s += n; continue; }
        const char *st = s++;
        if (*s == '%') { sb_put(&b, "%", 1); s++; continue; }
        while (*s && strchr("-+ #0'", *s)) s++;                          // flags
        if (*s == '*') { s++; } else while (*s >= '0' && *s <= '9') s++;  // width (* not supported: rare)
        if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }
        int ll = 0; char lenm[4] = {0}; int li = 0;
        while (*s && strchr("hlqLzjt", *s)) { if (*s == 'q' || (*s == 'l' && s[1] == 'l')) ll = 1; if (*s == 'j') ll = 1; if (li < 3) lenm[li++] = *s; s++; }
        char conv = *s ? *s++ : 0;
        char spec[64]; size_t sl = (size_t)(s - st); if (sl > 60) sl = 60;
        // rebuild spec without the guest length modifier; add the host one we need
        char base[64]; size_t bl = 0; for (const char *q = st; q < s - 1 && bl < 50; q++) if (!strchr("hlqLzjt", *q)) base[bl++] = *q; base[bl] = 0;
        char out[512]; int n = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (ll) { uint64_t v = (uint64_t)rd32(va) | ((uint64_t)rd32(va + 4) << 32); va += 8;
                      snprintf(spec, sizeof spec, "%sll%c", base, conv); n = snprintf(out, sizeof out, spec, v); }
            else { uint32_t v = rd32(va); va += 4;
                   if (conv == 'd' || conv == 'i') { snprintf(spec, sizeof spec, "%s%c", base, conv); n = snprintf(out, sizeof out, spec, (int32_t)v); }
                   else { snprintf(spec, sizeof spec, "%s%c", base, conv); n = snprintf(out, sizeof out, spec, v); } }
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v; uint64_t r = (uint64_t)rd32(va) | ((uint64_t)rd32(va + 4) << 32); memcpy(&v, &r, 8); va += 8;
            snprintf(spec, sizeof spec, "%s%c", base, conv); n = snprintf(out, sizeof out, spec, v); break; }
        case 's': { uint32_t p = rd32(va); va += 4; snprintf(spec, sizeof spec, "%ss", base);
                    n = snprintf(out, sizeof out, spec, p ? S(p) : "(null)"); break; }
        case 'p': { uint32_t p = rd32(va); va += 4; n = snprintf(out, sizeof out, "0x%x", p); break; }
        case '@': { uint32_t h = rd32(va); va += 4; sb_cf(&b, h ? cf(h) : NULL); n = 0; break; }
        case 'S': { uint32_t p = rd32(va); va += 4;   // UTF-16 string
                    if (p) { size_t k = 0; while (rd16(p + 2 * k)) k++;
                             CFStringRef t = CFStringCreateWithCharacters(NULL, (const UniChar *)G2H(p), (CFIndex)k); sb_cf(&b, t); CFRelease(t); }
                    break; }
        case 'C': { uint32_t u = rd32(va) & 0xFFFF; va += 4; UniChar ch = (UniChar)u;
                    CFStringRef t = CFStringCreateWithCharacters(NULL, &ch, 1); sb_cf(&b, t); CFRelease(t); break; }
        default: fprintf(stderr, "[cf] CFStringCreateWithFormat: unsupported conversion '%%%c' in \"%s\"\n", conv, fmt); va += 4; break;
        }
        if (n > 0) sb_put(&b, out, (size_t)n < sizeof out ? (size_t)n : sizeof out - 1);
    }
    CFStringRef r = CFStringCreateWithCString(NULL, b.p, kCFStringEncodingUTF8);
    free(b.p);
    return r ? handle_for(r) : 0;
}
void I__CFStringCreateWithFormat(CPU *c) { RET(cf_vformat(ARG(2), c->esp + 4 + 12)); }               // (alloc, options, format, ...)
void I__CFStringCreateWithFormatAndArguments(CPU *c) { RET(cf_vformat(ARG(2), ARG(3))); }          // (alloc, options, format, va_list)

// ---------- streams + property lists (settings / save metadata files) ----------
void I__CFReadStreamCreateWithFile(CPU *c) { RET(handle_for(CFReadStreamCreateWithFile(NULL, (CFURLRef)cf(ARG(1))))); }
void I__CFReadStreamOpen(CPU *c)  { RET(CFReadStreamOpen((CFReadStreamRef)cf(ARG(0)))); }
void I__CFReadStreamClose(CPU *c) { CFReadStreamClose((CFReadStreamRef)cf(ARG(0))); }
void I__CFWriteStreamCreateWithFile(CPU *c) { RET(handle_for(CFWriteStreamCreateWithFile(NULL, (CFURLRef)cf(ARG(1))))); }
void I__CFWriteStreamOpen(CPU *c)  { RET(CFWriteStreamOpen((CFWriteStreamRef)cf(ARG(0)))); }
void I__CFWriteStreamClose(CPU *c) { CFWriteStreamClose((CFWriteStreamRef)cf(ARG(0))); }
void I__CFPropertyListCreateWithStream(CPU *c) {   // (alloc, stream, len, options, CFPropertyListFormat *fmt, CFErrorRef *err)
    CFPropertyListFormat fmt = 0; CFErrorRef err = NULL;
    CFPropertyListRef p = CFPropertyListCreateWithStream(NULL, (CFReadStreamRef)cf(ARG(1)), (CFIndex)(int32_t)ARG(2), (CFOptionFlags)ARG(3), &fmt, &err);
    if (ARG(4)) wr32(ARG(4), (uint32_t)fmt);
    if (ARG(5)) wr32(ARG(5), err ? handle_for(err) : 0); else if (err) CFRelease(err);
    RET(p ? handle_for(p) : 0);
}
void I__CFPropertyListWrite(CPU *c) {   // (plist, stream, format, options, CFErrorRef *err) -> bytes written
    CFErrorRef err = NULL;
    CFIndex n = CFPropertyListWrite(cf(ARG(0)), (CFWriteStreamRef)cf(ARG(1)), (CFPropertyListFormat)ARG(2), (CFOptionFlags)ARG(3), &err);
    if (ARG(4)) wr32(ARG(4), err ? handle_for(err) : 0); else if (err) CFRelease(err);
    RET((uint32_t)n);
}
void I__CFPropertyListCreateFromXMLData(CPU *c) {   // (alloc, data, mutability, CFStringRef *errorString)
    CFStringRef es = NULL;
    CFPropertyListRef p = CFPropertyListCreateFromXMLData(NULL, (CFDataRef)cf(ARG(1)), (CFOptionFlags)ARG(2), &es);
    if (ARG(3)) wr32(ARG(3), es ? handle_for(es) : 0); else if (es) CFRelease(es);
    RET(p ? handle_for(p) : 0);
}
// CFDataGetBytePtr: host bytes live outside guest memory, so hand back a guest copy (refreshed on every call).
void I__CFDataGetBytePtr(CPU *c) {
    static uint32_t keys[256], bufs[256], caps[256]; static int nk;
    CFDataRef d = (CFDataRef)cf(ARG(0)); if (!d) { RET(0); return; }
    uint32_t len = (uint32_t)CFDataGetLength(d); int i;
    for (i = 0; i < nk; i++) if (keys[i] == ARG(0)) break;
    if (i == nk) { if (nk == 256) i = 0; else nk++; keys[i] = ARG(0); bufs[i] = 0; caps[i] = 0; }
    if (caps[i] < len + 1) { bufs[i] = gmalloc(len + 1); caps[i] = len + 1; }
    memcpy(G2H(bufs[i]), CFDataGetBytePtr(d), len); ((uint8_t *)G2H(bufs[i]))[len] = 0;
    RET(bufs[i]);
}
void I__CFStringGetCStringPtr(CPU *c) { RET(0); }   // allowed by the API: callers must fall back to CFStringGetCString
void I__CFStringConvertEncodingToNSStringEncoding(CPU *c) { RET(CFStringConvertEncodingToNSStringEncoding((CFStringEncoding)ARG(0))); }
void I__CFStringConvertNSStringEncodingToEncoding(CPU *c) { RET(CFStringConvertNSStringEncodingToEncoding((unsigned long)ARG(0))); }
void I__CFStringConvertEncodingToIANACharSetName(CPU *c) { RET(hret(CFStringConvertEncodingToIANACharSetName((CFStringEncoding)ARG(0)))); }
void I__CFStringConvertIANACharSetNameToEncoding(CPU *c) { RET(CFStringConvertIANACharSetNameToEncoding((CFStringRef)cf(ARG(0)))); }
void I__CFStringCreateWithPascalString(CPU *c) {   // (alloc, ConstStr255Param, encoding)
    uint32_t p = ARG(1); uint8_t n = p ? *(uint8_t *)G2H(p) : 0;
    RET(handle_for(CFStringCreateWithBytes(NULL, p ? (const UInt8 *)G2H(p + 1) : (const UInt8 *)"", n, (CFStringEncoding)ARG(2), false)));
}
void I__CFUUIDGetUUIDBytes(CPU *c) {   // CFUUIDBytes (16 bytes) returned through a hidden pointer the callee pops
    uint32_t out = ARG(0); CFUUIDBytes b = CFUUIDGetUUIDBytes((CFUUIDRef)cf(ARG(1)));
    memcpy(G2H(out), &b, 16); c->esp += 4; RET(out);
}
void I__CFUUIDGetConstantUUIDWithBytes(CPU *c) {   // (alloc, 16 bytes each promoted to an int)
    uint8_t b[16]; for (int i = 0; i < 16; i++) b[i] = (uint8_t)ARG(1 + i);
    RET(hret(CFUUIDGetConstantUUIDWithBytes(NULL, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15])));
}
void I__CFURLCreateWithFileSystemPathRelativeToBase(CPU *c) {   // (alloc, path, style, isDir, base)
    RET(handle_for(CFURLCreateWithFileSystemPathRelativeToBase(NULL, (CFStringRef)cf(ARG(1)), (CFURLPathStyle)ARG(2), (Boolean)ARG(3), (CFURLRef)cf(ARG(4)))));
}
void I__CFURLCreateStringByAddingPercentEscapes(CPU *c) {   // (alloc, str, leaveUnescaped, legalToEscape, encoding)
    RET(handle_for(CFURLCreateStringByAddingPercentEscapes(NULL, (CFStringRef)cf(ARG(1)), (CFStringRef)cf(ARG(2)), (CFStringRef)cf(ARG(3)), (CFStringEncoding)ARG(4))));
}
void I__CFURLCreateDataAndPropertiesFromResource(CPU *c) {   // (alloc, url, CFDataRef *data, CFDictionaryRef *props, desired, SInt32 *err)
    CFDataRef d = NULL; CFDictionaryRef pr = NULL; SInt32 e = 0;
    Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL, (CFURLRef)cf(ARG(1)), ARG(2) ? &d : NULL, ARG(3) ? &pr : NULL, (CFArrayRef)cf(ARG(4)), &e);
    if (ARG(2)) wr32(ARG(2), d ? handle_for(d) : 0);
    if (ARG(3)) wr32(ARG(3), pr ? handle_for(pr) : 0);
    if (ARG(5)) wr32(ARG(5), (uint32_t)e);
    RET(ok);
}
void I__CFURLWriteDataAndPropertiesToResource(CPU *c) {   // (url, data, props, SInt32 *err)
    SInt32 e = 0; Boolean ok = CFURLWriteDataAndPropertiesToResource((CFURLRef)cf(ARG(0)), (CFDataRef)cf(ARG(1)), (CFDictionaryRef)cf(ARG(2)), &e);
    if (ARG(3)) wr32(ARG(3), (uint32_t)e);
    RET(ok);
}
void I__CFDictionaryGetKeysAndValues(CPU *c) {   // (dict, const void **keys, const void **values): unretained ("Get")
    CFDictionaryRef d = (CFDictionaryRef)cf(ARG(0)); CFIndex n = d ? CFDictionaryGetCount(d) : 0;
    const void **k = calloc((size_t)n + 1, sizeof *k), **v = calloc((size_t)n + 1, sizeof *v);
    if (d) CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) wr32(ARG(1) + 4 * (uint32_t)i, hret(k[i]));
        if (ARG(2)) wr32(ARG(2) + 4 * (uint32_t)i, hret(v[i]));
    }
    free(k); free(v);
}
void I__CFBundleCopyLocalizedString(CPU *c) {   // (bundle, key, value, table)
    RET(handle_for(CFBundleCopyLocalizedString(game_bundle(), (CFStringRef)cf(ARG(1)), (CFStringRef)cf(ARG(2)), (CFStringRef)cf(ARG(3)))));
}
void I__CFBundleGetPackageInfo(CPU *c) {   // (bundle, UInt32 *type, UInt32 *creator)
    UInt32 t = 0, cr = 0; CFBundleGetPackageInfo(game_bundle(), &t, &cr);
    if (ARG(1)) wr32(ARG(1), t);
    if (ARG(2)) wr32(ARG(2), cr);
}

// ---------- Apply functions: call a guest callback per element (keys/values passed as unretained handles) ----------
void I__CFDictionaryApplyFunction(CPU *c) {   // (dict, applier(key, value, ctx), ctx)
    if (null_dict(c, "CFDictionaryApplyFunction")) return;
    CFDictionaryRef d = (CFDictionaryRef)cf(ARG(0)); uint32_t fn = ARG(1), ctx = ARG(2);
    if (!d || !fn) return;
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc((size_t)n + 1, sizeof *k), **v = calloc((size_t)n + 1, sizeof *v);
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) { uint32_t a[3] = { hret(k[i]), hret(v[i]), ctx }; guest_call(c, fn, 3, a); }
    free(k); free(v);
}
void I__CFArrayApplyFunction(CPU *c) {   // (array, CFRange{loc,len} by value, applier(value, ctx), ctx)
    CFArrayRef arr = (CFArrayRef)cf(ARG(0)); CFIndex loc = (CFIndex)(int32_t)ARG(1), len = (CFIndex)(int32_t)ARG(2);
    uint32_t fn = ARG(3), ctx = ARG(4);
    if (!arr || !fn) return;
    for (CFIndex i = loc; i < loc + len && i < CFArrayGetCount(arr); i++) {
        uint32_t a[2] = { hret(CFArrayGetValueAtIndex(arr, i)), ctx }; guest_call(c, fn, 2, a);
    }
}

// ---------- Aspyr app init (sub_2cd0) support ----------
// The game's nib (Carbon menus/dialogs) is optional: report "not found" and leave the out-ref 0, so it is skipped.
void I__CreateNibReferenceWithCFBundle(CPU *c) { if (ARG(2)) wr32(ARG(2), 0); RET((uint32_t)-1); }   // (bundle, name, IBNibRef *out)
void I__DisposeNibReference(CPU *c) { RET(0); }
void I__CFBundleCopyResourceURLForLocalization(CPU *c) {   // (bundle, name, type, subDir, localizationName)
    RET(handle_for(CFBundleCopyResourceURLForLocalization(game_bundle(), (CFStringRef)cf(ARG(1)), (CFStringRef)cf(ARG(2)),
                                                          (CFStringRef)cf(ARG(3)), (CFStringRef)cf(ARG(4)))));
}
// Run loops: handed to (faked) IOHIDManagerScheduleWithRunLoop and similar; real objects behind guest handles.
void I__CFRunLoopGetMain(CPU *c)    { RET(hret(CFRunLoopGetMain())); }
void I__CFRunLoopGetCurrent(CPU *c) { RET(hret(CFRunLoopGetCurrent())); }
// HIWindowCreate(class, const int *attrs, const HIWindowDef *def, HICoordinateSpace, const HIRect *bounds, WindowRef *out)
// The host owns the real window; the game gets the fake window ref. HIRect here is 4 floats (32-bit CGFloat).
void I__HIWindowCreate(CPU *c) {
    if (ARG(4)) {
        float f[4]; memcpy(f, G2H(ARG(4)), 16);
        fprintf(stderr, "[cf] HIWindowCreate: game asks for %gx%g at (%g,%g)\n", f[2], f[3], f[0], f[1]);
        int16_t r[4] = { (int16_t)f[1], (int16_t)f[0], (int16_t)(f[1] + f[3]), (int16_t)(f[0] + f[2]) };   // Rect: top,left,bottom,right
        memcpy(g_win_rect, r, 8);
    }
    win_rect_default();
    if (ARG(5)) wr32(ARG(5), FAKE_WINDOW);
    RET(0);
}

// ---------- Carbon windows / HIView / events / alerts: fakes on top of the host-owned window ----------
// ⚠ Untested. Event delivery goes through game32_* hooks, not Carbon, so Carbon event calls report "nothing".
#define FAKE_VIEW   0x7500u
#define FAKE_EVENT  0x7600u
#define FAKE_DIALOG 0x7800u
#define FAKE_TIMER  0x7900u
static void win_size(float *w, float *h) { win_rect_default(); *w = (float)(g_win_rect[3] - g_win_rect[1]); *h = (float)(g_win_rect[2] - g_win_rect[0]); }
void I__FrontNonFloatingWindow(CPU *c) { RET(FAKE_WINDOW); }
void I__GetNextWindow(CPU *c) { RET(0); }
void I__CopyWindowTitleAsCFString(CPU *c) { if (ARG(1)) wr32(ARG(1), handle_for(CFStringCreateWithCString(NULL, rt32_game.name, kCFStringEncodingUTF8))); RET(0); }
void I__InvalWindowRect(CPU *c) { RET(0); }
void I__ValidWindowRect(CPU *c) { RET(0); }
void I__HIWindowGetGreatestAreaDisplay(CPU *c) {   // (win, region, space, CGDirectDisplayID *out, HIRect *outRect)
    CGDirectDisplayID d = CGMainDisplayID(); CGRect r = CGDisplayBounds(d);
    if (ARG(3)) wr32(ARG(3), GDID(d));
    if (ARG(4)) { float f[4] = { (float)r.origin.x, (float)r.origin.y, (float)r.size.width, (float)r.size.height }; memcpy(G2H(ARG(4)), f, 16); }
    RET(0);
}
static void view_rect(uint32_t out) { if (!out) return; float w, h; win_size(&w, &h); float f[4] = { 0, 0, w, h }; memcpy(G2H(out), f, 16); }
void I__HIViewGetRoot(CPU *c) { RET(FAKE_VIEW); }
void I__HIViewFindByID(CPU *c) { if (ARG(3)) wr32(ARG(3), FAKE_VIEW + 1); RET(0); }   // (view, HIViewID{sig,id} by value, HIViewRef *out)
void I__HIViewGetWindow(CPU *c) { RET(FAKE_WINDOW); }
void I__HIViewGetBounds(CPU *c) { view_rect(ARG(1)); RET(0); }
void I__HIViewGetFrame(CPU *c)  { view_rect(ARG(1)); RET(0); }
void I__HIViewSetFrame(CPU *c) { RET(0); }
void I__HIViewSetBoundsOrigin(CPU *c) { RET(0); }
void I__HIViewSetVisible(CPU *c) { RET(0); }
void I__HIViewSetEnabled(CPU *c) { RET(0); }
void I__HIViewSetNeedsDisplay(CPU *c) { RET(0); }
void I__HIViewAddSubview(CPU *c) { RET(0); }
void I__HIViewSetID(CPU *c) { RET(0); }
void I__HIViewSetLayoutInfo(CPU *c) { RET(0); }
void I__HIViewGetLayoutInfo(CPU *c) { RET(0); }
void I__GetControlByID(CPU *c) { if (ARG(2)) wr32(ARG(2), 0); RET((uint32_t)-30598); }   // errUnknownControl
void I__GetControlBounds(CPU *c) { if (ARG(1)) memset(G2H(ARG(1)), 0, 8); RET(ARG(1)); }
void I__SetControlData(CPU *c) { RET(0); }
void I__SetControlID(CPU *c) { RET(0); }
void I__HIThemeDrawTextBox(CPU *c) { RET(0); }
void I__HIThemeGetTextDimensions(CPU *c) { RET(0); }
// events
void I__CreateEvent(CPU *c) { if (ARG(6)) wr32(ARG(6), FAKE_EVENT); RET(0); }   // (alloc, class, kind, double when (2 slots), attrs, EventRef *out)
void I__SetEventParameter(CPU *c) { RET(0); }
// ---------- Carbon event queue: key events from the host window ----------
// The host queues raw keyboard events (keyb/1 down, keyb/3 up); ReceiveNextEvent hands them out as fake
// EventRefs and GetEventParameter serves kcod (UInt32 keycode), kmod (UInt32 modifiers), kchr (1-byte char),
// wind (the fake window) and mloc (QD point 0,0). Games with a Win32-style pump convert events that
// SendEventToEventTarget reports as not handled, so that keeps returning eventNotHandledErr.
typedef struct { uint32_t cls, kind, kcod, kmod; uint8_t kchr;
                 float lx, ly, dx, dy; uint16_t btn, axis; uint32_t ccnt; int32_t wheel; } CEv;   // mouse: local pos, delta, button, wheel
static CEv cev_ring[256]; static uint32_t cev_ring_n;
static CEv cev_q[256]; static uint32_t cev_head, cev_tail; static pthread_mutex_t cev_lock = PTHREAD_MUTEX_INITIALIZER;
#define CEV_H 0x79000000u
void rt_carbon_key(uint16_t keycode, uint8_t ch, uint32_t mods, int down) {   // host thread
    pthread_mutex_lock(&cev_lock);
    uint32_t n = (cev_tail + 1) & 255;
    if (n != cev_head) { cev_q[cev_tail] = (CEv){ .cls = 'keyb', .kind = down ? 1u : 3u, .kcod = keycode, .kmod = mods, .kchr = ch }; cev_tail = n; }
    pthread_mutex_unlock(&cev_lock);
}
// Mouse: kind 1 down, 2 up, 5 moved, 6 dragged, 10 wheel. (lx, ly) in engine/window coordinates, top-left origin.
void rt_carbon_mouse(uint32_t kind, float lx, float ly, float dx, float dy, int button, int clicks, int wheel, uint32_t mods) {   // host thread
    pthread_mutex_lock(&cev_lock);
    if (kind == 5 || kind == 6) {   // coalesce moves: extend a queued move instead of flooding the queue
        uint32_t last = (cev_tail - 1) & 255;
        if (cev_head != cev_tail && cev_q[last].cls == 'mous' && cev_q[last].kind == kind) {
            cev_q[last].lx = lx; cev_q[last].ly = ly; cev_q[last].dx += dx; cev_q[last].dy += dy; pthread_mutex_unlock(&cev_lock); return; }
    }
    uint32_t n = (cev_tail + 1) & 255;
    if (n != cev_head) { cev_q[cev_tail] = (CEv){ .cls = 'mous', .kind = kind, .kmod = mods, .lx = lx, .ly = ly, .dx = dx, .dy = dy,
                                                  .btn = (uint16_t)(button + 1), .axis = 1, .ccnt = (uint32_t)clicks, .wheel = wheel }; cev_tail = n; }
    pthread_mutex_unlock(&cev_lock);
}
// Window activation (kEventWindowActivated 5 / Deactivated 6, FocusAcquired 200 / FocusRelinquish 201) for the
// game's window: games enable their mouse devices only while their window is active.
void rt_carbon_window(int active) {   // host thread
    pthread_mutex_lock(&cev_lock);
    uint32_t kinds[2] = { active ? 5u : 6u, active ? 200u : 201u };
    for (int i = 0; i < 2; i++) { uint32_t n = (cev_tail + 1) & 255; if (n == cev_head) break;
        cev_q[cev_tail] = (CEv){ .cls = 'wind', .kind = kinds[i] }; cev_tail = n; }
    pthread_mutex_unlock(&cev_lock);
    fprintf(stderr, "[carbon] window %s\n", active ? "activated" : "deactivated");
}
static CEv *cev_of(uint32_t h) { return (h & 0xFF000000u) == CEV_H ? &cev_ring[h & 255] : NULL; }
// Installed Carbon handlers: SendEventToEventTarget runs the ones for the event's class/kind
// (window target first, then application), stopping at the first that returns noErr.

void I__GetEventParameter(CPU *c) {   // (event, name, type, *actualType, size, *actualSize, *data)
    CEv *e = cev_of(ARG(0)); uint32_t name = ARG(1), size = ARG(4), out = ARG(6), type = 0, n = 0, v = 0;
    if (!e) { RET((uint32_t)-9870); return; }   // eventParameterNotFoundErr
    switch (name) {
    case 'kcod': type = 'magn'; n = 4; v = e->kcod; break;
    case 'kmod': type = 'magn'; n = 4; v = e->kmod; break;
    case 'kchr': type = 'TEXT'; n = 1; v = e->kchr; break;
    case 'wind': case '----': type = 'wind'; n = 4; v = FAKE_WINDOW; break;   // '----' (kEventParamDirectObject): the window a 'wind' event is about
    case 'mloc': { win_rect_default();   // QD Point {v, h}, global coordinates
        int16_t pv = (int16_t)(g_win_rect[0] + (int)e->ly), ph = (int16_t)(g_win_rect[1] + (int)e->lx);
        type = 'QDpt'; n = 4; v = (uint32_t)(uint16_t)pv | ((uint32_t)(uint16_t)ph << 16); break; }
    case 'wmou': case 'mdta': {   // HIPoint {float x, y}: window-relative location / delta
        float xy[2] = { name == 'wmou' ? e->lx : e->dx, name == 'wmou' ? e->ly : e->dy };
        if (ARG(3)) wr32(ARG(3), 'hipt'); if (ARG(5)) wr32(ARG(5), 8);
        if (out && size >= 8) { wrf(out, xy[0]); wrf(out + 4, xy[1]); }
        RET(0); return; }
    case 'mbtn': type = 'mbtn'; n = 2; v = e->btn; break;
    case 'mwax': type = 'mwax'; n = 2; v = e->axis; break;
    case 'ccnt': type = 'magn'; n = 4; v = e->ccnt; break;
    case 'mwdl': type = 'long'; n = 4; v = (uint32_t)e->wheel; break;
    default: { static int logged; if (name != 'QUED' && name != 'evnt' && logged++ < 8) fprintf(stderr, "[carbon] GetEventParameter %c%c%c%c not served\n", name >> 24, (name >> 16) & 255, (name >> 8) & 255, name & 255); }
        RET((uint32_t)-9870); return;
    }
    if (ARG(3)) wr32(ARG(3), type);
    if (ARG(5)) wr32(ARG(5), n);
    if (out) { if (n == 1) { if (size >= 1) wr8(out, (uint8_t)v); } else if (n == 2) { if (size >= 2) wr16(out, (uint16_t)v); } else if (size >= 4) wr32(out, v); }
    RET(0);
}
void I__GetEventClass(CPU *c) { CEv *e = cev_of(ARG(0)); RET(e ? e->cls : 0); }
void I__GetEventKind(CPU *c) { CEv *e = cev_of(ARG(0)); RET(e ? e->kind : 0); }
void I__ReleaseEvent(CPU *c) {
    extern uint32_t rt_msg_capture_ptr; CEv *e = cev_of(ARG(0));
    if (e && rt_msg_capture_ptr && getenv("RT32_MSG_CAPTURE")) {   // the converted Win32-style message for this event
        static int n; uint32_t m = rt_msg_capture_ptr;
        if (n++ < 400) fprintf(stderr, "[msg] %c%c%c%c/%u at (%.0f,%.0f) -> hwnd 0x%x msg 0x%x wParam 0x%x lParam 0x%x (x %d y %d)\n",
            e->cls >> 24, (e->cls >> 16) & 255, (e->cls >> 8) & 255, e->cls & 255, e->kind, e->lx, e->ly,
            rd32(m), rd32(m + 4), rd32(m + 8), rd32(m + 12), (int16_t)rd16(m + 12), (int16_t)rd16(m + 14));
        rt_msg_capture_ptr = 0;
    }
}
void I__GetEventRetainCount(CPU *c) { RET(1); }
void I__PostEventToQueue(CPU *c) { RET(0); }
void I__SendEventToEventTarget(CPU *c) {   // (event, target): our events go to matching installed handlers
    CEv *e = cev_of(ARG(0));
    if (e) {
        static const uint32_t order[2] = { 0x7202u, 0x7200u };   // window target (FAKE_TARGET + 2), then application
        for (int t = 0; t < 2; t++) for (int i = 0; i < nchand; i++) {
            CHandler *h = &chand[i]; if (h->target != order[t]) continue;
            int match = 0; for (uint32_t k = 0; k < h->n; k++) if (h->types[k][0] == e->cls && h->types[k][1] == e->kind) { match = 1; break; }
            if (!match) continue;
            uint32_t ev = ARG(0), a[3] = { 0x7A00u, ev, h->user };
            uint32_t r = guest_call(c, h->handler, 3, a);
            if (getenv("RT32_CARBON_DEBUG")) fprintf(stderr, "[carbon] %c%c%c%c/%u -> handler %s returned %d\n", e->cls >> 24, (e->cls >> 16) & 255,
                (e->cls >> 8) & 255, e->cls & 255, e->kind, rt_func_name(h->handler), (int)r);
            if (r == 0) { RET(0); return; }
        }
    }
    RET((uint32_t)-9874);   // eventNotHandledErr
}
void I__CallNextEventHandler(CPU *c) { RET((uint32_t)-9874); }
void I__ReceiveNextEvent(CPU *c) {   // (numTypes, list, double timeout (2 slots), pull, EventRef *out)
    uint32_t ntypes = ARG(0), list = ARG(1), pull = ARG(4) & 255, out = ARG(5);
    pthread_mutex_lock(&cev_lock);
    if (cev_head != cev_tail) {
        CEv e = cev_q[cev_head]; int ok = ntypes == 0;
        for (uint32_t i = 0; i < ntypes && !ok; i++) if (rd32(list + 8 * i) == e.cls && rd32(list + 8 * i + 4) == e.kind) ok = 1;
        if (ok) {
            if (pull) cev_head = (cev_head + 1) & 255;
            pthread_mutex_unlock(&cev_lock);
            uint32_t slot = cev_ring_n++ & 255; cev_ring[slot] = e;
            static int logged; if (getenv("RT32_CARBON_DEBUG") || logged++ < 4)
                fprintf(stderr, "[carbon] ReceiveNextEvent (from %s) -> %c%c%c%c/%u kcod %u kchr 0x%x\n", rt_func_name(rd32(c->esp)),
                        e.cls >> 24, (e.cls >> 16) & 255, (e.cls >> 8) & 255, e.cls & 255, e.kind, e.kcod, e.kchr);
            if (out) wr32(out, CEV_H | slot);
            RET(0); return;
        }
    }
    pthread_mutex_unlock(&cev_lock);
    if (out) wr32(out, 0);
    RET((uint32_t)-9875);   // eventLoopTimedOutErr
}
void I__InstallEventLoopTimer(CPU *c) {   // (loop, double delay, double interval, proc, userData, EventLoopTimerRef *out)
    static int once; if (!once++) fprintf(stderr, "[cf] InstallEventLoopTimer: proc 0x%x will NOT fire (timers not implemented)\n", ARG(5));
    if (ARG(7)) wr32(ARG(7), FAKE_TIMER);
    RET(0);
}
void I__QuitApplicationEventLoop(CPU *c) { (void)c; }
void I__RunApplicationEventLoop(CPU *c) { fprintf(stderr, "[cf] RunApplicationEventLoop called: the host owns the event loop\n"); abort(); }
void I__AEInstallEventHandler(CPU *c) { RET(0); }
// dialogs / alerts: log the text and answer "OK"
void I__GetStandardAlertDefaultParams(CPU *c) { RET(0); }
void I__CreateStandardAlert(CPU *c) {   // (type, CFString error, CFString explanation, params, DialogRef *out)
    char a[1024] = "", b[2048] = ""; CFTypeRef e = cf(ARG(1)), x = cf(ARG(2));
    if (e) CFStringGetCString((CFStringRef)e, a, sizeof a, kCFStringEncodingUTF8);
    if (x) CFStringGetCString((CFStringRef)x, b, sizeof b, kCFStringEncodingUTF8);
    fprintf(stderr, "[ALERT] %s | %s\n", a, b);
    if (ARG(4)) wr32(ARG(4), FAKE_DIALOG);
    RET(0);
}
void I__RunStandardAlert(CPU *c) { if (ARG(2)) wr16(ARG(2), 1); RET(0); }   // (dialog, filter, DialogItemIndex *outHit) -> default button
void I__GetDialogWindow(CPU *c) { RET(FAKE_WINDOW); }
void I__AutoSizeDialog(CPU *c) { RET(0); }
void I__RunAppModalLoopForWindow(CPU *c) { RET(0); }
void I__QuitAppModalLoopForWindow(CPU *c) { RET(0); }
void I__CreateWindowFromNib(CPU *c) { if (ARG(2)) wr32(ARG(2), 0); RET((uint32_t)-1); }

// ---------- CoreAudio HAL queries (pass-through) + a silent fake AUGraph (Wwise output) ----------
// ⚠ No sound yet: the graph "starts" but the game's render callback is never called. Real output needs the callback
// run on a guest CPU from CoreAudio's thread; that comes later.
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
static int ca_is_cf(AudioObjectPropertySelector s) {   // properties whose value is a CFStringRef (pointer-sized on the host)
    return s == kAudioObjectPropertyName || s == kAudioObjectPropertyManufacturer || s == kAudioDevicePropertyDeviceUID ||
           s == kAudioDevicePropertyModelUID || s == kAudioObjectPropertyElementName || s == kAudioDevicePropertyConfigurationApplication;
}
static AudioObjectPropertyAddress ca_addr(uint32_t g) { AudioObjectPropertyAddress a; memcpy(&a, G2H(g), 12); return a; }
void I__AudioObjectGetPropertyDataSize(CPU *c) {   // (obj, addr*, qualSize, qual*, UInt32 *outSize)
    AudioObjectPropertyAddress a = ca_addr(ARG(1)); UInt32 n = 0;
    OSStatus r = AudioObjectGetPropertyDataSize(ARG(0), &a, ARG(2), ARG(3) ? G2H(ARG(3)) : NULL, &n);
    if (ca_is_cf(a.mSelector)) n = 4;
    if (ARG(4)) wr32(ARG(4), n);
    RET(r);
}
void I__AudioObjectGetPropertyData(CPU *c) {   // (obj, addr*, qualSize, qual*, UInt32 *ioSize, void *out)
    AudioObjectPropertyAddress a = ca_addr(ARG(1)); UInt32 n = ARG(4) ? rd32(ARG(4)) : 0; OSStatus r;
    if (ca_is_cf(a.mSelector)) {
        CFStringRef s = NULL; UInt32 hn = sizeof s;
        r = AudioObjectGetPropertyData(ARG(0), &a, ARG(2), ARG(3) ? G2H(ARG(3)) : NULL, &hn, &s);
        if (ARG(5)) wr32(ARG(5), (r == 0 && s) ? handle_for(s) : 0);
        if (ARG(4)) wr32(ARG(4), 4);
    } else {
        r = AudioObjectGetPropertyData(ARG(0), &a, ARG(2), ARG(3) ? G2H(ARG(3)) : NULL, &n, ARG(5) ? G2H(ARG(5)) : NULL);
        if (ARG(4)) wr32(ARG(4), n);
    }
    if (r) { static int k; if (k++ < 10) fprintf(stderr, "[audio] AudioObjectGetPropertyData(obj %u, '%c%c%c%c' scope %u el %u) -> %d\n", ARG(0),
        a.mSelector >> 24, (a.mSelector >> 16) & 255, (a.mSelector >> 8) & 255, a.mSelector & 255, (unsigned)a.mScope, (unsigned)a.mElement, (int)r); }
    RET(r);
}
void I__AudioObjectSetPropertyData(CPU *c) {   // (obj, addr*, qualSize, qual*, dataSize, data*): let the game set buffer sizes etc.
    AudioObjectPropertyAddress a = ca_addr(ARG(1));
    if (ca_is_cf(a.mSelector)) { RET(0); return; }
    if (a.mSelector == kAudioHardwarePropertyRunLoop) {   // CFRunLoopRef is pointer-sized on the host; NULL = the HAL's own thread
        CFRunLoopRef rl = NULL; uint32_t h = ARG(5) ? rd32(ARG(5)) : 0;
        if (h) rl = (CFRunLoopRef)cf(h);
        RET(AudioObjectSetPropertyData(ARG(0), &a, 0, NULL, sizeof rl, &rl)); return;
    }
    OSStatus r = AudioObjectSetPropertyData(ARG(0), &a, ARG(2), ARG(3) ? G2H(ARG(3)) : NULL, ARG(4), ARG(5) ? G2H(ARG(5)) : NULL);
    { static int k; if (k++ < 10) fprintf(stderr, "[audio] AudioObjectSetPropertyData(obj %u, '%c%c%c%c' scope %u, %u bytes, first word %u) -> %d\n", ARG(0),
        a.mSelector >> 24, (a.mSelector >> 16) & 255, (a.mSelector >> 8) & 255, a.mSelector & 255, (unsigned)a.mScope, ARG(4), ARG(5) ? rd32(ARG(5)) : 0, (int)r); }
    RET(r);
}
void I__AudioDeviceSetProperty(CPU *c) { RET(0); }   // deprecated HAL call; accepted and ignored
static uint32_t aug_next = 1;
void I__NewAUGraph(CPU *c) { if (ARG(0)) wr32(ARG(0), 0x7A000000u); RET(0); }
void I__DisposeAUGraph(CPU *c) { RET(0); }
void I__AUGraphAddNode(CPU *c) { if (ARG(2)) wr32(ARG(2), aug_next++); RET(0); }   // (graph, desc*, AUNode *out)
void I__AUGraphNodeInfo(CPU *c) {   // (graph, node, AudioComponentDescription *outDesc, AudioUnit *outUnit)
    if (ARG(2)) memset(G2H(ARG(2)), 0, 20);
    if (ARG(3)) wr32(ARG(3), 0x7A100000u + ARG(1));
    RET(0);
}
void I__AUGraphOpen(CPU *c) { RET(0); }
void I__AUGraphClose(CPU *c) { RET(0); }
void I__AUGraphInitialize(CPU *c) { RET(0); }
// The game's mixer (Wwise) feeds its graph through a render callback. A real default-output AudioUnit
// stands in for the whole graph: CoreAudio's IO thread calls ca_render, which runs the guest callback on a
// guest CPU of its own with guest-memory buffers in the 32-bit AudioBufferList layout, then copies the
// samples out. The stream format is the one the game set on its units (or float32 stereo 48 kHz).
#include <AudioToolbox/AudioToolbox.h>
static uint32_t ca_proc, ca_ref;
static AudioStreamBasicDescription ca_fmt; static int ca_have_fmt;
static AudioUnit ca_out;
static OSStatus ca_render(void *refcon, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)refcon; (void)bus;
    static CPU *ac; static uint32_t g_flags, g_ts, g_abl, g_data, g_cap;
    uint32_t nb = io->mNumberBuffers, bytes = io->mBuffers[0].mDataByteSize;
    if (!ac) { ac = cpu_new(1u << 20); g_flags = gmalloc(16); g_ts = gmalloc(64); g_abl = gmalloc(4 + 12 * 8); }
    if (bytes * nb > g_cap) { if (g_data) gfree(g_data); g_cap = bytes * nb; g_data = gmalloc(g_cap); }
    wr32(g_flags, *flags);
    memset(G2H(g_ts), 0, 64); if (ts) { wrd(g_ts, ts->mSampleTime); wr64(g_ts + 8, ts->mHostTime); wr32(g_ts + 56, ts->mFlags & ~kAudioTimeStampWordClockTimeValid); }
    wr32(g_abl, nb);
    for (uint32_t i = 0; i < nb && i < 8; i++) { wr32(g_abl + 4 + 12 * i, io->mBuffers[i].mNumberChannels); wr32(g_abl + 8 + 12 * i, bytes); wr32(g_abl + 12 + 12 * i, g_data + i * bytes); }
    memset(G2H(g_data), 0, bytes * nb);
    uint32_t a[6] = { ca_ref, g_flags, g_ts, 0, frames, g_abl };
    uint32_t r = guest_call(ac, ca_proc, 6, a);
    for (uint32_t i = 0; i < nb && i < 8; i++) {
        uint32_t gp = rd32(g_abl + 12 + 12 * i), gn = rd32(g_abl + 8 + 12 * i);
        if (gn > io->mBuffers[i].mDataByteSize) gn = io->mBuffers[i].mDataByteSize;
        memcpy(io->mBuffers[i].mData, G2H(gp), gn);
    }
    *flags = rd32(g_flags);
    static int n; if (n++ == 0) fprintf(stderr, "[audio] first render: %u frames, %u buffers of %u bytes, guest returned %d\n", frames, nb, bytes, (int)r);
    return 0;
}
void I__AUGraphStart(CPU *c) {
    if (!ca_proc) { fprintf(stderr, "[audio] AUGraphStart without a render callback: silent\n"); RET(0); return; }
    if (getenv("RT32_NO_AUDIO")) { fprintf(stderr, "[audio] RT32_NO_AUDIO: silent\n"); RET(0); return; }
    if (!ca_out) {
        AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
        AudioComponent comp = AudioComponentFindNext(NULL, &d);
        if (!comp || AudioComponentInstanceNew(comp, &ca_out)) { fprintf(stderr, "[audio] no default output unit\n"); RET(0); return; }
        if (!ca_have_fmt) {
            ca_fmt = (AudioStreamBasicDescription){ 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
                                                    4, 1, 4, 2, 32, 0 };
        }
        OSStatus e = AudioUnitSetProperty(ca_out, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &ca_fmt, sizeof ca_fmt);
        AURenderCallbackStruct cb = { ca_render, NULL };
        OSStatus e2 = AudioUnitSetProperty(ca_out, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
        OSStatus e3 = AudioUnitInitialize(ca_out);
        fprintf(stderr, "[audio] output: %.0f Hz, %u ch, %u bits, flags 0x%x (format %d, callback %d, init %d)\n", ca_fmt.mSampleRate,
                (unsigned)ca_fmt.mChannelsPerFrame, (unsigned)ca_fmt.mBitsPerChannel, (unsigned)ca_fmt.mFormatFlags, (int)e, (int)e2, (int)e3);
    }
    OSStatus s = AudioOutputUnitStart(ca_out);
    fprintf(stderr, "[audio] AUGraphStart -> real output %s (%d)\n", s ? "FAILED" : "started", (int)s);
    RET(0);
}
void I__AUGraphStop(CPU *c) { if (ca_out) AudioOutputUnitStop(ca_out); RET(0); }
void I__AUGraphConnectNodeInput(CPU *c) { RET(0); }
void I__AUGraphSetNodeInputCallback(CPU *c) {   // (graph, node, bus, const AURenderCallbackStruct *{proc, refCon})
    if (ARG(3)) { ca_proc = rd32(ARG(3)); ca_ref = rd32(ARG(3) + 4);
        fprintf(stderr, "[audio] render callback proc=%s refCon=0x%x (node %u bus %u)\n", rt_func_name(ca_proc), ca_ref, ARG(1), ARG(2)); }
    RET(0);
}
void I__AudioUnitSetProperty(CPU *c) {   // (unit, prop, scope, element, data*, size)
    uint32_t prop = ARG(1), scope = ARG(2), el = ARG(3), data = ARG(4), size = ARG(5);
    if (prop == kAudioUnitProperty_StreamFormat && data && size >= 40) {
        AudioStreamBasicDescription f; f.mSampleRate = rdd(data); f.mFormatID = rd32(data + 8); f.mFormatFlags = rd32(data + 12);
        f.mBytesPerPacket = rd32(data + 16); f.mFramesPerPacket = rd32(data + 20); f.mBytesPerFrame = rd32(data + 24);
        f.mChannelsPerFrame = rd32(data + 28); f.mBitsPerChannel = rd32(data + 32); f.mReserved = 0;
        fprintf(stderr, "[audio] SetProperty StreamFormat unit 0x%x scope %u el %u: %.0f Hz, %u ch, %u bits, flags 0x%x, %u bytes/frame\n",
                ARG(0), scope, el, f.mSampleRate, (unsigned)f.mChannelsPerFrame, (unsigned)f.mBitsPerChannel, (unsigned)f.mFormatFlags, (unsigned)f.mBytesPerFrame);
        if (scope == kAudioUnitScope_Input && f.mFormatID == kAudioFormatLinearPCM) { ca_fmt = f; ca_have_fmt = 1; }
    } else if (prop == kAudioUnitProperty_SetRenderCallback && data) {
        ca_proc = rd32(data); ca_ref = rd32(data + 4);
        fprintf(stderr, "[audio] SetRenderCallback proc=%s refCon=0x%x\n", rt_func_name(ca_proc), ca_ref);
    } else { static int n; if (n++ < 12) fprintf(stderr, "[audio] SetProperty %u scope %u el %u size %u (ignored)\n", prop, scope, el, size); }
    RET(0);
}
void I__AudioUnitSetParameter(CPU *c) { RET(0); }
void I__AudioUnitUninitialize(CPU *c) { RET(0); }
void I__AudioOutputUnitStop(CPU *c) { if (ca_out) AudioOutputUnitStop(ca_out); RET(0); }

// ---------- keyboard layout (Text Input Sources + UCKeyTranslate) ----------
#include <Carbon/Carbon.h>
uint32_t rt_import_addr(const char *name);
static void tis_consts(void) {   // data imports start as zeroed blocks: store real keys in them before the game reads them
    static int done; if (done) return; done = 1;
    wr32(rt_import_addr("_kTISPropertyInputSourceLanguages"), hret(kTISPropertyInputSourceLanguages));
    wr32(rt_import_addr("_kTISPropertyUnicodeKeyLayoutData"), hret(kTISPropertyUnicodeKeyLayoutData));
}
void I__TISCopyCurrentKeyboardLayoutInputSource(CPU *c) { tis_consts(); RET(handle_for(TISCopyCurrentKeyboardLayoutInputSource())); }
void I__TISGetInputSourceProperty(CPU *c) {   // (source, key) -> unretained ("Get")
    tis_consts();
    CFTypeRef k = cf(ARG(1));
    if (!k) { fprintf(stderr, "[cf] TISGetInputSourceProperty: key 0x%x unknown (placeholder not filled in time?)\n", ARG(1)); RET(0); return; }
    RET(hret(TISGetInputSourceProperty((TISInputSourceRef)cf(ARG(0)), (CFStringRef)k)));
}
// (layout*, vkey, action, modifiers, kbdType, options, UInt32 *deadKeyState, UniCharCount max, UniCharCount *actual, UniChar *out)
void I__UCKeyTranslate(CPU *c) {
    UInt32 dead = ARG(6) ? rd32(ARG(6)) : 0; UniCharCount n = 0;
    OSStatus r = UCKeyTranslate((const UCKeyboardLayout *)G2H(ARG(0)), (UInt16)ARG(1), (UInt16)ARG(2), ARG(3), ARG(4), ARG(5),
                                &dead, (UniCharCount)ARG(7), &n, (UniChar *)G2H(ARG(9)));
    if (ARG(6)) wr32(ARG(6), dead);
    if (ARG(8)) wr32(ARG(8), (uint32_t)n);
    RET(r);
}

// ---------- run loop: the host owns the real one. Sources are accepted and ignored; pumping reports "timed out". ----------
#define FAKE_RLSOURCE 0x7A800000u
void I__CFRunLoopAddSource(CPU *c) { (void)c; }
void I__CFRunLoopRemoveSource(CPU *c) { (void)c; }
void I__CFRunLoopContainsSource(CPU *c) { RET(0); }
void I__CFRunLoopSourceCreate(CPU *c) {
    static int once; if (!once++) fprintf(stderr, "[cf] CFRunLoopSourceCreate: custom run-loop sources are never signalled\n");
    RET(FAKE_RLSOURCE);
}
void I__CFRunLoopRunInMode(CPU *c) { RET(3); }   // kCFRunLoopRunTimedOut
void I__CFRunLoopStop(CPU *c) { (void)c; }
void I__CFRunLoopRun(CPU *c) { fprintf(stderr, "[cf] CFRunLoopRun called from %s: would block forever\n", rt_func_name(rd32(c->esp))); abort(); }

// ---------- Carbon odds and ends (BioShock Infinite) ----------
void I__BeginUpdate(CPU *c) { (void)c; }
void I__EndUpdate(CPU *c) { (void)c; }
void I__HLock(CPU *c) { (void)c; }
void I__HUnlock(CPU *c) { (void)c; }
void I__DisposeHandle(CPU *c) { (void)c; }
void I__ReleaseResource(CPU *c) { (void)c; }
void I__GetIntlResource(CPU *c) { RET(0); }
void I__SameProcess(CPU *c) { if (ARG(2)) *(uint8_t *)G2H(ARG(2)) = 1; RET(0); }   // only one process exists for the game
void I__GetProcessInformation(CPU *c) { RET((uint32_t)-600); }   // procNotFound: ⚠ revisit if the game needs its bundle from here
void I__CSCopyMachineName(CPU *c) { RET(handle_for(CSCopyMachineName())); }
void I__CSCopyUserName(CPU *c) { RET(handle_for(CSCopyUserName((Boolean)ARG(0)))); }
// Foundation C functions returning toll-free bridged objects (NSArray/NSString == CFArray/CFString)
extern CFArrayRef NSSearchPathForDirectoriesInDomains(unsigned long dir, unsigned long mask, signed char expand);
extern CFStringRef NSTemporaryDirectory(void);
void I__NSSearchPathForDirectoriesInDomains(CPU *c) { CFArrayRef a = NSSearchPathForDirectoriesInDomains(ARG(0), ARG(1), (signed char)ARG(2)); RET(a ? handle_for(CFRetain(a)) : 0); }
void I__NSTemporaryDirectory(CPU *c) { CFStringRef s = NSTemporaryDirectory(); RET(s ? handle_for(CFRetain(s)) : 0); }

// ---------- exports for objc.c: Cocoa objects that are toll-free bridged to CF live in the CF handle table ----------
uint32_t cf_export_handle(const void *o) { return handle_for((CFTypeRef)o); }   // takes ownership of one reference
const void *cf_export_obj(uint32_t h) { return cf(h); }
int cf_is_handle(uint32_t h) { return h >= 0x1000 && h < 0xF0000000u && rd32(h) == CF_MAGIC; }
void cf_export_retain(uint32_t h) { cf_retain_h(h); }
void cf_export_release(uint32_t h) { cf_release_h(h); }

// ---------- Quartz images (the game builds its mouse-cursor image from raw pixels) ----------
void I__CGColorSpaceCreateWithName(CPU *c) {   // the name is usually a data import (kCGColorSpaceGenericRGB...) we don't map: use sRGB
    CFTypeRef n = cf(ARG(0)); CGColorSpaceRef s = n ? CGColorSpaceCreateWithName((CFStringRef)n) : NULL;
    if (!s) s = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    RET(handle_for(s));
}
void I__CGDataProviderCreateWithCFData(CPU *c) { CFTypeRef d = cf(ARG(0)); RET(d ? handle_for(CGDataProviderCreateWithCFData((CFDataRef)d)) : 0); }
void I__CGImageCreate(CPU *c) {   // (w, h, bitsPerComponent, bitsPerPixel, bytesPerRow, space, bitmapInfo, provider, decode*, interpolate, intent)
    CGImageRef im = CGImageCreate(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), (CGColorSpaceRef)cf(ARG(5)), (CGBitmapInfo)ARG(6),
                                  (CGDataProviderRef)cf(ARG(7)), NULL, (bool)(ARG(9) & 1), (CGColorRenderingIntent)ARG(10));
    static int n; if (n++ < 3) fprintf(stderr, "[cg] CGImageCreate %ux%u bpp %u -> %s\n", ARG(0), ARG(1), ARG(3), im ? "ok" : "failed");
    RET(im ? handle_for(im) : 0);
}
void I__CGColorSpaceRelease(CPU *c) { if (cf(ARG(0))) I__CFRelease(c); }
void I__CGDataProviderRelease(CPU *c) { if (cf(ARG(0))) I__CFRelease(c); }
void I__CGImageRelease(CPU *c) { if (cf(ARG(0))) I__CFRelease(c); }
void I__CGImageGetWidth(CPU *c) { CFTypeRef i = cf(ARG(0)); RET(i ? (uint32_t)CGImageGetWidth((CGImageRef)i) : 0); }
void I__CGImageGetHeight(CPU *c) { CFTypeRef i = cf(ARG(0)); RET(i ? (uint32_t)CGImageGetHeight((CGImageRef)i) : 0); }

// ---------- CGContext drawing: real only when the context is a real CG object; the fake NSGraphicsContext's
// graphicsPort is 0, so cursor-image drawing into it is a no-op (the game draws its own cursor in-engine) ----------
#define CGX ((CGContextRef)cf(ARG(0)))
#define RECT(i) CGRectMake(ARGF(i), ARGF((i) + 1), ARGF((i) + 2), ARGF((i) + 3))
void I__CGContextDrawImage(CPU *c) { if (CGX && cf(ARG(5))) CGContextDrawImage(CGX, RECT(1), (CGImageRef)cf(ARG(5))); }   // (ctx, CGRect, image)
void I__CGContextClipToRect(CPU *c) { if (CGX) CGContextClipToRect(CGX, RECT(1)); }
void I__CGContextFillRect(CPU *c) { if (CGX) CGContextFillRect(CGX, RECT(1)); }
void I__CGContextStrokeRect(CPU *c) { if (CGX) CGContextStrokeRect(CGX, RECT(1)); }
void I__CGContextStrokeRectWithWidth(CPU *c) { if (CGX) CGContextStrokeRectWithWidth(CGX, RECT(1), ARGF(5)); }
void I__CGContextSaveGState(CPU *c) { if (CGX) CGContextSaveGState(CGX); }
void I__CGContextRestoreGState(CPU *c) { if (CGX) CGContextRestoreGState(CGX); }
void I__CGContextSetRGBFillColor(CPU *c) { if (CGX) CGContextSetRGBFillColor(CGX, ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
void I__CGContextSetRGBStrokeColor(CPU *c) { if (CGX) CGContextSetRGBStrokeColor(CGX, ARGF(1), ARGF(2), ARGF(3), ARGF(4)); }
void I__CGContextSetTextMatrix(CPU *c) { if (CGX) CGContextSetTextMatrix(CGX, CGAffineTransformMake(ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5), ARGF(6))); }
void I__CGContextRelease(CPU *c) { if (CGX) I__CFRelease(c); }
void I__GetWindowPropertySize(CPU *c) { WinProp *w = wprop(ARG(0), ARG(1), ARG(2), 0); if (!w) { RET((uint32_t)-5604); return; } if (ARG(3)) wr32(ARG(3), w->size); RET(0); }
void I__RemoveWindowProperty(CPU *c) { WinProp *w = wprop(ARG(0), ARG(1), ARG(2), 0); if (w) w->win = 0; RET(0); }

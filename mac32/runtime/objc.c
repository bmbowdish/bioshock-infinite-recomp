// Minimal Objective-C (1.0 / 32-bit) message dispatch for Aspyr's wrapper code (BioShock Infinite).
// Cocoa objects the guest talks to are faked natively. Anything not handled stops with "[objc] unimplemented -[Class sel]".
// ⚠ Untested. In this old ABI __cls_refs entries stay as pointers to the class NAME string and selectors are pointers to the
// selector string (no dyld fix-up runs), so classes are recognised by looking at the string the receiver points to.
#include "rt.h"
#include "tables.h"
#include "game32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <OpenGL/OpenGL.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define OBJ_MAGIC 0x4F424A46u   // 'OBJF'
typedef struct { char cls[48]; uint32_t v[4]; } Obj;
#define NOBJ (1 << 20)   // fake instances are never freed (retain/release are no-ops), so leave plenty of room
static Obj objs[NOBJ];
static int nobjs = 1;

static void obj_census(void) {   // which classes fill the table (printed at milestones)
    static char nm[64][48]; static int cnt[64]; int k = 0; memset(cnt, 0, sizeof cnt);
    for (int i = 1; i < nobjs; i++) { int j; for (j = 0; j < k; j++) if (!strcmp(nm[j], objs[i].cls)) break;
        if (j == k && k < 64) { snprintf(nm[k], 48, "%s", objs[i].cls); k++; } if (j < 64) cnt[j]++; }
    fprintf(stderr, "[objc] %d fake objects:", nobjs - 1);
    for (int j = 0; j < k; j++) if (cnt[j] * 50 >= nobjs) fprintf(stderr, " %s=%d", nm[j], cnt[j]);
    fprintf(stderr, "\n");
}
static uint32_t new_obj(const char *cls) {
    if (nobjs >= NOBJ) { obj_census(); fprintf(stderr, "[objc] object table full\n"); abort(); }
    if (nobjs == 8192 || nobjs == 65536 || nobjs == 262144) obj_census();
    int i = nobjs++; snprintf(objs[i].cls, sizeof objs[i].cls, "%s", cls);
    uint32_t g = gmalloc(16); wr32(g, OBJ_MAGIC); wr32(g + 4, (uint32_t)i); return g;
}
static Obj *obj_of(uint32_t g) {
    if (g < 0x1000 || g >= 0xF0000000u || rd32(g) != OBJ_MAGIC) return NULL;
    uint32_t i = rd32(g + 4); return i < (uint32_t)nobjs ? &objs[i] : NULL;
}
static const char *cstr(uint32_t a) { return (const char *)G2H(a); }
static int looks_like_class_name(uint32_t r) {   // pointer to an identifier starting with an upper-case letter
    if (r < 0x1000 || r >= 0xF0000000u) return 0;
    const char *s = cstr(r); size_t n = 0;
    if (!isupper((unsigned char)s[0])) return 0;
    while (s[n] && n < 48) { if (!isalnum((unsigned char)s[n]) && s[n] != '_') return 0; n++; }
    return n > 1 && n < 48 && s[n] == 0;
}
// Cocoa UI classes whose messages only affect the (host-owned) UI: unknown messages log once and return a default.
// Class methods return a fresh fake instance (e.g. +[NSCursor arrowCursor]); instance methods return 0.
static int ui_class(const char *cls) {
    static const char *k[] = { "NSCursor", "NSMenu", "NSMenuItem", "NSWindow", "NSView", "NSScreen", "NSApplication",
        "NSAutoreleasePool", "NSNotificationCenter", "NSWorkspace", "NSGraphicsContext", "NSColor", "NSImage", "NSEvent",
        "NSRunningApplication", "NSProcessInfo", "NSUserDefaults", "NSDistributedNotificationCenter", "NSAlert", 0 };
    for (int i = 0; k[i]; i++) if (!strcmp(cls, k[i])) return 1;
    return 0;
}
static int ui_default(CPU *c, const char *cls, int is_class, const char *sel, uint32_t self) {
    if (!ui_class(cls)) return 0;
    static char seen[256][96]; static int nseen; char key[96]; snprintf(key, sizeof key, "%c%s %s", is_class ? '+' : '-', cls, sel);
    int s = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) { s = 1; break; }
    if (!s && nseen < 256) { snprintf(seen[nseen++], 96, "%s", key); fprintf(stderr, "[objc] default for %c[%s %s]\n", is_class ? '+' : '-', cls, sel); }
    if (is_class) RET(new_obj(cls)); else RET(!strncmp(sel, "init", 4) ? self : 0);   // -init... returns the object itself
    (void)self; return 1;
}
static void die(CPU *c, const char *cls, int is_class, const char *sel) {
    fprintf(stderr, "[objc] unimplemented %c[%s %s] (called from %s)\n", is_class ? '+' : '-', cls, sel, rt_func_name(rd32(c->esp)));
    abort();
}

// Generic memory-management / identity messages shared by every faked object.
static int common(CPU *c, uint32_t self, const char *sel) {
    if (!strcmp(sel, "retain") || !strcmp(sel, "autorelease") || !strcmp(sel, "init") || !strcmp(sel, "self")) { RET(self); return 1; }
    if (!strcmp(sel, "release") || !strcmp(sel, "drain")) { RET(0); return 1; }
    return 0;
}


// Toll-free bridged objects (NSData/NSString/NSArray/NSDictionary...) are CF objects behind CF handles.
#include <CoreFoundation/CoreFoundation.h>
uint32_t cf_export_handle(const void *o); const void *cf_export_obj(uint32_t h); int cf_is_handle(uint32_t h);
void cf_export_retain(uint32_t h); void cf_export_release(uint32_t h);
static int cf_message(CPU *c, uint32_t self, const char *sel) {
    if (!cf_is_handle(self)) return 0;
    CFTypeRef o = cf_export_obj(self);
    if (!strcmp(sel, "retain")) { cf_export_retain(self); RET(self); return 1; }
    if (!strcmp(sel, "release")) { cf_export_release(self); RET(0); return 1; }
    if (!strcmp(sel, "autorelease") || !strcmp(sel, "self") || !strcmp(sel, "copy")) { RET(self); return 1; }
    if (o && CFGetTypeID(o) == CFDataGetTypeID()) {
        if (!strcmp(sel, "length")) { RET((uint32_t)CFDataGetLength(o)); return 1; }
        if (!strcmp(sel, "bytes")) {   // guest copy, refreshed per call (same approach as CFDataGetBytePtr)
            uint32_t n = (uint32_t)CFDataGetLength(o), g = gmalloc(n + 1); memcpy(G2H(g), CFDataGetBytePtr(o), n); RET(g); return 1; }
    }
    if (o && CFGetTypeID(o) == CFStringGetTypeID()) {
        if (!strcmp(sel, "length")) { RET((uint32_t)CFStringGetLength(o)); return 1; }
        if (!strcmp(sel, "UTF8String")) { char b[4096]; if (!CFStringGetCString(o, b, sizeof b, kCFStringEncodingUTF8)) b[0] = 0; RET(gstrdup(b)); return 1; }
    }
    if (o && CFGetTypeID(o) == CFArrayGetTypeID()) {
        if (!strcmp(sel, "count")) { RET((uint32_t)CFArrayGetCount(o)); return 1; }
        if (!strcmp(sel, "objectAtIndex:")) { CFTypeRef v = CFArrayGetValueAtIndex(o, (CFIndex)ARG(2)); RET(v ? cf_export_handle(CFRetain(v)) : 0); return 1; }
    }
    fprintf(stderr, "[objc] unimplemented -[%s %s] on a CF object (called from %s)\n",
            o ? "<CF type>" : "<dead CF handle>", sel, rt_func_name(rd32(c->esp)));
    abort();
}
static int nsdata_class(CPU *c, const char *cls, const char *sel) {   // +[NSData dataWithBytes:length:] etc.
    if (strcmp(cls, "NSData") && strcmp(cls, "NSMutableData")) return 0;
    if (!strcmp(sel, "dataWithBytes:length:")) { RET(cf_export_handle(CFDataCreate(NULL, ARG(2) ? (const UInt8 *)G2H(ARG(2)) : NULL, (CFIndex)ARG(3)))); return 1; }
    return 0;
}
static void dispatch(CPU *c) {
    uint32_t self = ARG(0); const char *sel = cstr(ARG(1));
    Obj *o = obj_of(self);
    if (!self) { RET(0); return; }                                 // messages to nil return nil/0, as in Objective-C
    if (!o && cf_message(c, self, sel)) return;                    // toll-free bridged CF handles first (their magic reads "BOFC")
    if (!o && looks_like_class_name(self)) {                       // class message
        const char *cls = cstr(self);
        if (!strcmp(sel, "alloc") && !strcmp(cls, "NSData")) {   // class-cluster placeholder: init returns a CFData,
            static uint32_t ph; if (!ph) ph = new_obj("NSData");      // so one shared, stateless placeholder suffices
            RET(ph); return; }
        if (!strcmp(sel, "alloc") || !strcmp(sel, "new")) { RET(new_obj(cls)); return; }
        if (!strcmp(sel, "class")) { RET(self); return; }
        if (nsdata_class(c, cls, sel)) return;
        if (!strcmp(cls, "NSOpenGLContext")) {
            if (!strcmp(sel, "clearCurrentContext")) { CGLSetCurrentContext(NULL); RET(0); return; }
            if (!strcmp(sel, "currentContext")) {
                static uint32_t cur; if (!cur) cur = new_obj("NSOpenGLContext"); RET(cur); return; }
        }
        if (ui_default(c, cls, 1, sel, self)) return;
        if (!strcmp(sel, "retain") || !strcmp(sel, "release") || !strcmp(sel, "autorelease")) {   // memory management on a class
            static int n; if (n++ < 5) fprintf(stderr, "[objc] +[%s %s] treated as a no-op (called from %s)\n", cls, sel, rt_func_name(rd32(c->esp)));
            RET(!strcmp(sel, "release") ? 0 : self); return;   // (or on a pointer that merely looks like a class name)
        }
        die(c, cls, 1, sel);
    }
    if (!o && cf_message(c, self, sel)) return;
    if (!o) { fprintf(stderr, "[objc] message %s to unknown receiver 0x%x\n", sel, self); abort(); }
    if (common(c, self, sel)) return;
    if (!strcmp(o->cls, "NSData") && !strcmp(sel, "initWithBytes:length:")) {   // init returns the real (bridged) CFData
        RET(cf_export_handle(CFDataCreate(NULL, ARG(2) ? (const UInt8 *)G2H(ARG(2)) : NULL, (CFIndex)ARG(3)))); return; }
    if (!strcmp(o->cls, "NSOpenGLPixelFormat")) {
        if (!strcmp(sel, "initWithAttributes:")) {   // log what the game asks for (profile, depth, samples...)
            uint32_t a = ARG(2); fprintf(stderr, "[gl] NSOpenGLPixelFormat attributes:");
            for (int i = 0; a && i < 64; i++) { uint32_t v = rd32(a + 4 * i); if (!v) break; fprintf(stderr, " %u", v); }
            fprintf(stderr, "\n"); RET(self); return; }
        if (!strcmp(sel, "CGLPixelFormatObj")) { RET(0x7710u); return; }
        if (!strcmp(sel, "numberOfVirtualScreens")) { RET(1); return; }
    }
    if (!strcmp(o->cls, "NSOpenGLContext")) {                      // the host's context is already current (guest handle 1)
        if (!strcmp(sel, "initWithFormat:shareContext:")) { RET(self); return; }
        if (!strcmp(sel, "makeCurrentContext")) { extern void *host_cgl_context(void); extern void host_note_gl_bind(void); host_note_gl_bind(); CGLSetCurrentContext((CGLContextObj)host_cgl_context()); RET(0); return; }
        if (!strcmp(sel, "update") || !strcmp(sel, "clearDrawable") ||
            !strcmp(sel, "setView:") || !strcmp(sel, "setValues:forParameter:")) { RET(0); return; }
        if (!strcmp(sel, "flushBuffer")) { host_present(); RET(0); return; }
        if (!strcmp(sel, "CGLContextObj")) { RET(1); return; }
    }
    if (ui_default(c, o->cls, 0, sel, self)) return;
    die(c, o->cls, 0, sel);
}
void I__objc_msgSend(CPU *c) { dispatch(c); }
// Carbon apps call this to bring up Cocoa; our host process already runs NSApplication.
void I__NSApplicationLoad(CPU *c) { RET(1); }
// HICocoaView: a Carbon HIView hosting an NSView. The host owns the real window/view, so this just remembers the
// guest's NSView object (or hands out a fake NSView when the game asks for one it never created).
static uint32_t hicocoa_view;
void I__HICocoaViewCreate(CPU *c) {   // (NSView *view, OptionBits, HIViewRef *out)
    hicocoa_view = ARG(0) ? ARG(0) : new_obj("NSView");
    if (ARG(2)) wr32(ARG(2), 0x7502u);
    RET(0);
}
void I__HICocoaViewGetView(CPU *c) { if (!hicocoa_view) hicocoa_view = new_obj("NSView"); RET(hicocoa_view); }

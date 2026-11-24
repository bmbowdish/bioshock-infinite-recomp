import os
p=os.path.expandvars("$HOME/mnt/bioshock-infinite-recomp/mac32/runtime/cf.c")
g=open(p).read()
start=g.index("void I__IOHIDManagerCreate(CPU *c)")
end=g.index("void I__IOHIDValueGetIntegerValue(CPU *c) { RET(0); }")+len("void I__IOHIDValueGetIntegerValue(CPU *c) { RET(0); }")
new=r'''// ---------- IOHIDManager: keyboard from the host window as HID values ----------
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
}'''
g=g[:start]+new+g[end:]
if "#include <pthread.h>" not in g: g="#include <pthread.h>\n"+g
open(p,"w").write(g); print("ok")

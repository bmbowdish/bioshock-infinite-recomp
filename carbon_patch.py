import os
p=os.path.expandvars("$HOME/mnt/bioshock-infinite-recomp/mac32/runtime/cf.c")
g=open(p).read()
# 1. replace the event stubs
old_block_start=g.index("void I__GetEventParameter(CPU *c) { RET((uint32_t)-9870); }")
old_block_end=g.index("void I__InstallEventLoopTimer(CPU *c)")
new=r'''// ---------- Carbon event queue: key events from the host window ----------
// The host queues raw keyboard events (keyb/1 down, keyb/3 up); ReceiveNextEvent hands them out as fake
// EventRefs and GetEventParameter serves kcod (UInt32 keycode), kmod (UInt32 modifiers), kchr (1-byte char),
// wind (the fake window) and mloc (QD point 0,0). Games with a Win32-style pump convert events that
// SendEventToEventTarget reports as not handled, so that keeps returning eventNotHandledErr.
typedef struct { uint32_t cls, kind, kcod, kmod; uint8_t kchr; } CEv;
static CEv cev_ring[256]; static uint32_t cev_ring_n;
static CEv cev_q[256]; static uint32_t cev_head, cev_tail; static pthread_mutex_t cev_lock = PTHREAD_MUTEX_INITIALIZER;
#define CEV_H 0x79000000u
void rt_carbon_key(uint16_t keycode, uint8_t ch, uint32_t mods, int down) {   // host thread
    pthread_mutex_lock(&cev_lock);
    uint32_t n = (cev_tail + 1) & 255;
    if (n != cev_head) { cev_q[cev_tail] = (CEv){ 'keyb', down ? 1u : 3u, keycode, mods, ch }; cev_tail = n; }
    pthread_mutex_unlock(&cev_lock);
}
static CEv *cev_of(uint32_t h) { return (h & 0xFF000000u) == CEV_H ? &cev_ring[h & 255] : NULL; }
void I__GetEventParameter(CPU *c) {   // (event, name, type, *actualType, size, *actualSize, *data)
    CEv *e = cev_of(ARG(0)); uint32_t name = ARG(1), size = ARG(4), out = ARG(6), type = 0, n = 0, v = 0;
    if (!e) { RET((uint32_t)-9870); return; }   // eventParameterNotFoundErr
    switch (name) {
    case 'kcod': type = 'magn'; n = 4; v = e->kcod; break;
    case 'kmod': type = 'magn'; n = 4; v = e->kmod; break;
    case 'kchr': type = 'TEXT'; n = 1; v = e->kchr; break;
    case 'wind': type = 'wind'; n = 4; v = FAKE_WINDOW; break;
    case 'mloc': type = 'QDpt'; n = 4; v = 0; break;
    default: { static int logged; if (logged++ < 8) fprintf(stderr, "[carbon] GetEventParameter %c%c%c%c not served\n", name >> 24, (name >> 16) & 255, (name >> 8) & 255, name & 255); }
        RET((uint32_t)-9870); return;
    }
    if (ARG(3)) wr32(ARG(3), type);
    if (ARG(5)) wr32(ARG(5), n);
    if (out) { if (n == 1) { if (size >= 1) wr8(out, (uint8_t)v); } else if (size >= 4) wr32(out, v); }
    RET(0);
}
void I__GetEventClass(CPU *c) { CEv *e = cev_of(ARG(0)); RET(e ? e->cls : 0); }
void I__GetEventKind(CPU *c) { CEv *e = cev_of(ARG(0)); RET(e ? e->kind : 0); }
void I__ReleaseEvent(CPU *c) { (void)c; }
void I__GetEventRetainCount(CPU *c) { RET(1); }
void I__PostEventToQueue(CPU *c) { RET(0); }
void I__SendEventToEventTarget(CPU *c) { RET((uint32_t)-9874); }   // eventNotHandledErr
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
                fprintf(stderr, "[carbon] ReceiveNextEvent (from %s) -> keyb/%u kcod %u kchr 0x%x\n", rt_func_name(rd32(c->esp)), e.kind, e.kcod, e.kchr);
            if (out) wr32(out, CEV_H | slot);
            RET(0); return;
        }
    }
    pthread_mutex_unlock(&cev_lock);
    if (out) wr32(out, 0);
    RET((uint32_t)-9875);   // eventLoopTimedOutErr
}
'''
g=g[:old_block_start]+new+g[old_block_end:]
open(p,"w").write(g); print("ok")

import os
R=os.path.expandvars("$HOME/mnt/bioshock-infinite-recomp/mac32/runtime/")
g=open(R+"cf.c").read()
# 1. event struct gains mouse fields
old='typedef struct { uint32_t cls, kind, kcod, kmod; uint8_t kchr; } CEv;'
new='''typedef struct { uint32_t cls, kind, kcod, kmod; uint8_t kchr;
                 float lx, ly, dx, dy; uint16_t btn, axis; uint32_t ccnt; int32_t wheel; } CEv;   // mouse: local pos, delta, button, wheel'''
assert old in g; g=g.replace(old,new,1)
# 2. key constructor must zero the new fields (designated init)
old="if (n != cev_head) { cev_q[cev_tail] = (CEv){ 'keyb', down ? 1u : 3u, keycode, mods, ch }; cev_tail = n; }"
new="if (n != cev_head) { cev_q[cev_tail] = (CEv){ .cls = 'keyb', .kind = down ? 1u : 3u, .kcod = keycode, .kmod = mods, .kchr = ch }; cev_tail = n; }"
assert old in g; g=g.replace(old,new,1)
# 3. mouse producer + handler registry after rt_carbon_key
old='''static CEv *cev_of(uint32_t h) { return (h & 0xFF000000u) == CEV_H ? &cev_ring[h & 255] : NULL; }'''
new='''// Mouse: kind 1 down, 2 up, 5 moved, 6 dragged, 10 wheel. (lx, ly) in engine/window coordinates, top-left origin.
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
static CEv *cev_of(uint32_t h) { return (h & 0xFF000000u) == CEv_H_CHECK ? &cev_ring[h & 255] : NULL; }
// Installed Carbon handlers: SendEventToEventTarget runs the ones for the event's class/kind
// (window target first, then application), stopping at the first that returns noErr.
typedef struct { uint32_t target, handler, user, n; uint32_t types[32][2]; } CHandler;
static CHandler chand[32]; static int nchand;'''
new=new.replace("CEv_H_CHECK","CEV_H")
assert old in g; g=g.replace(old,new,1)
# 4. InstallEventHandler records
old='''    if (ARG(5)) wr32(ARG(5), 0x7400u); RET(0);
}'''
new='''    if (nchand < 32) { CHandler *h = &chand[nchand++]; h->target = ARG(0); h->handler = ARG(1); h->user = ARG(4); h->n = ARG(2) < 32 ? ARG(2) : 32;
        for (uint32_t i = 0; i < h->n; i++) { h->types[i][0] = rd32(ARG(3) + 8 * i); h->types[i][1] = rd32(ARG(3) + 8 * i + 4); } }
    if (ARG(5)) wr32(ARG(5), 0x7400u + (uint32_t)nchand); RET(0);
}'''
assert old in g; g=g.replace(old,new,1)
# 5. GetEventParameter: mouse params
old='''    case 'mloc': type = 'QDpt'; n = 4; v = 0; break;'''
new='''    case 'mloc': { win_rect_default();   // QD Point {v, h}, global coordinates
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
    case 'mwdl': type = 'long'; n = 4; v = (uint32_t)e->wheel; break;'''
assert old in g; g=g.replace(old,new,1)
old='''    if (out) { if (n == 1) { if (size >= 1) wr8(out, (uint8_t)v); } else if (size >= 4) wr32(out, v); }'''
new='''    if (out) { if (n == 1) { if (size >= 1) wr8(out, (uint8_t)v); } else if (n == 2) { if (size >= 2) wr16(out, (uint16_t)v); } else if (size >= 4) wr32(out, v); }'''
assert old in g; g=g.replace(old,new,1)
# quiet the "not served" log for params the game probes on purpose
g=g.replace('''    default: { static int logged; if (logged++ < 8)''','''    default: { static int logged; if (name != 'QUED' && name != 'evnt' && logged++ < 8)''',1)
# 6. SendEventToEventTarget dispatches to handlers
old='''void I__SendEventToEventTarget(CPU *c) { RET((uint32_t)-9874); }   // eventNotHandledErr'''
new='''void I__SendEventToEventTarget(CPU *c) {   // (event, target): our events go to matching installed handlers
    CEv *e = cev_of(ARG(0));
    if (e) {
        static const uint32_t order[2] = { 0x7202u, 0x7200u };   // window target (FAKE_TARGET + 2), then application
        for (int t = 0; t < 2; t++) for (int i = 0; i < nchand; i++) {
            CHandler *h = &chand[i]; if (h->target != order[t]) continue;
            int match = 0; for (uint32_t k = 0; k < h->n; k++) if (h->types[k][0] == e->cls && h->types[k][1] == e->kind) { match = 1; break; }
            if (!match) continue;
            uint32_t ev = ARG(0), a[3] = { 0x7A00u, ev, h->user };
            uint32_t r = guest_call(c, h->handler, 3, a);
            if (r == 0) { RET(0); return; }
        }
    }
    RET((uint32_t)-9874);   // eventNotHandledErr
}'''
assert old in g; g=g.replace(old,new,1)
# 7. cursor: real association / warp (CGPoint is two floats on i386)
old='''void I__CGAssociateMouseAndMouseCursorPosition(CPU *c) { RET(0); }
void I__CGWarpMouseCursorPosition(CPU *c) { RET(0); }'''
new='''void I__CGAssociateMouseAndMouseCursorPosition(CPU *c) {   // false while the game uses the mouse for looking
    static int last = -1; int on = ARG(0) & 1;
    if (on != last) { fprintf(stderr, "[mouse] cursor %s\\n", on ? "follows the mouse" : "detached (mouse look)"); last = on; }
    RET(CGAssociateMouseAndMouseCursorPosition(on));
}
void I__CGWarpMouseCursorPosition(CPU *c) { CGPoint p = { ARGF(0), ARGF(1) }; RET(CGWarpMouseCursorPosition(p)); }'''
assert old in g; g=g.replace(old,new,1)
open(R+"cf.c","w").write(g)

h=open(R+"host.m").read()
old='''    game32_mouse_move(gcpu, (float)((px.x - gl_win_off_x) / gl_win_scale), (float)((sz.height - px.y - gl_win_off_y) / gl_win_scale));
}'''
new='''    float lx = (float)((px.x - gl_win_off_x) / gl_win_scale), ly = (float)((sz.height - px.y - gl_win_off_y) / gl_win_scale);
    game32_mouse_move(gcpu, lx, ly);
    NSEventType t = [e type];
    if (t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged || t == NSEventTypeRightMouseDragged || t == NSEventTypeOtherMouseDragged)
        rt_carbon_mouse(t == NSEventTypeMouseMoved ? 5 : 6, lx, ly, (float)[e deltaX], (float)[e deltaY], (int)[e buttonNumber], 0, 0, carbon_mods([e modifierFlags]));
    last_lx = lx; last_ly = ly;
}'''
assert old in h; h=h.replace(old,new,1)
old='''    game32_mouse_button(gcpu, b, down, (int)[e clickCount]);'''
new='''    game32_mouse_button(gcpu, b, down, (int)[e clickCount]);
    rt_carbon_mouse(down ? 1 : 2, last_lx, last_ly, 0, 0, b, (int)[e clickCount], 0, carbon_mods([e modifierFlags]));'''
assert old in h; h=h.replace(old,new,1)
old='''    if ((int)d) game32_wheel(gcpu, (int)d);'''
new='''    if ((int)d) { game32_wheel(gcpu, (int)d); rt_carbon_mouse(10, last_lx, last_ly, 0, 0, 0, 0, (int)d, carbon_mods([e modifierFlags])); }'''
assert old in h; h=h.replace(old,new,1)
old='''extern void rt_carbon_key(uint16_t keycode, uint8_t ch, uint32_t mods, int down);'''
new='''extern void rt_carbon_key(uint16_t keycode, uint8_t ch, uint32_t mods, int down);
extern void rt_carbon_mouse(uint32_t kind, float lx, float ly, float dx, float dy, int button, int clicks, int wheel, uint32_t mods);
static float last_lx, last_ly;   // last mouse position in engine coordinates (for button and wheel events)'''
assert old in h; h=h.replace(old,new,1)
open(R+"host.m","w").write(h)
print("ok")

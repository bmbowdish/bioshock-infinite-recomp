// C++ exception handling for translated i386 code (Itanium ABI, gcc/clang Mach-O). Added for BioShock Infinite.
// throw: walk the guest frame chain (ebp), find each frame's call site in the LSDA tables, and jump to the first landing
// pad that either catches the thrown type or runs cleanups. Callee-saved registers are restored from each skipped
// frame's prologue pushes. Cleanups end in _Unwind_Resume, which continues the walk from that frame.
// ⚠ Simplifications: single phase (cleanups run even if nothing catches); type matching by mangled name with
// pointer-qualification and single/multiple-inheritance base walks; exception specifications are ignored.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local EHReg *eh_top;
#define HDR       32          // header before the thrown object: magic, typeinfo, dest, handlers, next, rethrown
#define EXC_MAGIC 0x43584345u // 'ECXC'
typedef struct {
    uint32_t h;
} Caught;
static _Thread_local uint32_t caught[64];
static _Thread_local int ncaught;
static int eh_debug = -1;

static const GFde *fde_for(uint32_t pc) {
    int lo = 0, hi = ngfdes - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gfdes[m].hi <= pc)
            lo = m + 1;
        else if (gfdes[m].lo > pc)
            hi = m - 1;
        else
            return &gfdes[m];
    }
    return NULL;
}
static const GEHSite *site_for(uint32_t pc) {
    int lo = 0, hi = ngehsites - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gehsites[m].hi <= pc)
            lo = m + 1;
        else if (gehsites[m].lo > pc)
            hi = m - 1;
        else
            return &gehsites[m];
    }
    return NULL;
}
static const char *ti_name(uint32_t ti) {
    uint32_t n = ti ? rd32(ti + 4) : 0;
    return n ? (const char *)G2H(n) : "?";
}
static void norm(const char *in, char *out, size_t cap) { // drop cv-qualifiers on pointees: PKw -> Pw
    size_t k = 0;
    for (const char *p = in; *p && k + 1 < cap; p++) {
        if ((*p == 'K' || *p == 'V') && p > in && (p[-1] == 'P' || p[-1] == 'K' || p[-1] == 'V'))
            continue;
        out[k++] = *p;
    }
    out[k] = 0;
}
static int name_eq(uint32_t a, uint32_t b) {
    char x[512], y[512];
    norm(ti_name(a), x, sizeof x);
    norm(ti_name(b), y, sizeof y);
    const char *px = x[0] == '*' ? x + 1 : x,
               *py = y[0] == '*' ? y + 1 : y; // some names carry a leading '*' (local types)
    return !strcmp(px, py);
}
// Base classes: __si_class_type_info {vt, name, base}; __vmi_class_type_info {vt, name, flags, count, {base, offflags}...}.
// The kind is guessed from the layout (⚠ heuristic): a word at +8 that is itself a typeinfo-looking pointer -> si.
static int is_ti(uint32_t p) {
    if (p < 0x1000 || p >= 0xF0000000u)
        return 0;
    uint32_t n = rd32(p + 4);
    if (n < 0x1000 || n >= 0xF0000000u)
        return 0;
    const char *s = G2H(n);
    return s[0] && s[0] < 0x7f;
}
static int derives(uint32_t thrown, uint32_t want, int depth) {
    if (!thrown || depth > 8)
        return 0;
    if (thrown == want || name_eq(thrown, want))
        return 1;
    const char *n = ti_name(thrown);
    if (n[0] == 'P')
        return 0; // pointers: only exact (qualification) matches
    uint32_t w2 = rd32(thrown + 8);
    if (is_ti(w2))
        return derives(w2, want, depth + 1);
    uint32_t cnt = rd32(thrown + 12);
    if (cnt > 0 && cnt < 16)
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t b = rd32(thrown + 16 + 8 * i);
            if (is_ti(b) && derives(b, want, depth + 1))
                return 1;
        }
    return 0;
}
static int ptr_match(uint32_t thrown, uint32_t want) { // T* caught as Base* / const T* / void*
    const char *a = ti_name(thrown), *b = ti_name(want);
    if (a[0] != 'P' || b[0] != 'P')
        return 0;
    if (name_eq(thrown, want))
        return 1;
    char y[512];
    norm(b, y, sizeof y);
    if (!strcmp(y, "Pv"))
        return 1;
    uint32_t pa = rd32(thrown + 12), pb = rd32(want + 12); // __pointer_type_info: {vt, name, flags, pointee}
    return is_ti(pa) && is_ti(pb) && derives(pa, pb, 0);
}
static int catches(uint32_t thrown, uint32_t want) {
    return !want || derives(thrown, want, 0) || ptr_match(thrown, want);
}

// Saved callee-saved registers of a frame: standard prologue push ebp; mov ebp,esp; then push ebx/esi/edi in any order.
static int restore_saved(uint32_t fstart, uint32_t ebp, void *rp) {
    struct {
        uint32_t ebx, esi, edi;
    } *c = rp;
    const uint8_t *p = (const uint8_t *)G2H(fstart);
    if (!(p[0] == 0x55 && p[1] == 0x89 && p[2] == 0xE5))
        return 0;
    uint32_t slot = ebp - 4;
    for (int i = 3; i < 3 + 4; i++, slot -= 4) {
        if (p[i] == 0x53)
            c->ebx = rd32(slot);
        else if (p[i] == 0x56)
            c->esi = rd32(slot);
        else if (p[i] == 0x57)
            c->edi = rd32(slot);
        else
            break;
    }
    return 1;
}
static void die(const char *why, uint32_t h, uint32_t pc) {
    fprintf(stderr, "[eh] %s (type %s, last frame %s)\n", why, h ? ti_name(rd32(h + 4)) : "?", rt_func_name(pc));
    if (h) {
        uint32_t o = h + HDR, p = rd32(o);
        char msg[600];
        int k = 0;
        if (p > 0x1000 && p < 0xF0000000u)
            for (uint32_t i = 0; i < 300 && k < 590; i++) {
                uint32_t ch = rd32(p + 4 * i);
                if (!ch)
                    break;
                msg[k++] = ch < 0x80 ? (char)ch : '?';
            }
        msg[k] = 0;
        if (k)
            fprintf(stderr, "[eh] message (if it is a wide string): %s\n", msg);
    }
    abort();
}

// Unwind from a call made at return address 'pc' in the frame with base 'ebp'; esp_after = esp once that call returned.
static void unwind(CPU *c, uint32_t h, uint32_t pc, uint32_t ebp, uint32_t esp_after) {
    if (eh_debug < 0)
        eh_debug = getenv("RT32_EH_DEBUG") != NULL;
    uint32_t thrown = rd32(h + 4);
    struct {
        uint32_t ebx, esi, edi;
    } regs = {c->ebx, c->esi, c->edi}; // tracked as frames are skipped
    for (int depth = 0; depth < 4096; depth++) {
        if (pc < 0x1000 || pc == 0xDEAD0000u || ebp < 0x1000)
            die("uncaught C++ exception", h, pc);
        const GEHSite *s = site_for(pc - 1);
        if (s) {
            int32_t sel = -1;
            int cleanup = s->nact == 0;
            for (int i = 0; i < s->nact && sel < 0; i++) {
                const GEHAct *a = &gehacts[s->act + i];
                if (a->filter == 0)
                    cleanup = 1;
                else if (a->filter > 0) {
                    uint32_t want = a->ti ? (a->indirect ? rd32(a->ti) : a->ti) : 0;
                    if (catches(thrown, want))
                        sel = a->filter;
                }
            }
            if (sel >= 0 || cleanup) {
                EHReg *r = eh_top;
                while (r && !(r->fn == s->fn && r->esp == ebp + 4))
                    r = r->prev;
                if (!r) {
                    fprintf(stderr, "[eh] %s has a landing pad at 0x%x but no registration (fn 0x%x ebp 0x%x)\n",
                            rt_func_name(pc), s->lp, s->fn, ebp);
                    die("cannot unwind", h, pc);
                }
                if (eh_debug)
                    fprintf(stderr, "[eh] -> %s landing pad 0x%x (%s, selector %d)\n", rt_func_name(pc), s->lp,
                            sel >= 0 ? "catch" : "cleanup", sel >= 0 ? sel : 0);
                c->esp = esp_after;
                c->ebp = ebp;
                c->ebx = regs.ebx;
                c->esi = regs.esi;
                c->edi = regs.edi;
                c->eax = h;
                c->edx = (uint32_t)(sel >= 0 ? sel : 0);
                eh_top = r;
                r->lp = s->lp;
                _longjmp(r->jb, 1);
            }
        }
        // skip this frame: restore what it saved for its caller, then step to the caller
        const GFde *f = fde_for(pc - 1);
        if (!f)
            die("return address outside known functions", h, pc);
        if (!restore_saved(f->lo, ebp, &regs)) {
            fprintf(stderr, "[eh] %s: unexpected prologue\n", rt_func_name(f->lo));
            die("cannot unwind", h, pc);
        }
        esp_after = ebp + 8;
        pc = rd32(ebp + 4);
        ebp = rd32(ebp);
    }
    die("unwind depth exceeded", h, pc);
}

void I____cxa_allocate_exception(CPU *c) {
    uint32_t n = ARG(0), h = gmalloc(n + HDR);
    memset(G2H(h), 0, n + HDR);
    wr32(h, EXC_MAGIC);
    c->eax = h + HDR;
}
void I____cxa_free_exception(CPU *c) {
    gfree(ARG(0) - HDR);
}
void I____cxa_throw(CPU *c) { // (obj, typeinfo, destructor)
    uint32_t h = ARG(0) - HDR;
    wr32(h + 4, ARG(1));
    wr32(h + 8, ARG(2));
    wr32(h + 12, 0);
    if (eh_debug < 0)
        eh_debug = getenv("RT32_EH_DEBUG") != NULL;
    if (eh_debug)
        fprintf(stderr, "[eh] throw %s from %s\n", ti_name(ARG(1)), rt_func_name(rd32(c->esp)));
    unwind(c, h, rd32(c->esp), c->ebp, c->esp + 4);
}
void I___Unwind_Resume(CPU *c) {
    unwind(c, ARG(0), rd32(c->esp), c->ebp, c->esp + 4);
} // (exception) from a cleanup pad
void I____cxa_begin_catch(CPU *c) {
    uint32_t h = ARG(0);
    if (rd32(h) != EXC_MAGIC) {
        c->eax = 0;
        fprintf(stderr, "[eh] begin_catch on foreign exception 0x%x\n", h);
        return;
    }
    wr32(h + 12, rd32(h + 12) + 1);
    if (ncaught < 64)
        caught[ncaught++] = h;
    c->eax = h + HDR;
}
void I____cxa_end_catch(CPU *c) {
    if (!ncaught)
        return;
    uint32_t h = caught[ncaught - 1];
    uint32_t n = rd32(h + 12) - 1;
    wr32(h + 12, n);
    if (n == 0) {
        ncaught--;
        if (!rd32(h + 20)) { // not being rethrown: destroy
            uint32_t dest = rd32(h + 8), obj = h + HDR;
            if (dest)
                guest_call(c, dest, 1, &obj);
            gfree(h);
        } else
            wr32(h + 20, 0);
    }
}
void I____cxa_rethrow(CPU *c) {
    if (!ncaught)
        die("rethrow with no active exception", 0, rd32(c->esp));
    uint32_t h = caught[ncaught - 1];
    wr32(h + 20, 1); // rethrown: the end_catch run by the handler's cleanup must not destroy it
    unwind(c, h, rd32(c->esp), c->ebp, c->esp + 4);
}
void I____cxa_get_exception_ptr(CPU *c) {
    c->eax = ARG(0) + HDR;
}
void I____cxa_call_unexpected(CPU *c) {
    die("exception violated an exception specification", ARG(0), rd32(c->esp));
}
void I___ZSt9terminatev(CPU *c) {
    die("std::terminate called", 0, rd32(c->esp));
}

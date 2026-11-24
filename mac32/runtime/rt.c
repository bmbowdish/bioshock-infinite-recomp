// ===== Added by Claude Fable - 2026-11-24 — index of changes in this file =====
//  * rt_heap_used_mb()   : how far the game's heap has grown, for the [mem] log line and the overlay.
//  * A note where the PhysX "fire bomb" crash-hunting helpers used to be (removed once the real cause was fixed).
// =====================================================================
#include <pthread.h>
#include <execinfo.h>
// Core runtime: guest address space, image loader, dispatch, heap, setjmp.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

_Thread_local CPU *rt_cur;
// ---- RT32_MEMWATCH: catch guest writes to an address range (debug) ----
static uint32_t mw_addr, mw_len = 8, mw_armed, mw_hits;
static uint8_t *mw_page; static size_t mw_pagesz;
uint8_t rt_watch[0x2000000 / 8];   // one bit per guest code address; must cover all of __text (BioShock Infinite: up to 0x13ec120)
static void mw_protect(int on) { mprotect(mw_page, mw_pagesz, on ? PROT_READ : PROT_READ | PROT_WRITE); }
static int mw_reprotect_pending;
void rt_memwatch_tick(void) { if (mw_reprotect_pending) { mw_reprotect_pending = 0; mw_protect(1); } }
// Full guest state on any fatal path: registers, thread, raw stack, raw ebp chain, bytes near esi, frame.
void rt_fatal_dump(CPU *c, const char *why) {
    if (!c) return;
    extern volatile int host_present_count; extern void rt_thread_desc(CPU *, char *, size_t);
    char td[160]; rt_thread_desc(c, td, sizeof td);
    fprintf(stderr, "\n[fatal] %s | present #%d | %s | host thread %p\n", why, host_present_count, td, (void *)pthread_self());
    fprintf(stderr, "[fatal] eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x esp=%08x ebp=%08x fn=%s (0x%x) stack=[%08x,%08x)\n",
            c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->esp, c->ebp, rt_func_name(c->fn), c->fn, c->stack_lo, c->stack_hi);
    fprintf(stderr, "[fatal] stack words from esp:");
    for (int i = 0; i < 64 && c->esp + 4u * i < 0xF0000000u; i++) { if (i % 8 == 0) fprintf(stderr, "\n  %08x:", c->esp + 4 * i); fprintf(stderr, " %08x", rd32(c->esp + 4 * i)); }
    fprintf(stderr, "\n[fatal] ebp chain (frame, ret, name):");
    uint32_t f = c->ebp; int k = 0;
    for (; k < 24 && f > 0x1000 && f < 0xF0000000u; k++) {
        uint32_t nf = rd32(f), ret = rd32(f + 4);
        fprintf(stderr, "\n  %08x ret %08x %s", f, ret, rt_func_name(ret));
        if (nf <= f || (c->stack_hi && (nf < c->stack_lo || nf >= c->stack_hi))) { fprintf(stderr, "\n  chain breaks: next frame %08x", nf); break; }
        f = nf;
    }
    uint32_t a = (c->esi & ~3u) - 0x40;
    if (a > 0x1000 && a < 0xF0000000u) {
        fprintf(stderr, "\n[fatal] bytes around esi&~3 (%08x):", c->esi & ~3u);
        for (int i = 0; i < 0x100; i += 4) { if (i % 32 == 0) fprintf(stderr, "\n  %08x:", a + i); fprintf(stderr, " %08x", rd32(a + i)); }
    }
    fprintf(stderr, "\n"); fflush(stderr);
}
static void crash_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t fa = (uintptr_t)si->si_addr;
    { extern int gl_dirty_fault(uintptr_t); if ((sig == SIGSEGV || sig == SIGBUS) && gl_dirty_fault(fa)) return; }   // GL buffer dirty-page tracking
    if (mw_armed && (uint8_t *)fa >= mw_page && (uint8_t *)fa < mw_page + mw_pagesz) {
        uint32_t g = (uint32_t)(fa - (uintptr_t)M);
        if (g >= mw_addr && g < mw_addr + mw_len) {
            CPU *c = rt_cur;
            fprintf(stderr, "[memwatch] write to 0x%x by %s (0x%x), caller %s\n", g, c ? rt_func_name(c->fn) : "?", c ? c->fn : 0,
                    c ? rt_func_name(rd32(c->esp)) : "?");
            if (++mw_hits >= 12 || getenv("RT32_MEMWATCH_ONESHOT")) {
                fprintf(stderr, "[memwatch] guest esp=%x ebp=%x eax=%x ecx=%x edx=%x esi=%x edi=%x; stack:", c->esp, c->ebp, c->eax, c->ecx, c->edx, c->esi, c->edi);
                for (uint32_t f = c->ebp, k = 0; f && k < 12; f = rd32(f), k++) fprintf(stderr, "\n    %s", rt_func_name(rd32(f + 4)));
                fprintf(stderr, "\n");
                signal(sig, SIG_DFL); raise(SIGABRT);
            }
        }
        mw_protect(0); mw_reprotect_pending = 1;   // let this write through; re-arm at the next function entry
        return;
    }
    CPU *c = rt_cur;
    fprintf(stderr, "\n[rt] signal %d at host %p", sig, si->si_addr);
    if (fa >= (uintptr_t)M && fa < (uintptr_t)M + (1ull << 32)) fprintf(stderr, " (guest 0x%x)", (uint32_t)(fa - (uintptr_t)M));
    if (c) rt_fatal_dump(c, "host signal");
    if (c) fprintf(stderr, "\n[rt] in guest %s (0x%x) eax=%x ecx=%x edx=%x ebx=%x esp=%x ebp=%x esi=%x edi=%x\n",
                   rt_func_name(c->fn), c->fn, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (c) {   // guest call chain (ebp frames), then the host stack (shows which runtime/shim code faulted)
        fprintf(stderr, "[rt] guest stack:");
        for (uint32_t f = c->ebp, k = 0; f > 0x1000 && f < 0xF0000000u && k < 16; f = rd32(f), k++) fprintf(stderr, "\n    %s (ret 0x%x)", rt_func_name(rd32(f + 4)), rd32(f + 4));
        fprintf(stderr, "\n");
    }
    { void *bt[32]; int n = backtrace(bt, 32); fprintf(stderr, "[rt] host stack:\n"); backtrace_symbols_fd(bt, n, 2); }
    signal(sig, SIG_DFL);
    raise(sig);
}

uint8_t *M;

#define STACK_TOP  0xF0000000u
#define HEAP_BASE  0x10000000u
#define HEAP_END   0xE0000000u

void rt_shims_init(void);

int32_t cvtt_i32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;  // x86 "integer indefinite"
    return (int32_t)v;
}

const char *rt_func_name(uint32_t a);
static const char *func_name(uint32_t a) { return rt_func_name(a); }
const char *rt_func_name(uint32_t a) {
    int lo = 0, hi = ngfuncs - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (gfuncs[m].addr <= a) { best = m; lo = m + 1; } else hi = m - 1; }
    return best >= 0 ? gfuncs[best].name : "?";
}

static int lax_mode = -1;
void unimpl_import(CPU *c, const char *name) {
    if (lax_mode < 0) lax_mode = getenv("RT32_STRICT") == NULL;   // players: log and continue; RT32_STRICT=1: abort
    if (lax_mode) {
        static const char *seen[4096]; static int nseen;
        for (int i = 0; i < nseen; i++) if (seen[i] == name) { c->eax = 0; return; }
        if (nseen < 4096) seen[nseen++] = name;
        fprintf(stderr, "[lax] %s (from %s) -> 0\n", name, func_name(rd32(C.esp)));
        c->eax = 0;
        return;
    }
    fprintf(stderr, "[rt] unimplemented import %s (called from guest, ret=0x%x in %s)\n", name, rd32(C.esp), func_name(rd32(C.esp)));
    abort();
}
void missing_func(CPU *c, uint32_t addr, uint32_t from) {
    fprintf(stderr, "[rt] call to untranslated function 0x%x from 0x%x (in %s)\n", addr, from, func_name(from));
    abort();
}
void bad_jump(CPU *c, uint32_t t, uint32_t from) {
    fprintf(stderr, "[rt] bad indirect jump to 0x%x at 0x%x (%s)\n", t, from, func_name(from));
    abort();
}
void bad_insn(CPU *c, uint32_t a) {
    fprintf(stderr, "[rt] unsupported/ud2 instruction at 0x%x (%s)\n", a, func_name(a));
    abort();
}

// ---- native functions callable from guest code through synthetic addresses ----
#define HOSTFN_BASE 0xFF000000u
#define NHOSTFN 65536
static gfn hostfns[NHOSTFN];
static void (*hostfns_ctx[NHOSTFN])(CPU *, uint32_t);
static uint32_t hostctx[NHOSTFN];
static int nhostfn;
uint32_t rt_host_fn(gfn f) {
    int i = __atomic_fetch_add(&nhostfn, 1, __ATOMIC_SEQ_CST);
    hostfns[i] = f;
    return HOSTFN_BASE + 16u * (uint32_t)i;
}
uint32_t rt_host_fn_ctx(void (*f)(CPU *, uint32_t), uint32_t ctx) {
    int i = __atomic_fetch_add(&nhostfn, 1, __ATOMIC_SEQ_CST);
    hostfns_ctx[i] = f; hostctx[i] = ctx;
    return HOSTFN_BASE + 16u * (uint32_t)i;
}

// RT32_DUMP_MEM="addr:len,addr:len": hex + text dump of guest memory when the run stops (debug aid)
static void dump_mem(void) {
    const char *e = getenv("RT32_DUMP_MEM"); if (!e) return;
    char b[1024]; snprintf(b, sizeof b, "%s", e);
    for (char *tok = strtok(b, ","); tok; tok = strtok(NULL, ",")) {
        uint32_t a = (uint32_t)strtoul(tok, NULL, 16), n = 64; char *colon = strchr(tok, ':'); if (colon) n = (uint32_t)strtoul(colon + 1, NULL, 16);
        for (uint32_t o = 0; o < n && o < 4096; o += 16) {
            fprintf(stderr, "  [mem] %08x:", a + o);
            for (uint32_t k = 0; k < 16; k += 4) fprintf(stderr, " %08x", rd32(a + o + k));
            fprintf(stderr, "  ");
            for (uint32_t k = 0; k < 16; k++) { uint8_t ch = M[a + o + k]; fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
            fputc('\n', stderr);
        }
    }
}
// RT32_DIAG_RET1="<fn>:<until fn>" (diagnostic only): indirect calls to fn return 1 without running it,
// until the second function is first entered (then fn runs normally again)
uint32_t dr_fn, dr_until; volatile int dr_off;
static uint32_t pe_getname; static void pe_name(CPU *c, uint32_t obj, char *out, size_t n);
static void dr_init(void) {
    const char *s = getenv("RT32_DIAG_RET1"); if (!s) return;
    char *e; dr_fn = (uint32_t)strtoul(s, &e, 16); if (*e == ':') dr_until = (uint32_t)strtoul(e + 1, NULL, 16);
}
void call_ind(CPU *c, uint32_t t) {
    if (__builtin_expect(t == dr_fn && dr_fn && !dr_off, 0)) {
        static int n; if (!n++) fprintf(stderr, "[diag] %s forced to return 1 until %s\n", rt_func_name(t), rt_func_name(dr_until));
        C.eax = 1; C.esp += 4; return;
    }
    if (t >= HOSTFN_BASE && t < HOSTFN_BASE + 16u * NHOSTFN) {
        uint32_t i = (t - HOSTFN_BASE) / 16;
        if (hostfns[i]) hostfns[i](c); else hostfns_ctx[i](c, hostctx[i]);
        C.esp += 4; return;
    }
    int lo = 0, hi = ngfuncs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gfuncs[m].addr == t) { gfuncs[m].fn(c); return; }
        if (gfuncs[m].addr < t) lo = m + 1; else hi = m - 1;
    }
    lo = 0; hi = ngstubs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (gstubs[m].addr == t) { gstubs[m].fn(c); C.esp += 4; return; }
        if (gstubs[m].addr < t) lo = m + 1; else hi = m - 1;
    }
    fprintf(stderr, "[rt] indirect call to unknown 0x%x (ret=0x%x) eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x\n  stack:", t, rd32(C.esp), C.eax, C.ebx, C.ecx, C.edx, C.esi, C.edi);
    rt_fatal_dump(c, "indirect call to unknown target");
    if (getenv("RT32_DUMP_CALL_OBJ")) {   // debug: the first argument as a C++ object (vtable, slots, raw words)
        uint32_t o = rd32(C.esp + 4), vt = o > 0x1000 && o < 0xF0000000u ? rd32(o) : 0;
        if (getenv("RT32_DUMP_GLOBALS")) {   // "addr,addr,...": raw words at the fault
            char b[256]; snprintf(b, sizeof b, "%s", getenv("RT32_DUMP_GLOBALS"));
            for (char *t = strtok(b, ","); t; t = strtok(NULL, ",")) { uint32_t ad = (uint32_t)strtoul(t, NULL, 16); fprintf(stderr, "\n  [0x%x] = 0x%x", ad, rd32(ad)); }
        }
        fprintf(stderr, "\n  arg0 0x%x vtable 0x%x (%s)\n  words:", o, vt, vt ? rt_func_name(vt) : "-");
        for (int i = 0; o > 0x1000 && o < 0xF0000000u && i < 16; i++) fprintf(stderr, " %08x", rd32(o + 4 * i));
        fprintf(stderr, "\n  slots:");
        for (int i = 0; vt > 0x1000 && vt < 0xF0000000u && i < 12; i++) fprintf(stderr, " [%d]%s", i, rt_func_name(rd32(vt + 4 * i)));
        fprintf(stderr, "\n");
        // RT32_DUMP_CALL_OBJ=<GetName hex>: also name the object and the UObjects its words point at (UE3 debugging)
        uint32_t gn = (uint32_t)strtoul(getenv("RT32_DUMP_CALL_OBJ"), NULL, 16);
        if (gn > 1 && o > 0x1000) {
            pe_getname = gn;
            { char nm[128]; pe_name(c, o, nm, sizeof nm); fprintf(stderr, "  object name '%s'\n", nm); }
            for (int i = 1; i < 16; i++) {
                uint32_t w = rd32(o + 4 * i); if (w < 0x10000000u || w >= 0xF0000000u) continue;
                uint32_t wv = rd32(w); if (wv < 0x1000000u || wv > 0x1d00000u) continue;   // only things whose first word looks like a vtable in __DATA
                char nm[128]; pe_name(c, w, nm, sizeof nm); fprintf(stderr, "  word[%d] 0x%x -> UObject '%s' (vtable %s)\n", i, w, nm, rt_func_name(wv));
            }
        }
    }
    for (uint32_t f = C.ebp, k = 0; f > 0x1000 && k < 12; f = rd32(f), k++) fprintf(stderr, "\n    %s", rt_func_name(rd32(f + 4)));
    fprintf(stderr, "\n");
    dump_mem();
    if (getenv("RT32_DUMP_PTRS")) {   // debug: "addr,addr,..." -> print *addr, **addr, ***addr
        char b[512]; snprintf(b, sizeof b, "%s", getenv("RT32_DUMP_PTRS"));
        for (char *tok = strtok(b, ","); tok; tok = strtok(NULL, ",")) {
            uint32_t a = (uint32_t)strtoul(tok, NULL, 16), v1 = rd32(a), v2 = v1 > 0x1000 ? rd32(v1) : 0, v3 = v2 > 0x1000 ? rd32(v2) : 0;
            fprintf(stderr, "  [dump] *%x=%x **=%x ***=%x (%s)\n", a, v1, v2, v3, v3 ? rt_func_name(v3) : "-");
        }
    }
    abort();
}

uint32_t guest_func(const char *sym) {
    for (int i = 0; i < ngfuncs; i++) if (!strcmp(gfuncs[i].name, sym)) return gfuncs[i].addr;
    fprintf(stderr, "[rt] no guest function %s\n", sym); abort();
}

uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args) {
    CPU *saved = rt_cur; rt_cur = c;
    // Mac i386 ABI: esp is a multiple of 16 at every call instruction (after the arguments are pushed).
    // Compiled code relies on it (e.g. OR-for-ADD addressing of aligned locals in
    // FMaterialUniformExpressionComponentMask::GetNumberValue), so align here and restore the caller's esp after.
    uint32_t sp0 = C.esp;
    C.esp = ((C.esp - 4u * (uint32_t)nargs) & ~15u) + 4u * (uint32_t)nargs;
    for (int i = nargs - 1; i >= 0; i--) PUSH(args[i]);
    PUSH(0xDEAD0000u);
    call_ind(c, addr);
    C.esp = sp0;
    rt_cur = saved;
    return C.eax;
}

// ---- MEMCHECK builds: report guest writes into [RT32_MC_LO, RT32_MC_HI) once armed ----
uint32_t rt_mc_lo = 0, rt_mc_hi = 0;
static uint32_t mc_lo_cfg, mc_hi_cfg, mc_arm_fn;
void rt_mc_hit(uint32_t a, uint32_t n) {
    CPU *c = rt_cur;
    fprintf(stderr, "[memcheck] %u-byte write to 0x%x in %s\n", n, a, c ? rt_func_name(c->fn) : "?");
    if (c) {
        fprintf(stderr, "    called from:");
        for (uint32_t f = c->ebp, k = 0; f > 0x1000 && k < 10; f = rd32(f), k++) fprintf(stderr, "\n      %s", rt_func_name(rd32(f + 4)));
        fprintf(stderr, "\n");
    }
    static int hits; if (++hits >= (getenv("RT32_MC_MAX") ? atoi(getenv("RT32_MC_MAX")) : 1)) abort();
}

// ---- RT32_WATCH: log entry into selected guest functions ----
#include <stdio.h>
extern char *__cxa_demangle(const char *, char *, size_t *, int *);
static uint32_t mw_arm_fn;
// RT32_WATCH_STATE="fn:off<b|w|f>,off<b|w|f>,...": on each entry of guest function fn (hex), read fields of
// its first argument (this) and log them only when they change (state-machine tracing without log floods)
static uint32_t ws_fn, ws_arg = 1; static int ws_n; static uint32_t ws_off[8]; static char ws_ty[8]; static uint32_t ws_last[8]; static uint32_t ws_this; static int ws_have;
static void ws_init(void) {
    const char *s = getenv("RT32_WATCH_STATE"); if (!s) return;
    char buf[256]; snprintf(buf, sizeof buf, "%s", s);
    char *colon = strchr(buf, ':'); if (!colon) return; *colon = 0;
    { char *at; ws_fn = (uint32_t)strtoul(buf, &at, 16); if (*at == '@') ws_arg = (uint32_t)atoi(at + 1); }   // fn@N: base = Nth stack arg
    for (char *tok = strtok(colon + 1, ","); tok && ws_n < 8; tok = strtok(NULL, ",")) {
        char *e; ws_off[ws_n] = (uint32_t)strtoul(tok, &e, 16); if (*e == '/') e++;   // "8/b": offset 8, byte ("8b" alone would read as hex 0x8b)
        ws_ty[ws_n] = *e ? *e : 'w'; ws_n++;
    }
    rt_watch[ws_fn >> 3] |= (uint8_t)(1u << (ws_fn & 7));
}
static int ws_step(CPU *c, uint32_t a) {
    if (!ws_fn || a != ws_fn) return 0;
    uint32_t self = rd32(c->esp + 4 * ws_arg), v[8]; int changed = !ws_have || self != ws_this;
    for (int i = 0; i < ws_n; i++) {
        uint32_t ad = self + ws_off[i];
        v[i] = ws_ty[i] == 'b' ? rd8(ad) : rd32(ad);
        if (v[i] != ws_last[i]) changed = 1;
    }
    static int budget = 400;
    if (changed && budget-- > 0) {
        extern volatile int host_present_count;
        fprintf(stderr, "[state] %s this=%08x present#%d:", rt_func_name(a), self, host_present_count);
        for (int i = 0; i < ws_n; i++) {
            if (ws_ty[i] == 'f') { float f; memcpy(&f, &v[i], 4); fprintf(stderr, " +%x=%g", ws_off[i], f); }
            else fprintf(stderr, " +%x=%u", ws_off[i], v[i]);
        }
        fprintf(stderr, "\n");
    }
    memcpy(ws_last, v, sizeof v); ws_this = self; ws_have = 1;
    return 1;
}
// RT32_PE_TRACE="<ProcessEvent hex>:<GetName hex>": log each distinct (function, object) pair dispatched
// through UObject::ProcessEvent(this, UFunction*, Parms, Result), first time only, with the present number.
// GetName is FString GetName() const: hidden return pointer first, callee pops it (ret 4).
static uint32_t pe_fn; static int pe_busy;
static void pe_init(void) {
    const char *s = getenv("RT32_PE_TRACE"); if (!s) return;
    char *e; pe_fn = (uint32_t)strtoul(s, &e, 16); if (*e == ':') pe_getname = (uint32_t)strtoul(e + 1, NULL, 16);
    if (pe_fn && pe_getname) rt_watch[pe_fn >> 3] |= (uint8_t)(1u << (pe_fn & 7));
}
static void pe_name(CPU *c, uint32_t obj, char *out, size_t n) {
    out[0] = 0; if (!obj) { snprintf(out, n, "null"); return; }
    uint32_t esp0 = C.esp, sret = gmalloc(16); wr32(sret, 0); wr32(sret + 4, 0); wr32(sret + 8, 0);
    uint32_t args[2] = { sret, obj }; guest_call(c, pe_getname, 2, args); C.esp = esp0;
    uint32_t data = rd32(sret), num = rd32(sret + 4); size_t k = 0;
    for (uint32_t i = 0; data && i < num && k + 1 < n; i++) { uint32_t ch = rd32(data + 4 * i); if (!ch) break; out[k++] = ch < 128 ? (char)ch : '?'; }
    out[k] = 0; gfree(sret);   // the FString's own buffer is leaked (debug only)
}
static int pe_step(CPU *c, uint32_t a) {
    if (!pe_fn || a != pe_fn || pe_busy) return 0;
    uint32_t self = rd32(c->esp + 4), fn = rd32(c->esp + 8);
    static uint32_t seen[65536]; uint32_t h = ((fn * 2654435761u) ^ (self >> 4) * 40503u) & 65535, i = h;
    uint32_t key = fn ^ (self * 31u);
    while (seen[i] && seen[i] != key) { i = (i + 1) & 65535; if (i == h) return 1; }
    if (seen[i]) return 1;
    seen[i] = key;
    static int budget = 3000; if (budget-- <= 0) return 1;
    pe_busy = 1;
    char fname[128], oname[128]; pe_name(c, fn, fname, sizeof fname); pe_name(c, self, oname, sizeof oname);
    pe_busy = 0;
    extern volatile int host_present_count;
    fprintf(stderr, "[event] present#%d %s.%s\n", host_present_count, oname, fname);
    return 1;
}
// RT32_DIAG_CALL="<hook fn>:<target fn>:<present#>" (diagnostic only): the first time the hook function is
// entered at or after that present, call target(this) once from there (this = the hook's first argument)
static uint32_t dc_hook, dc_target; static int dc_at = -1, dc_done;
static void dc_init(void) {
    const char *s = getenv("RT32_DIAG_CALL"); if (!s) return;
    char *e; dc_hook = (uint32_t)strtoul(s, &e, 16); if (*e != ':') return;
    dc_target = (uint32_t)strtoul(e + 1, &e, 16); if (*e != ':') return; dc_at = atoi(e + 1);
    rt_watch[dc_hook >> 3] |= (uint8_t)(1u << (dc_hook & 7));
}
static void dc_step(CPU *c, uint32_t a) {
    extern volatile int host_present_count;
    if (dc_done || a != dc_hook || dc_at < 0 || host_present_count < dc_at) return;
    dc_done = 1;
    uint32_t self = rd32(c->esp + 4), esp0 = C.esp, args[1] = { self };
    fprintf(stderr, "[diag] present#%d: calling %s(0x%x) from %s\n", host_present_count, rt_func_name(dc_target), self, rt_func_name(a));
    guest_call(c, dc_target, 1, args); C.esp = esp0;
    fprintf(stderr, "[diag] returned\n");
}
// RT32_DIAG_POKE="<hook fn>:<offset>:<value>:<present#>" (diagnostic only): once, write a 32-bit value at this+offset
static uint32_t dp_hook, dp_off, dp_val; static int dp_at = -1, dp_done;
static void dp_init(void) {
    const char *s = getenv("RT32_DIAG_POKE"); if (!s) return;
    char *e; dp_hook = (uint32_t)strtoul(s, &e, 16); if (*e != ':') return;
    dp_off = (uint32_t)strtoul(e + 1, &e, 16); if (*e != ':') return;
    dp_val = (uint32_t)strtoul(e + 1, &e, 0); if (*e != ':') return; dp_at = atoi(e + 1);
    rt_watch[dp_hook >> 3] |= (uint8_t)(1u << (dp_hook & 7));
}
static void dp_step(CPU *c, uint32_t a) {
    extern volatile int host_present_count;
    if (dp_done || a != dp_hook || dp_at < 0 || host_present_count < dp_at) return;
    dp_done = 1; uint32_t self = rd32(c->esp + 4);
    fprintf(stderr, "[diag] present#%d: [0x%x+0x%x] %u -> %u\n", host_present_count, self, dp_off, rd32(self + dp_off), dp_val);
    wr32(self + dp_off, dp_val);
}
// RT32_SLOT_WATCH="<array global>:<index>:<GetName>:<hook fn>[,<hook fn>...]" (all hex): on entry of each hook
// function, look at element <index> of the pointer array whose data pointer is stored at <array global>
// (e.g. GObjObjects) and log when the slot's object changes, or when that object's first word changes.
static uint32_t sw_last_obj_pub;   // object under RT32_SLOT_WATCH: report when it is freed
static uint32_t sw_glob, sw_idx, sw_last_obj, sw_last_w0, sw_prev_fn; static int sw_on;
static void sw_init(void) {
    const char *s = getenv("RT32_SLOT_WATCH"); if (!s) return;
    char *e; sw_glob = (uint32_t)strtoul(s, &e, 16); if (*e != ':') return;
    sw_idx = (uint32_t)strtoul(e + 1, &e, 16); if (*e != ':') return;
    pe_getname = (uint32_t)strtoul(e + 1, &e, 16); if (*e != ':') return;
    for (char *q = e + 1; *q; ) { uint32_t f = (uint32_t)strtoul(q, &q, 16); rt_watch[f >> 3] |= (uint8_t)(1u << (f & 7)); if (*q == ',') q++; else break; }
    sw_on = 1;
}
static void sw_stack(CPU *c) {
    fprintf(stderr, "    stack:");
    for (uint32_t f = C.ebp, k = 0; f > 0x1000 && f < 0xF0000000u && k < 10; f = rd32(f), k++) fprintf(stderr, " %s", rt_func_name(rd32(f + 4)));
    fprintf(stderr, "\n");
}
static void sw_step(CPU *c, uint32_t a) {
    if (!sw_on) return;
    static int busy, budget = 300; if (busy || budget <= 0) return;
    uint32_t arr = rd32(sw_glob); if (arr < 0x1000) return;
    uint32_t obj = rd32(arr + 4 * sw_idx);
    extern volatile int host_present_count;
    busy = 1;
    if (obj != sw_last_obj) {
        budget--;
        fprintf(stderr, "[slot] present#%d at entry of %s: slot 0x%x now 0x%x (was 0x%x)\n", host_present_count, rt_func_name(a), sw_idx, obj, sw_last_obj);
        if (obj > 0x1000 && obj < 0xF0000000u) {
            fprintf(stderr, "    words:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", rd32(obj + 4 * i)); fprintf(stderr, "\n");
            char nm[128], cn[128] = "-"; pe_name(c, obj, nm, sizeof nm);
            uint32_t cls = rd32(obj + 0x20); if (cls > 0x1000 && cls < 0xF0000000u) pe_name(c, cls, cn, sizeof cn);
            fprintf(stderr, "    name '%s' class '%s' vtable %s\n", nm, cn, rt_func_name(rd32(obj)));
        }
        sw_stack(c);
        sw_last_obj = obj; sw_last_obj_pub = obj; sw_last_w0 = obj > 0x1000 && obj < 0xF0000000u ? rd32(obj) : 0;
    } else if (obj > 0x1000 && obj < 0xF0000000u && rd32(obj) != sw_last_w0) {
        budget--;
        // RT32_SLOT_FINE: after the first change, check at every function entry to find the writing function
        static int fine; if (!fine && getenv("RT32_SLOT_FINE")) { fine = 1; memset(rt_watch, 0xFF, 0x2000000 / 8); budget = 60;
            fprintf(stderr, "[slot] fine mode: checking at every function entry\n"); }
        if (fine && budget <= 1) { fprintf(stderr, "[slot] enough data, exiting\n"); fflush(stderr); _exit(3); }
        fprintf(stderr, "[slot] present#%d at entry of %s (previous entry %s): object 0x%x word0 0x%x -> 0x%x\n", host_present_count, rt_func_name(a),
                rt_func_name(sw_prev_fn), obj, sw_last_w0, rd32(obj));
        sw_stack(c);
        sw_last_w0 = rd32(obj);
    }
    busy = 0;
}
// RT32_FENCE_AT="fn,fn": full memory barrier at entry of these guest functions (x86 store-ordering proof tests)
// RT32_ABORT_ON="fn,fn": stop the process at entry of these functions (e.g. the engine's critical-error handler)
static uint32_t fence_fns[8], abort_fns[8]; static int nfence, nabort;
static void list_init(const char *env, uint32_t *out, int *n) {
    const char *s = getenv(env); if (!s) return;
    for (const char *q = s; *q && *n < 8; ) { uint32_t f = (uint32_t)strtoul(q, (char **)&q, 16); out[(*n)++] = f; rt_watch[f >> 3] |= (uint8_t)(1u << (f & 7)); if (*q == ',') q++; else break; }
}
// RT32_MSG_CAPTURE="<fn>": remember argument 2 (an output struct pointer) at each entry of fn; cf.c's
// ReleaseEvent prints that struct, i.e. what fn produced from the event (BioShock Infinite: sub_1096a, the
// Carbon-event -> Win32-message converter)
uint32_t rt_msg_capture_ptr; static uint32_t msgcap_fn;
static void msgcap_init(void) { const char *s = getenv("RT32_MSG_CAPTURE"); if (!s) return; msgcap_fn = (uint32_t)strtoul(s, NULL, 16); rt_watch[msgcap_fn >> 3] |= (uint8_t)(1u << (msgcap_fn & 7)); }
// Tripwire (always on; RT32_NO_TRIP=1 disables): sub_2abe40 keeps a growable array {data, count, cap}
// at [ebp-0x48]. A crash showed its data pointer 1 too high. Check alignment whenever sub_2abe40 calls its helpers.
static int trip_on;
static void trip_check(CPU *c, uint32_t a) {
    uint32_t ret = rd32(c->esp);   // at callee entry: return address into the caller; ebp is still the caller's frame
    if (ret < 0x2abe40u || ret >= 0x2ad19fu) return;   // only calls made from sub_2abe40's body
    uint32_t d = rd32(c->ebp - 0x48);
    if (d & 3) {
        fprintf(stderr, "[trip] sub_2abe40 array data pointer misaligned: [ebp-0x48]=%08x count=%08x cap=%08x at call to %s (ret %08x)\n",
                d, rd32(c->ebp - 0x44), rd32(c->ebp - 0x40), rt_func_name(a), ret);
        rt_fatal_dump(c, "tripwire: misaligned array pointer in sub_2abe40"); abort();
    }
}
void rt_on_watch(CPU *c, uint32_t a) {
    if (trip_on && (a == 0x2aedecu || a == 0x2ad19fu)) trip_check(c, a);
    if (msgcap_fn && a == msgcap_fn) { rt_msg_capture_ptr = rd32(c->esp + 8); return; }
    for (int i = 0; i < nfence; i++) if (a == fence_fns[i]) { __sync_synchronize(); return; }
    for (int i = 0; i < nabort; i++) if (a == abort_fns[i]) {
        fprintf(stderr, "[rt] stopping at %s (RT32_ABORT_ON)\n", rt_func_name(a));
        for (int k = 1; k <= 2; k++) { uint32_t s = rd32(c->esp + 4 * k); if (s < 0x1000 || s >= 0xF0000000u) continue;
            char b[400]; int i2 = 0; for (; i2 < 399; i2++) { uint32_t ch = rd32(s + 4 * i2); if (!ch || ch > 0xFFFF) break; b[i2] = ch < 128 ? (char)ch : '?'; }
            b[i2] = 0; if (i2 > 2) fprintf(stderr, "    arg%d L\"%s\"\n", k, b); }
        fflush(stderr); abort();
    }
    sw_step(c, a); sw_prev_fn = a;
    if (dr_until && a == dr_until && !dr_off) { extern volatile int host_present_count; dr_off = 1;
        fprintf(stderr, "[diag] present#%d: %s entered; %s back to normal\n", host_present_count, rt_func_name(a), rt_func_name(dr_fn)); }
    dp_step(c, a);
    dc_step(c, a);
    if (pe_step(c, a)) return;
    if (ws_step(c, a)) return;
    if (mc_arm_fn && a == mc_arm_fn) {   // arm MEMCHECK range on entry of the arm function
        static int n; int want = getenv("RT32_MC_ARM_N") ? atoi(getenv("RT32_MC_ARM_N")) : 1;
        if (++n == want) { rt_mc_lo = mc_lo_cfg; rt_mc_hi = mc_hi_cfg; fprintf(stderr, "[memcheck] armed 0x%x-0x%x\n", rt_mc_lo, rt_mc_hi); }
    }
    if (mw_reprotect_pending) rt_memwatch_tick();
    if (mw_arm_fn && a == mw_arm_fn && !mw_armed) {
        /* arm on the first call */
        mw_armed = 1;
        if (!getenv("RT32_MEMWATCH_ONESHOT")) memset(rt_watch, 0xFF, sizeof rt_watch);   // every function entry re-arms protection
        mw_pagesz = (size_t)getpagesize(); mw_page = (uint8_t *)(((uintptr_t)(M + mw_addr)) & ~(mw_pagesz - 1));
        mw_protect(1);
        fprintf(stderr, "[memwatch] armed on 0x%x\n", mw_addr);
        return;
    }
    if (mw_armed) return;

    int st; char *d = __cxa_demangle(rt_func_name(a) + 1, NULL, NULL, &st);
    { extern volatile int host_present_count; static int wb = 2000; if (wb-- <= 0) { free(d); return; }
      fprintf(stderr, "[watch] present#%d ", host_present_count); }
    if (getenv("RT32_WATCH_DUMP_ARG")) {   // "n:words": also print `words` words at the pointer in argument n (1-based)
        int an = 0, nw = 0; sscanf(getenv("RT32_WATCH_DUMP_ARG"), "%d:%d", &an, &nw);
        uint32_t ptr = rd32(c->esp + 4 * an);
        if (ptr > 0x1000 && ptr < 0xF0000000u) { fprintf(stderr, "[watch]   *arg%d:", an); for (int i = 0; i < nw && i < 16; i++) fprintf(stderr, " %08x", rd32(ptr + 4 * i)); fprintf(stderr, "\n"); }
    }
    if (getenv("RT32_WATCH_WSTR")) {   // debug: show the first two arguments as wide (UTF-32) strings when they look like one
        for (int k = 1; k <= 2; k++) { uint32_t s = rd32(c->esp + 4 * k); if (s < 0x1000 || s >= 0xF0000000u) continue;
            char b[300]; int i = 0; for (; i < 299; i++) { uint32_t ch = rd32(s + 4 * i); if (!ch || ch > 0xFFFF) break; b[i] = ch < 128 ? (char)ch : '?'; }
            b[i] = 0; if (i > 2) fprintf(stderr, "[watch]   arg%d L\"%s\"\n", k, b); }
        fprintf(stderr, "[watch]   stack:"); for (uint32_t f = C.ebp, k = 0; f > 0x1000 && f < 0xF0000000u && k < 10; f = rd32(f), k++) fprintf(stderr, " %s", rt_func_name(rd32(f + 4)));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "%s  <- %s  args %08x %08x %08x %08x\n", d ? d : rt_func_name(a),
            rt_func_name(rd32(c->esp)), rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16));
    free(d);
    if (strstr(rt_func_name(a), "St4list") && strstr(rt_func_name(a), "6remove")) {
        uint32_t head = rd32(c->esp + 4), n = rd32(head);
        uint32_t tb = head - 4 + 0x1030;
        fprintf(stderr, "          TextBuffer@%08x:", tb);
        for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", rd32(tb + 4 * k));
        fprintf(stderr, "\n");
        fprintf(stderr, "          around: \"");
        for (uint32_t q = head - 0x6000; q < head - 0x4800; q++) { uint8_t ch = rd8(q); if ((q & 63) == 0) fprintf(stderr, "\n            %08x ", q); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
        fprintf(stderr, "\"\n");
        fprintf(stderr, "          list %08x next %08x prev %08x:", head, rd32(head), rd32(head + 4));
        for (int i = 0; i < 12 && n != head; i++) { fprintf(stderr, " [%08x n=%08x p=%08x]", n, rd32(n), rd32(n + 4)); n = rd32(n); }
        fprintf(stderr, "\n");
    }
    if (strstr(rt_func_name(a), "IntrusiveSet") && strstr(rt_func_name(a), "4find")) {
        // boost::intrusive rbtree: header {parent|color, left, right}; node key (Symbol) at +0x10
        uint32_t set = rd32(c->esp + 8), key = rd32(c->esp + 12);
        uint64_t want = rd64(key);
        uint32_t stack[256]; int sp = 0, n = 0, found = 0, bad = 0;
        uint32_t cur = rd32(set) & ~1u;
        while ((cur || sp) && n < 100000) {
            while (cur && sp < 256) { stack[sp++] = cur; cur = rd32(cur + 4); }
            if (!sp) break;
            cur = stack[--sp]; n++;
            if (rd64(cur + 0x10) == want) found = 1;
            if (cur < 0x1000) { bad = 1; break; }
            cur = rd32(cur + 8);
        }
        fprintf(stderr, "          set %08x: %d keys, key %016llx %s%s\n", set, n, (unsigned long long)want, found ? "PRESENT" : "absent", bad ? " (corrupt)" : "");
    }
    for (int i = 0; i < 6; i++) {   // show arguments that point at printable strings
        uint32_t p = rd32(c->esp + 4 + 4 * i);
        if (p < 0x1000 || p >= 0xF0000000u) continue;
        const uint8_t *s = M + p; int n = 0;
        while (n < 200 && s[n] >= 0x20 && s[n] < 0x7f) n++;
        if (n >= 4 && (s[n] == 0 || s[n] == '\n')) fprintf(stderr, "          arg%d: \"%.*s\"\n", i, n, s);
        else {   // 4-byte wide string (wchar_t on Mac): print ASCII, mark anything else as \x{..}
            int k = 0; char w[200]; int wl = 0, odd = 0;
            for (; k < 60; k++) { uint32_t ch = rd32(p + 4 * k); if (!ch) break; if (ch < 0x20 || ch > 0x10FFFF) { odd = 1; break; }
                wl += (ch < 0x7f) ? snprintf(w + wl, sizeof w - wl, "%c", (char)ch) : snprintf(w + wl, sizeof w - wl, "\\x{%x}", ch); if (wl > 180) break; }
            if (!odd && k >= 2) fprintf(stderr, "          arg%d: L\"%s\"\n", i, w);
        }
    }
}
static void watch_init(void) {
    if (!getenv("RT32_NO_TRIP")) { trip_on = 1; rt_watch[0x2aedec >> 3] |= (uint8_t)(1u << (0x2aedec & 7)); rt_watch[0x2ad19f >> 3] |= (uint8_t)(1u << (0x2ad19f & 7)); }
    if (getenv("RT32_MC_LO") && getenv("RT32_MC_ARM")) {
        mc_lo_cfg = (uint32_t)strtoul(getenv("RT32_MC_LO"), NULL, 16);
        mc_hi_cfg = getenv("RT32_MC_HI") ? (uint32_t)strtoul(getenv("RT32_MC_HI"), NULL, 16) : mc_lo_cfg + 4;
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, getenv("RT32_MC_ARM"))) {
            mc_arm_fn = gfuncs[i].addr; rt_watch[mc_arm_fn >> 3] |= (uint8_t)(1u << (mc_arm_fn & 7)); break;
        }
    }
    if (getenv("RT32_MEMWATCH") && getenv("RT32_MEMWATCH_ARM")) {
        mw_addr = (uint32_t)strtoul(getenv("RT32_MEMWATCH"), NULL, 16);
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, getenv("RT32_MEMWATCH_ARM"))) {
            mw_arm_fn = gfuncs[i].addr; rt_watch[mw_arm_fn >> 3] |= (uint8_t)(1u << (mw_arm_fn & 7)); break;
        }
    }
    ws_init(); pe_init(); dc_init(); dp_init(); dr_init(); sw_init(); list_init("RT32_FENCE_AT", fence_fns, &nfence); list_init("RT32_ABORT_ON", abort_fns, &nabort); msgcap_init();
    if (dr_until) rt_watch[dr_until >> 3] |= (uint8_t)(1u << (dr_until & 7));
    const char *w = getenv("RT32_WATCH");
    if (!w) return;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", w);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        for (int i = 0; i < ngfuncs; i++) if (strstr(gfuncs[i].name, tok)) {
            uint32_t a = gfuncs[i].addr; rt_watch[a >> 3] |= (uint8_t)(1u << (a & 7));
        }
}

// ---- setjmp slots ----
#define NJMP 256
static GJmp jmps[NJMP];
GJmp *gjmp_slot(uint32_t buf) {
    for (int i = 0; i < NJMP; i++) if (jmps[i].guest_buf == buf) return &jmps[i];
    for (int i = 0; i < NJMP; i++) if (!jmps[i].guest_buf) { jmps[i].guest_buf = buf; return &jmps[i]; }
    fprintf(stderr, "[rt] out of jmp slots\n"); abort();
}

// ---- guest heap: power-of-two size classes, 16-byte header keeps 16-byte alignment ----
static uint32_t heap_top = HEAP_BASE;
// The guest heap is one growing region [HEAP_BASE, 0xE0000000) = 3328 MB. heap_top is its high-water mark: the heap
// never shrinks, so this is "the most the game has ever needed", not what is in use right now.
uint32_t rt_heap_used_mb(void) { return (heap_top - HEAP_BASE) >> 20; }   // Claude Fable - 2026-11-24: guest heap high-water mark, for the [mem] line (the guest has a 4 GB ceiling)
static uint32_t freelist[40];
// Header (16 bytes): word0 = size class (<= 20) or 0xFF for large; word1 = free-list link;
// word2 = total block size for large blocks; word3 = state (ALOC / FREE) to catch double/bad frees.
#define BLK_ALLOC 0x434F4C41u
#define BLK_FREE  0x45455246u
#include <pthread.h>
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
#define LARGE 0xFFu
static uint32_t large_free;   // singly linked, first fit
static int size_class(uint32_t n) { int c = 4; while ((1u << c) < n + 16) c++; return c; }
static uint32_t bump(uint32_t sz) {
    uint32_t b = heap_top; heap_top += sz;
    if (heap_top > HEAP_END || heap_top < b) { fprintf(stderr, "[rt] guest heap exhausted (%u bytes)\n", sz); abort(); }
    return b;
}
uint32_t gmalloc(uint32_t n) {
    uint32_t b;
    n = n < 16 ? 16 : (n + 15u) & ~15u;   // like macOS: at least 16 usable bytes, multiples of 16 (malloc(0) callers may write there)
    pthread_mutex_lock(&heap_lock);
    if (n + 16 <= (1u << 20)) {
        int c = size_class(n);
        if (freelist[c]) { b = freelist[c]; freelist[c] = rd32(b + 4); }
        else b = bump(1u << c);
        wr32(b, (uint32_t)c);
    } else {
        uint32_t sz = (n + 16 + 0xFFFu) & ~0xFFFu, *prev = &large_free;
        b = 0;
        for (uint32_t cur = large_free; cur; prev = (uint32_t *)(M + cur + 4), cur = rd32(cur + 4))
            if (rd32(cur + 8) >= sz) { b = cur; *prev = rd32(cur + 4); break; }
        if (!b) { b = bump(sz); wr32(b + 8, sz); }
        wr32(b, LARGE);
    }
    wr32(b + 12, BLK_ALLOC);
    pthread_mutex_unlock(&heap_lock);
    return b + 16;
}
static uint32_t gsize(uint32_t p) {
    uint32_t c = rd32(p - 16);
    return c == LARGE ? rd32(p - 16 + 8) - 16 : (1u << c) - 16;
}
uint32_t gmalloc_usable(uint32_t p) { return p ? gsize(p) : 0; }
void gfree(uint32_t p) {
    if (!p) return;
    if (p == sw_last_obj_pub && rt_cur) {
        CPU *c = rt_cur; extern volatile int host_present_count;
        fprintf(stderr, "[slot] present#%d: watched object 0x%x is being FREED (in %s)\n    stack:", host_present_count, p, rt_func_name(c->fn));
        for (uint32_t f = C.ebp, k = 0; f > 0x1000 && f < 0xF0000000u && k < 14; f = rd32(f), k++) fprintf(stderr, " %s", rt_func_name(rd32(f + 4)));
        fprintf(stderr, "\n");
    }
    uint32_t b = p - 16, c = rd32(b);
    pthread_mutex_lock(&heap_lock);
    uint32_t st = (p >= HEAP_BASE + 16 && p < heap_top) ? rd32(b + 12) : 0;
    if (st != BLK_ALLOC) {
        // macOS would report "pointer being freed was not allocated"; ignoring keeps the heap intact
        static int n; CPU *cur = rt_cur;
        if (n++ < 50 || n % 1000 == 0) fprintf(stderr, "[rt] %s free #%d of 0x%x ignored (in %s, called from %s)\n", st == BLK_FREE ? "double" : "invalid", n, p,
                              cur ? rt_func_name(cur->fn) : "?", cur ? rt_func_name(rd32(cur->esp)) : "?");
        if (0) fprintf(stderr, "[rt] %s free of 0x%x ignored (in %s, called from %s)\n",
                              st == BLK_FREE ? "double" : "invalid", p, cur ? rt_func_name(cur->fn) : "?",
                              cur ? rt_func_name(rd32(cur->esp)) : "?");
        pthread_mutex_unlock(&heap_lock);
        return;
    }
    wr32(b + 12, BLK_FREE);
    if (c == LARGE) { wr32(b + 4, large_free); large_free = b; }
    else { wr32(b + 4, freelist[c]); freelist[c] = b; }
    pthread_mutex_unlock(&heap_lock);
}
uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    if (!n) { gfree(p); return 0; }
    uint32_t have = gsize(p);
    if (have >= n) return p;
    uint32_t q = gmalloc(n);
    memcpy(M + q, M + p, have);
    gfree(p);
    return q;
}
uint32_t gstrdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, p = gmalloc(n);
    memcpy(M + p, s, n);
    return p;
}

// ---- CPUs (one per guest thread), each with its own guest stack ----
static uint32_t stack_next = STACK_TOP;
CPU *cpu_new(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    c->stack_hi = __atomic_fetch_sub(&stack_next, stack_size + 0x10000u, __ATOMIC_SEQ_CST);
    c->stack_lo = c->stack_hi - stack_size;
    c->esp = c->stack_hi - 64;
    c->fcw = 0x37F;
    return c;
}

void cpu_cpuid(CPU *c) { c->eax = 1; c->ebx = c->ecx = c->edx = 0; }

double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static void dump_only(int sig) {
    extern void rt_dump_threads(void);
    fprintf(stderr, "\n[threads] sample (signal %d)\n", sig); rt_dump_threads(); fflush(stderr);
}
static void dump_and_exit(int sig) {
    extern void rt_dump_threads(void);
    fprintf(stderr, "\n[threads] signal %d: guest thread states\n", sig);
    rt_dump_threads(); fflush(stderr); _exit(128 + sig);
}

#include <sys/resource.h>
CPU *rt_init(const char *binpath) {
    { struct rlimit rl; if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {   // macOS default soft limit is 256 open files
        rlim_t want = rl.rlim_max < 10240 ? rl.rlim_max : 10240; if (want > rl.rlim_cur) { rl.rlim_cur = want; setrlimit(RLIMIT_NOFILE, &rl); } } }
    M = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (M == MAP_FAILED) { perror("mmap guest space"); exit(1); }
    FILE *f = fopen(binpath, "rb");
    if (!f) { perror(binpath); exit(1); }
    for (int i = 0; i < ngsegs; i++) {
        const GSeg *s = &gsegs[i];
        fseek(f, s->fo, SEEK_SET);
        if (fread(M + s->va, 1, s->fs, f) != s->fs) { fprintf(stderr, "short read %s\n", s->name); exit(1); }
    }
    fclose(f);
    // Imported function pointers point at their stubs (call_ind resolves them);
    // pure data imports get a zeroed placeholder block until a shim provides the real object.
    for (int i = 0; i < ngptrs; i++) wr32(gptrs[i].addr, rt_import_addr(gptrs[i].name));
    // External relocations: slot += address of symbol (addend is stored in the slot).
    for (int i = 0; i < ngextrel; i++) wr32(gextrel[i].addr, rd32(gextrel[i].addr) + rt_import_addr(gextrel[i].name));
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    signal(SIGTERM, dump_and_exit); signal(SIGUSR1, dump_only); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    rt_shims_init();
    watch_init();
    { extern CPU *rt_main_cpu; extern void rt_prof_start(void); rt_main_cpu = cpu_new(8u << 20); rt_prof_start(); return rt_main_cpu; }
}

// ---- import name -> guest address ----
// BioShock Infinite: data symbols defined inside the game binary resolve to their real address
#define NIMPDATA 16384   // was 512; BioShock Infinite needs more (vtable imports)
static struct { const char *name; uint32_t addr; } impdata[NIMPDATA];
static int nimpdata;
uint32_t rt_import_addr(const char *name) {
    for (int k = 0; k < ngdefs; k++) if (!strcmp(gdefs[k].name, name)) return gdefs[k].addr;
    for (int k = 0; k < ngstubs; k++) if (!strcmp(gstubs[k].name, name)) return gstubs[k].addr;
    for (int i = 0; i < nimpdata; i++) if (!strcmp(impdata[i].name, name)) return impdata[i].addr;
    for (int k = 0; k < ngimports; k++)
        if (!strcmp(gimports[k].name, name) && gimports[k].fn) {   // function referenced only via data (vtables)
            uint32_t a = rt_host_fn(gimports[k].fn);
            if (nimpdata >= NIMPDATA) { fprintf(stderr, "[rt] too many data imports (vtable path)\n"); abort(); }
            impdata[nimpdata].name = name; impdata[nimpdata].addr = a; nimpdata++;
            return a;
        }
    if (nimpdata == NIMPDATA) { fprintf(stderr, "[rt] too many data imports\n"); abort(); }
    uint32_t a = gmalloc(256); memset(M + a, 0, 256);
    impdata[nimpdata].name = name; impdata[nimpdata].addr = a; nimpdata++;
    if (!strncmp(name, "__ZTI", 5)) {
        // imported RTTI object (libstdc++ type): {vptr, name} (+ flags, pointee for pointer types)
        const char *mangled = name + 5;
        wr32(a + 4, gstrdup(mangled));
        if (mangled[0] == 'P') {
            uint32_t flags = 0; const char *p = mangled + 1;
            while (*p == 'K' || *p == 'V') { flags |= (*p == 'K') ? 1u : 2u; p++; }
            char pointee[128]; snprintf(pointee, sizeof pointee, "__ZTI%s", p);
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv119__pointer_type_infoE") + 8);
            wr32(a + 8, flags); wr32(a + 12, rt_import_addr(pointee));
        } else if (strlen(mangled) == 1) {
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv123__fundamental_type_infoE") + 8);
        } else {
            wr32(a, rt_import_addr("__ZTVN10__cxxabiv117__class_type_infoE") + 8);
        }
    }
    return a;
}

// data-import slots are filled by the shim layer
void rt_set_data_import(const char *name, uint32_t guest_addr) {
    for (int i = 0; i < ngptrs; i++) if (!strcmp(gptrs[i].name, name)) wr32(gptrs[i].addr, guest_addr);
}

// ---- PhysX "fire bomb" crash hunt (Claude Fable - 2026-11-24): helpers removed ----
// Five diagnostic helpers lived here (rt_ccd_pre, rt_ccd_post, rt_gjk_overflow, rt_gjk_canary, rt_ccd_esp). They were hand-patched into the
// generated PhysX code while chasing "guest stack smashing detected" crashes. The cause turned out to be the game
// project's starts.txt listing addresses in the MIDDLE of functions as function starts: the recompiler ends a C
// function at the next listed start, and CPU flags (C locals) do not survive across the cut, so branches went wrong.
// Lesson: a function that "corrupts memory" in recompiled code may simply be cut in two. Check starts.txt first.
// Fix and audit: STARTS_AUDIT.md at the top of this repository.

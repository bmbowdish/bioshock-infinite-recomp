// Runtime for statically recompiled i386 code.
// Guest memory is a 4 GiB reservation; guest address a lives at M + a.
// Every translated function / shim receives `CPU *c` (one CPU per guest thread);
// inside them `C` names the current CPU.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

typedef union {
    float f[4];
    double d[2];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    uint16_t w[8];
    uint8_t b[16];
} XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    XMM xmm[8];
    uint64_t mm[8];
    double st[8]; // x87 stack, st[top] = st(0); kept at double precision
    int top;
    uint16_t fcw; // x87 control word (rounding mode used by fistp)
    uint32_t stack_lo, stack_hi;
    uint32_t fn; // guest function currently executing (debug)
} CPU;

typedef void (*gfn)(CPU *c);
extern uint8_t rt_watch[];
void rt_on_watch(struct CPU *c, uint32_t a);
#ifndef NO_TRACE
#define TRACE_FN(a)                                                                                                    \
    do {                                                                                                               \
        c->fn = (a);                                                                                                   \
        if (__builtin_expect(rt_watch[(a) >> 3] & (1u << ((a) & 7)), 0))                                               \
            rt_on_watch(c, (a));                                                                                       \
    } while (0)
#else
#define TRACE_FN(a) ((void)0)
#endif
#define C (*c)

extern uint8_t *M;

#ifdef MEMCHECK
extern uint32_t rt_mc_lo, rt_mc_hi;
void rt_mc_hit(uint32_t a, uint32_t n);
#define MC(a, n)                                                                                                       \
    do {                                                                                                               \
        if (__builtin_expect((a) < rt_mc_hi && (a) + (n) > rt_mc_lo, 0))                                               \
            rt_mc_hit((a), (n));                                                                                       \
    } while (0)
#else
#define MC(a, n) ((void)0)
#endif
// ---- guest memory ----
// x86 is TSO: a thread's stores become visible in program order and loads aren't reordered with other loads.
// ARM64 is weaker, so lock-free guest code (e.g. UE3's render command ring: write the command, then the
// write pointer) breaks with plain accesses. With RT_TSO (default) aligned guest loads are acquire and
// stores release (LDAPR/STLR), unaligned ones are bracketed with fences. Stack accesses (esp/ebp based,
// thread-private) use the plain rds*/wrs* accessors; the generator picks them. Same scheme as runtime/cpu.h.
#ifndef RT_TSO
#define RT_TSO 1
#endif
#define RT_PLAIN(T, n)                                                                                                 \
    static inline __attribute__((always_inline)) T rds##n(uint32_t a) {                                                \
        T v;                                                                                                           \
        memcpy(&v, M + a, sizeof v);                                                                                   \
        return v;                                                                                                      \
    }                                                                                                                  \
    static inline __attribute__((always_inline)) void wrs##n(uint32_t a, T v) {                                        \
        MC(a, sizeof v);                                                                                               \
        memcpy(M + a, &v, sizeof v);                                                                                   \
    }
RT_PLAIN(uint8_t, 8)
RT_PLAIN(uint16_t, 16) RT_PLAIN(uint32_t, 32) RT_PLAIN(uint64_t, 64) static inline float rdsf(uint32_t a) {
    float v;
    memcpy(&v, M + a, 4);
    return v;
}
static inline double rdsd(uint32_t a) {
    double v;
    memcpy(&v, M + a, 8);
    return v;
}
static inline void wrsf(uint32_t a, float v) {
    MC(a, 4);
    memcpy(M + a, &v, 4);
}
static inline void wrsd(uint32_t a, double v) {
    MC(a, 8);
    memcpy(M + a, &v, 8);
}
static inline XMM rdsx(uint32_t a) {
    XMM v;
    memcpy(&v, M + a, 16);
    return v;
}
static inline void wrsx(uint32_t a, XMM v) {
    MC(a, 16);
    memcpy(M + a, &v, 16);
}
#if RT_TSO
#ifdef RT_RELAX_FP // float/vector accesses relaxed; integer flags and locks keep the ordering
#define RT_FP_LD __ATOMIC_RELAXED
#define RT_FP_ST __ATOMIC_RELAXED
#else
#define RT_FP_LD __ATOMIC_ACQUIRE
#define RT_FP_ST __ATOMIC_RELEASE
#endif
#define RT_ORD(T, n, LD, ST)                                                                                           \
    static inline __attribute__((always_inline)) T rdo##n(uint32_t a) {                                                \
        if (__builtin_expect(!(a & (sizeof(T) - 1)), 1))                                                               \
            return __atomic_load_n((T *)(M + a), LD);                                                                  \
        T v;                                                                                                           \
        memcpy(&v, M + a, sizeof v);                                                                                   \
        __atomic_thread_fence(__ATOMIC_ACQUIRE);                                                                       \
        return v;                                                                                                      \
    }                                                                                                                  \
    static inline __attribute__((always_inline)) void wro##n(uint32_t a, T v) {                                        \
        MC(a, sizeof v);                                                                                               \
        if (__builtin_expect(!(a & (sizeof(T) - 1)), 1)) {                                                             \
            __atomic_store_n((T *)(M + a), v, ST);                                                                     \
            return;                                                                                                    \
        }                                                                                                              \
        __atomic_thread_fence(__ATOMIC_RELEASE);                                                                       \
        memcpy(M + a, &v, sizeof v);                                                                                   \
    }
RT_ORD(uint8_t, 8, __ATOMIC_ACQUIRE, __ATOMIC_RELEASE)
RT_ORD(uint16_t, 16, __ATOMIC_ACQUIRE, __ATOMIC_RELEASE)
RT_ORD(uint32_t, 32, __ATOMIC_ACQUIRE, __ATOMIC_RELEASE) RT_ORD(uint64_t, 64, __ATOMIC_ACQUIRE, __ATOMIC_RELEASE)
    RT_ORD(uint32_t, f32, RT_FP_LD, RT_FP_ST) RT_ORD(uint64_t, f64, RT_FP_LD, RT_FP_ST) static inline uint8_t
    rd8(uint32_t a) {
    return rdo8(a);
}
static inline uint16_t rd16(uint32_t a) {
    return rdo16(a);
}
static inline uint32_t rd32(uint32_t a) {
    return rdo32(a);
}
static inline uint64_t rd64(uint32_t a) {
    return rdo64(a);
}
static inline float rdf(uint32_t a) {
    uint32_t u = rdof32(a);
    float v;
    memcpy(&v, &u, 4);
    return v;
}
static inline double rdd(uint32_t a) {
    uint64_t u = rdof64(a);
    double v;
    memcpy(&v, &u, 8);
    return v;
}
static inline void wr8(uint32_t a, uint8_t v) {
    wro8(a, v);
}
static inline void wr16(uint32_t a, uint16_t v) {
    wro16(a, v);
}
static inline void wr32(uint32_t a, uint32_t v) {
    wro32(a, v);
}
static inline void wr64(uint32_t a, uint64_t v) {
    wro64(a, v);
}
static inline void wrf(uint32_t a, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    wrof32(a, u);
}
static inline void wrd(uint32_t a, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    wrof64(a, u);
}
// 128-bit: two ordered 64-bit halves when 8-byte aligned (x86 doesn't promise 16-byte atomicity either)
static inline XMM rdx(uint32_t a) {
    XMM v;
    if (__builtin_expect(!(a & 7), 1)) {
        v.q[0] = __atomic_load_n((uint64_t *)(M + a), RT_FP_LD);
        v.q[1] = __atomic_load_n((uint64_t *)(M + a + 8), RT_FP_LD);
        return v;
    }
    memcpy(&v, M + a, 16);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return v;
}
static inline void wrx(uint32_t a, XMM v) {
    MC(a, 16);
    if (__builtin_expect(!(a & 7), 1)) {
        __atomic_store_n((uint64_t *)(M + a), v.q[0], RT_FP_ST);
        __atomic_store_n((uint64_t *)(M + a + 8), v.q[1], RT_FP_ST);
        return;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(M + a, &v, 16);
}
#else
static inline uint8_t rd8(uint32_t a) {
    return rds8(a);
}
static inline uint16_t rd16(uint32_t a) {
    return rds16(a);
}
static inline uint32_t rd32(uint32_t a) {
    return rds32(a);
}
static inline uint64_t rd64(uint32_t a) {
    return rds64(a);
}
static inline float rdf(uint32_t a) {
    return rdsf(a);
}
static inline double rdd(uint32_t a) {
    return rdsd(a);
}
static inline void wr8(uint32_t a, uint8_t v) {
    wrs8(a, v);
}
static inline void wr16(uint32_t a, uint16_t v) {
    wrs16(a, v);
}
static inline void wr32(uint32_t a, uint32_t v) {
    wrs32(a, v);
}
static inline void wr64(uint32_t a, uint64_t v) {
    wrs64(a, v);
}
static inline void wrf(uint32_t a, float v) {
    wrsf(a, v);
}
static inline void wrd(uint32_t a, double v) {
    wrsd(a, v);
}
static inline XMM rdx(uint32_t a) {
    return rdsx(a);
}
static inline void wrx(uint32_t a, XMM v) {
    wrsx(a, v);
}
#endif
static inline double rd80(uint32_t a) { // x87 80-bit extended -> double
    uint64_t m = rd64(a);
    uint16_t se = rd16(a + 8);
    int e = se & 0x7fff;
    double r;
    if (e == 0x7fff)
        r = ((m << 1) == 0) ? __builtin_inf() : __builtin_nan("");
    else if (m == 0)
        r = 0.0;
    else
        r = __builtin_ldexp((double)m, (e ? e : 1) - 16383 - 63);
    return (se & 0x8000) ? -r : r;
}
static inline void wr80(uint32_t a, double v) { // double -> x87 80-bit extended (10 bytes)
    uint64_t b;
    memcpy(&b, &v, 8);
    uint64_t f = b & ((1ULL << 52) - 1), m;
    int e = (int)((b >> 52) & 0x7ff);
    uint16_t se;
    if (e == 0x7ff) {
        se = 0x7fff;
        m = (1ULL << 63) | (f ? ((1ULL << 62) | (f << 11)) : 0);
    } else if (e == 0) {
        if (f == 0) {
            m = 0;
            se = 0;
        } else {
            int sh = __builtin_clzll(f);
            m = f << sh;
            se = (uint16_t)(15372 - sh);
        }
    } else {
        m = ((1ULL << 52) | f) << 11;
        se = (uint16_t)(e - 1023 + 16383);
    }
    wr64(a, m);
    wr16(a + 8, se | (uint16_t)((b >> 63) << 15));
}

static inline void cpu_push(CPU *c, uint32_t v) {
    c->esp -= 4;
    wrs32(c->esp, v);
}
static inline uint32_t cpu_pop(CPU *c) {
    uint32_t v = rds32(c->esp);
    c->esp += 4;
    return v;
}
#define PUSH(v) cpu_push(c, (v))
#define POP()   cpu_pop(c)

// x87 (only what gcc emits for float returns/conversions)
static inline void cpu_fpush(CPU *c, double v) {
    c->top = (c->top - 1) & 7;
    c->st[c->top] = v;
}
static inline double cpu_fpop(CPU *c) {
    double v = c->st[c->top];
    c->top = (c->top + 1) & 7;
    return v;
}
#define FPUSH(v) cpu_fpush(c, (v))
#define FPOP()   cpu_fpop(c)
#define ST(i)    c->st[(c->top + (i)) & 7]
double fpu_round(CPU *c, double v); // honours fcw rounding control

// guest pointer <-> host pointer
#define G2H(a) ((void *)(M + (uint32_t)(a)))
#define H2G(p) ((uint32_t)((uint8_t *)(p) - M))

// argument i of a cdecl import shim (return address is at esp)
#define ARG(i)     rds32(c->esp + 4 + 4 * (i)) /* stack: plain access */
#define ARGD(slot) rdd(c->esp + 4 + 4 * (slot))
#define ARGF(slot) rdf(c->esp + 4 + 4 * (slot))

void call_ind(CPU *c, uint32_t target); // call (return address already pushed)
void missing_func(CPU *c, uint32_t addr, uint32_t from);
void bad_jump(CPU *c, uint32_t target, uint32_t from);
void bad_insn(CPU *c, uint32_t addr);
int32_t cvtt_i32(double v);
void cpu_cpuid(CPU *c);
const char *rt_func_name(uint32_t addr);
extern _Thread_local CPU *rt_cur;

// C++ exceptions: every translated function with landing pads registers itself on entry. The unwinder longjmps
// back into that function's wrapper, which re-enters the body at the landing pad.
typedef struct EHReg {
    struct EHReg *prev;
    uint32_t fn, esp, lp;
    jmp_buf jb;
} EHReg;
extern _Thread_local EHReg *eh_top;
static inline void eh_enter(CPU *c, EHReg *r, uint32_t fn) {
    while (eh_top && eh_top->esp <= c->esp)
        eh_top = eh_top->prev; // frames at or below this depth are gone
    r->prev = eh_top;
    r->fn = fn;
    r->esp = c->esp;
    r->lp = 0;
    eh_top = r;
}
static inline void eh_leave(EHReg *r) {
    eh_top = r->prev;
}

// setjmp/longjmp support: translated code calls setjmp inline in its own frame
typedef struct {
    uint32_t guest_buf, esp, ebp, ebx, esi, edi, val;
    CPU *cpu;
    jmp_buf host;
} GJmp;
GJmp *gjmp_slot(uint32_t guest_buf);
static inline void gjmp_save(CPU *c, GJmp *j) {
    j->cpu = c;
    j->esp = c->esp;
    j->ebp = c->ebp;
    j->ebx = c->ebx;
    j->esi = c->esi;
    j->edi = c->edi;
}
static inline void gjmp_restore(CPU *c, GJmp *j) {
    c->esp = j->esp;
    c->ebp = j->ebp;
    c->ebx = j->ebx;
    c->esi = j->esi;
    c->edi = j->edi;
    c->eax = j->val;
}

static inline uint32_t rol32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v << n) | (v >> (32 - n)) : v;
}
static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31;
    return n ? (v >> n) | (v << (32 - n)) : v;
}

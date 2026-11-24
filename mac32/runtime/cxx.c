// libstdc++ / C++ ABI shims for the guest (i386 Itanium ABI layouts, 32-bit pointers).
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define RET(v) (c->eax = (uint32_t)(v))

// ---------- static-init guards, atexit, pure virtual ----------
static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;
void I____cxa_guard_acquire(CPU *c) {
    pthread_mutex_lock(&guard_lock);
    if (rd8(ARG(0))) { pthread_mutex_unlock(&guard_lock); RET(0); return; }
    RET(1);   // caller runs the initializer, then calls release (lock held meanwhile)
}
void I____cxa_guard_release(CPU *c) { wr8(ARG(0), 1); pthread_mutex_unlock(&guard_lock); }
void I____cxa_guard_abort(CPU *c) { pthread_mutex_unlock(&guard_lock); }
void I____cxa_atexit(CPU *c) { RET(0); }   // static destructors are not run at exit
void I____cxa_pure_virtual(CPU *c) { fprintf(stderr, "[rt] pure virtual call\n"); abort(); }

extern char *__cxa_demangle(const char *, char *, size_t *, int *);
void I____cxa_demangle(CPU *c) {   // (mangled, buf, len*, status*) -> malloc'd guest string
    int st = 0;
    char *r = __cxa_demangle((const char *)G2H(ARG(0)), NULL, NULL, &st);
    if (ARG(3)) wr32(ARG(3), (uint32_t)st);
    if (!r) { RET(0); return; }
    {   // reproduce GCC 4.2 (libiberty) formatting: nested template closers are written "> >"
        size_t L = strlen(r), k = 0; char *o = malloc(2 * L + 1);
        for (size_t i = 0; i < L; i++) { o[k++] = r[i]; if (r[i] == '>' && r[i + 1] == '>') o[k++] = ' '; }
        o[k] = 0; free(r); r = o;
    }
    if (getenv("RT32_TRACE_DEMANGLE")) fprintf(stderr, "[demangle] %s\n", r);
    uint32_t n = (uint32_t)strlen(r) + 1, g;
    if (ARG(1) && ARG(2) && rd32(ARG(2)) >= n) g = ARG(1);
    else { if (ARG(1)) gfree(ARG(1)); g = gmalloc(n); }
    memcpy(M + g, r, n);
    if (ARG(2)) wr32(ARG(2), n);
    free(r);
    RET(g);
}

static void die(const char *what, CPU *c) {
    fprintf(stderr, "[rt] C++ %s: %s\n", what, ARG(0) ? (char *)G2H(ARG(0)) : "");
    abort();
}
void I___ZSt17__throw_bad_allocv(CPU *c) { die("bad_alloc", c); }
void I___ZSt19__throw_logic_errorPKc(CPU *c) { die("logic_error", c); }
void I___ZSt20__throw_length_errorPKc(CPU *c) { die("length_error", c); }
void I___ZSt20__throw_out_of_rangePKc(CPU *c) { die("out_of_range", c); }

// ---------- std::list node base: {next, prev} ----------
#define NX(n) rd32((n))
#define PV(n) rd32((n) + 4)
#define SNX(n, v) wr32((n), (v))
#define SPV(n, v) wr32((n) + 4, (v))
static int trace_list = -1;
#define TL(...) do { if (trace_list < 0) trace_list = getenv("RT32_TRACE_LIST") != NULL; if (trace_list) fprintf(stderr, __VA_ARGS__); } while (0)
void I___ZNSt15_List_node_base4hookEPS_(CPU *c) {     // this->hook(pos): insert this before pos
    uint32_t self = ARG(0), pos = ARG(1);
    TL("[list] hook node %08x before %08x (from %s)\n", self, pos, rt_func_name(rd32(c->esp)));
    SNX(self, pos); SPV(self, PV(pos)); SNX(PV(pos), self); SPV(pos, self);
}
void I___ZNSt15_List_node_base6unhookEv(CPU *c) {
    uint32_t self = ARG(0), n = NX(self), p = PV(self);
    TL("[list] unhook node %08x (n=%08x p=%08x) (from %s)\n", self, n, p, rt_func_name(rd32(c->esp)));
    SNX(p, n); SPV(n, p);
}
void I___ZNSt15_List_node_base8transferEPS_S0_(CPU *c) {  // this->transfer(first, last): move [first,last) before this
    uint32_t self = ARG(0), first = ARG(1), last = ARG(2);
    if (self == last) return;
    SNX(PV(last), self); SNX(PV(first), last); SNX(PV(self), first);
    uint32_t tmp = PV(self);
    SPV(self, PV(last)); SPV(last, PV(first)); SPV(first, tmp);
}

// ---------- red-black tree (std::_Rb_tree_node_base: color, parent, left, right) ----------
enum { RED = 0, BLACK = 1 };
#define COL(n) rd32(n)
#define PAR(n) rd32((n) + 4)
#define LEF(n) rd32((n) + 8)
#define RIG(n) rd32((n) + 12)
#define SCOL(n, v) wr32((n), (v))
#define SPAR(n, v) wr32((n) + 4, (v))
#define SLEF(n, v) wr32((n) + 8, (v))
#define SRIG(n, v) wr32((n) + 12, (v))

static uint32_t rb_inc(uint32_t x) {
    if (RIG(x)) { x = RIG(x); while (LEF(x)) x = LEF(x); return x; }
    uint32_t y = PAR(x);
    while (x == RIG(y)) { x = y; y = PAR(y); }
    if (RIG(x) != y) x = y;
    return x;
}
static uint32_t rb_dec(uint32_t x) {
    if (COL(x) == RED && PAR(PAR(x)) == x) return RIG(x);   // header -> rightmost
    if (LEF(x)) { uint32_t y = LEF(x); while (RIG(y)) y = RIG(y); return y; }
    uint32_t y = PAR(x);
    while (x == LEF(y)) { x = y; y = PAR(y); }
    return y;
}
void I___ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(CPU *c) { RET(rb_inc(ARG(0))); }
void I___ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base(CPU *c) { RET(rb_inc(ARG(0))); }
void I___ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(CPU *c) { RET(rb_dec(ARG(0))); }
void I___ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base(CPU *c) { RET(rb_dec(ARG(0))); }

static void rot_left(uint32_t x, uint32_t root_slot) {
    uint32_t y = RIG(x);
    SRIG(x, LEF(y));
    if (LEF(y)) SPAR(LEF(y), x);
    SPAR(y, PAR(x));
    if (x == rd32(root_slot)) wr32(root_slot, y);
    else if (x == LEF(PAR(x))) SLEF(PAR(x), y);
    else SRIG(PAR(x), y);
    SLEF(y, x); SPAR(x, y);
}
static void rot_right(uint32_t x, uint32_t root_slot) {
    uint32_t y = LEF(x);
    SLEF(x, RIG(y));
    if (RIG(y)) SPAR(RIG(y), x);
    SPAR(y, PAR(x));
    if (x == rd32(root_slot)) wr32(root_slot, y);
    else if (x == RIG(PAR(x))) SRIG(PAR(x), y);
    else SLEF(PAR(x), y);
    SRIG(y, x); SPAR(x, y);
}

// (bool insert_left, node *x, node *p, node &header)
void I___ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(CPU *c) {
    int left = ARG(0) & 0xFF;
    uint32_t x = ARG(1), p = ARG(2), hdr = ARG(3);
    uint32_t root_slot = hdr + 4;   // header.parent is the root
    SPAR(x, p); SLEF(x, 0); SRIG(x, 0); SCOL(x, RED);
    if (left) {
        SLEF(p, x);
        if (p == hdr) { SPAR(hdr, x); SRIG(hdr, x); }
        else if (p == LEF(hdr)) SLEF(hdr, x);
    } else {
        SRIG(p, x);
        if (p == RIG(hdr)) SRIG(hdr, x);
    }
    while (x != rd32(root_slot) && COL(PAR(x)) == RED) {
        uint32_t xpp = PAR(PAR(x));
        if (PAR(x) == LEF(xpp)) {
            uint32_t y = RIG(xpp);
            if (y && COL(y) == RED) { SCOL(PAR(x), BLACK); SCOL(y, BLACK); SCOL(xpp, RED); x = xpp; }
            else {
                if (x == RIG(PAR(x))) { x = PAR(x); rot_left(x, root_slot); }
                SCOL(PAR(x), BLACK); SCOL(xpp, RED); rot_right(xpp, root_slot);
            }
        } else {
            uint32_t y = LEF(xpp);
            if (y && COL(y) == RED) { SCOL(PAR(x), BLACK); SCOL(y, BLACK); SCOL(xpp, RED); x = xpp; }
            else {
                if (x == LEF(PAR(x))) { x = PAR(x); rot_right(x, root_slot); }
                SCOL(PAR(x), BLACK); SCOL(xpp, RED); rot_left(xpp, root_slot);
            }
        }
    }
    SCOL(rd32(root_slot), BLACK);
}

// (node *z, node &header) -> node to free
void I___ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(CPU *c) {
    uint32_t z = ARG(0), hdr = ARG(1);
    uint32_t root_slot = hdr + 4, lm_slot = hdr + 8, rm_slot = hdr + 12;
    uint32_t y = z, x = 0, xp = 0;
    if (!LEF(y)) x = RIG(y);
    else if (!RIG(y)) x = LEF(y);
    else { y = RIG(y); while (LEF(y)) y = LEF(y); x = RIG(y); }
    if (y != z) {   // relink y in place of z
        SPAR(LEF(z), y); SLEF(y, LEF(z));
        if (y != RIG(z)) {
            xp = PAR(y);
            if (x) SPAR(x, PAR(y));
            SLEF(PAR(y), x);
            SRIG(y, RIG(z)); SPAR(RIG(z), y);
        } else xp = y;
        if (rd32(root_slot) == z) wr32(root_slot, y);
        else if (LEF(PAR(z)) == z) SLEF(PAR(z), y);
        else SRIG(PAR(z), y);
        SPAR(y, PAR(z));
        uint32_t t = COL(y); SCOL(y, COL(z)); SCOL(z, t);
        y = z;
    } else {
        xp = PAR(y);
        if (x) SPAR(x, PAR(y));
        if (rd32(root_slot) == z) wr32(root_slot, x);
        else if (LEF(PAR(z)) == z) SLEF(PAR(z), x);
        else SRIG(PAR(z), x);
        if (rd32(lm_slot) == z) {
            if (!RIG(z)) wr32(lm_slot, PAR(z));
            else { uint32_t m = x; while (LEF(m)) m = LEF(m); wr32(lm_slot, m); }
        }
        if (rd32(rm_slot) == z) {
            if (!LEF(z)) wr32(rm_slot, PAR(z));
            else { uint32_t m = x; while (RIG(m)) m = RIG(m); wr32(rm_slot, m); }
        }
    }
    if (COL(y) != RED) {
        while (x != rd32(root_slot) && (!x || COL(x) == BLACK)) {
            if (x == LEF(xp)) {
                uint32_t w = RIG(xp);
                if (COL(w) == RED) { SCOL(w, BLACK); SCOL(xp, RED); rot_left(xp, root_slot); w = RIG(xp); }
                if ((!LEF(w) || COL(LEF(w)) == BLACK) && (!RIG(w) || COL(RIG(w)) == BLACK)) {
                    SCOL(w, RED); x = xp; xp = PAR(xp);
                } else {
                    if (!RIG(w) || COL(RIG(w)) == BLACK) { SCOL(LEF(w), BLACK); SCOL(w, RED); rot_right(w, root_slot); w = RIG(xp); }
                    SCOL(w, COL(xp)); SCOL(xp, BLACK);
                    if (RIG(w)) SCOL(RIG(w), BLACK);
                    rot_left(xp, root_slot);
                    break;
                }
            } else {
                uint32_t w = LEF(xp);
                if (COL(w) == RED) { SCOL(w, BLACK); SCOL(xp, RED); rot_right(xp, root_slot); w = LEF(xp); }
                if ((!RIG(w) || COL(RIG(w)) == BLACK) && (!LEF(w) || COL(LEF(w)) == BLACK)) {
                    SCOL(w, RED); x = xp; xp = PAR(xp);
                } else {
                    if (!LEF(w) || COL(LEF(w)) == BLACK) { SCOL(RIG(w), BLACK); SCOL(w, RED); rot_left(w, root_slot); w = LEF(xp); }
                    SCOL(w, COL(xp)); SCOL(xp, BLACK);
                    if (LEF(w)) SCOL(LEF(w), BLACK);
                    rot_right(xp, root_slot);
                    break;
                }
            }
        }
        if (x) SCOL(x, BLACK);
    }
    RET(y);
}

// ---------- RTTI / __dynamic_cast (Itanium ABI, i386) ----------
// type_info: {vptr, name}; __si: +base; __vmi: +flags, count, {base, offset_flags}[]
enum { TI_CLASS, TI_SI, TI_VMI, TI_OTHER };
static int ti_kind(uint32_t ti) {
    static uint32_t vc, vs, vv;
    if (!vc) {
        vc = rt_import_addr("__ZTVN10__cxxabiv117__class_type_infoE") + 8;
        vs = rt_import_addr("__ZTVN10__cxxabiv120__si_class_type_infoE") + 8;
        vv = rt_import_addr("__ZTVN10__cxxabiv121__vmi_class_type_infoE") + 8;
    }
    uint32_t v = rd32(ti);
    return v == vc ? TI_CLASS : v == vs ? TI_SI : v == vv ? TI_VMI : TI_OTHER;
}
static int ti_eq(uint32_t a, uint32_t b) {
    if (a == b) return 1;
    return a && b && !strcmp((char *)G2H(rd32(a + 4)), (char *)G2H(rd32(b + 4)));
}
// find a public subobject of type dst inside the (sub)object of type `ti` at guest address obj
static uint32_t find_base(uint32_t ti, uint32_t obj, uint32_t dst, int *found) {
    if (ti_eq(ti, dst)) { (*found)++; return obj; }
    int k = ti_kind(ti);
    if (k == TI_SI) return find_base(rd32(ti + 8), obj, dst, found);
    if (k != TI_VMI) return 0;
    uint32_t n = rd32(ti + 12), r = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t base = rd32(ti + 16 + 8 * i);
        int32_t of = (int32_t)rd32(ti + 20 + 8 * i);
        if (!(of & 2)) continue;   // non-public base
        int32_t off = of >> 8;
        uint32_t sub = (of & 1) ? obj + (uint32_t)(int32_t)rd32(rd32(obj) + (uint32_t)off) : obj + (uint32_t)off;
        uint32_t x = find_base(base, sub, dst, found);
        if (x && !r) r = x;
    }
    return r;
}
// (const void *src, const __class_type_info *src_type, const __class_type_info *dst_type, ptrdiff_t hint)
void I____dynamic_cast(CPU *c) {
    uint32_t src = ARG(0), dst_t = ARG(2);
    if (!src) { RET(0); return; }
    uint32_t vt = rd32(src);
    int32_t to_top = (int32_t)rd32(vt - 8);
    uint32_t whole_t = rd32(vt - 4), whole = src + (uint32_t)to_top;
    int found = 0;
    uint32_t r = find_base(whole_t, whole, dst_t, &found);
    RET(found >= 1 ? r : 0);
}

// ---------- minimal iostream (GCC 4.2 libstdc++, i386 layouts) ----------
// ios_base: vptr 0, precision 4, width 8, flags 12, exceptions 16, state 20, callbacks 24,
//           word_zero 28, local_word[8] 36, word_size 100, word 104, locale 108  (112 bytes)
// basic_ios: tie 112, fill 116, fill_init 117, streambuf 120, ctype 124, num_put 128, num_get 132
// basic_streambuf vtable: 0 D1, 1 D0, 2 imbue, 3 setbuf, 4 seekoff, 5 seekpos, 6 sync, 7 showmanyc,
//                         8 xsgetn, 9 underflow, 10 uflow, 11 pbackfail, 12 xsputn, 13 overflow
// streambuf fields: in_beg 4, in_cur 8, in_end 12, out_beg 16, out_cur 20, out_end 24
static uint32_t ios_of(uint32_t os) { uint32_t vt = rd32(os); return os + (uint32_t)(int32_t)rd32(vt - 12); }
static void sb_write(CPU *c, uint32_t sb, const char *s, uint32_t n) {
    if (!sb || !n) return;
    uint32_t tmp = gmalloc(n + 1); MC(tmp, n + 1); memcpy(M + tmp, s, n); wr8(tmp + n, 0);
    uint32_t a[3] = { sb, tmp, n };
    guest_call(c, rd32(rd32(sb) + 12 * 4), 3, a);   // virtual xsputn
    gfree(tmp);
}
#define IOS_UNITBUF 0x2000u   // libstdc++ ios_base::unitbuf (1 << 13)
static void os_write(CPU *c, uint32_t os, const char *s, uint32_t n) {
    uint32_t ios = ios_of(os);
    uint32_t sb = rd32(ios + 120);
    if (!sb) { wr32(ios + 20, rd32(ios + 20) | 1); return; }   // badbit
    sb_write(c, sb, s, n);
    // ostream::sentry destructor: with unitbuf set, flush (pubsync -> virtual sync) after every insertion
    if (rd32(ios + 12) & IOS_UNITBUF) {
        uint32_t a[1] = { sb };
        if ((int32_t)guest_call(c, rd32(rd32(sb) + 6 * 4), 1, a) == -1) wr32(ios + 20, rd32(ios + 20) | 1);
    }
}
void I___ZNSt8ios_baseC2Ev(CPU *c) {
    uint32_t t = ARG(0);
    memset(M + t + 4, 0, 108);
    wr32(t + 100, 8); wr32(t + 104, t + 36);
}
void I___ZNSt6localeC1Ev(CPU *c) { static uint32_t impl; if (!impl) { impl = gmalloc(64); memset(M + impl, 0, 64); } wr32(ARG(0), impl); }
void I___ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E(CPU *c) {
    uint32_t t = ARG(0), sb = ARG(1);
    wr32(t + 4, 6); wr32(t + 8, 0); wr32(t + 12, 0x1002); wr32(t + 16, 0); wr32(t + 20, sb ? 0 : 1);
    wr32(t + 112, 0); wr8(t + 116, ' '); wr8(t + 117, 1); wr32(t + 120, sb);
}
void I___ZNSt9basic_iosIcSt11char_traitsIcEED2Ev(CPU *c) { (void)c; }
void I___ZNSoD0Ev(CPU *c) { (void)c; }
void I___ZNSoD1Ev(CPU *c) { (void)c; }
void I___ZNSoD2Ev(CPU *c) { (void)c; }
void I___ZTv0_n12_NSoD0Ev(CPU *c) { (void)c; }
void I___ZTv0_n12_NSoD1Ev(CPU *c) { (void)c; }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEED2Ev(CPU *c) { (void)c; }
void I___ZSt16__ostream_insertIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i(CPU *c) {
    os_write(c, ARG(0), (char *)G2H(ARG(1)), ARG(2)); RET(ARG(0));
}
void I___ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(CPU *c) {
    const char *s = ARG(1) ? (char *)G2H(ARG(1)) : "(null)";
    os_write(c, ARG(0), s, (uint32_t)strlen(s)); RET(ARG(0));
}
void I___ZNSolsEi(CPU *c) { char b[32]; int n = snprintf(b, sizeof b, "%d", (int32_t)ARG(1)); os_write(c, ARG(0), b, n); RET(ARG(0)); }
void I___ZNSo9_M_insertIlEERSoT_(CPU *c) { char b[32]; int n = snprintf(b, sizeof b, "%d", (int32_t)ARG(1)); os_write(c, ARG(0), b, n); RET(ARG(0)); }
void I___ZNSo9_M_insertImEERSoT_(CPU *c) { char b[32]; int n = snprintf(b, sizeof b, "%u", ARG(1)); os_write(c, ARG(0), b, n); RET(ARG(0)); }
void I___ZNSo9_M_insertIdEERSoT_(CPU *c) {
    int prec = (int)rd32(ios_of(ARG(0)) + 4);
    char b[64]; int n = snprintf(b, sizeof b, "%.*g", prec > 0 ? prec : 6, ARGD(1)); os_write(c, ARG(0), b, n); RET(ARG(0));
}
// basic_streambuf defaults (used when a derived buffer does not override)
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale(CPU *c) { (void)c; }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci(CPU *c) { RET(ARG(0)); }
static void ret_fpos_minus1(CPU *c) {   // pos_type returned through hidden pointer (callee pops it)
    uint32_t p = ARG(0); memset(M + p, 0, 16); wr64(p, (uint64_t)-1); RET(p); c->esp += 4;
}
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffExSt12_Ios_SeekdirSt13_Ios_Openmode(CPU *c) { ret_fpos_minus1(c); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposI11__mbstate_tESt13_Ios_Openmode(CPU *c) { ret_fpos_minus1(c); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv(CPU *c) { RET(0); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci(CPU *c) { RET(0); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv(CPU *c) { RET(-1); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi(CPU *c) { RET(-1); }
void I___ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci(CPU *c) {   // default: fill put area, else overflow()
    uint32_t sb = ARG(0), s = ARG(1), n = ARG(2), done = 0;
    if (getenv("RT32_TRACE_SB")) fprintf(stderr, "[sb] xsputn sb=%08x beg=%08x cur=%08x end=%08x n=%u \"%.*s\"\n", sb, rd32(sb + 16), rd32(sb + 20), rd32(sb + 24), n, (int)(n < 40 ? n : 40), (char *)G2H(s));
    while (done < n) {
        uint32_t cur = rd32(sb + 20), end = rd32(sb + 24);
        if (cur < end) {
            uint32_t k = end - cur; if (k > n - done) k = n - done;
            MC(cur, k); memmove(M + cur, M + s + done, k); wr32(sb + 20, cur + k); done += k;
        } else {
            uint32_t a[2] = { sb, rd8(s + done) };
            if ((int32_t)guest_call(c, rd32(rd32(sb) + 13 * 4), 2, a) == -1) break;
            done++;
        }
    }
    RET(done);
}


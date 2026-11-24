// libstdc++ (GCC 4.2, copy-on-write) std::string / std::wstring shims for the 32-bit guest.
// Layout: the object is one pointer to the character data; a 12-byte header precedes the data:
//   { uint32 length; uint32 capacity; int32 refcount; }   refcount <0 = leaked, 0 = unshared, >0 = shared.
// Strings are allocated and freed only here (gmalloc/gfree), so the game's inline code and these shims agree.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RET(v) (c->eax = (uint32_t)(v))
#define NPOS   0xFFFFFFFFu
#define REPSZ  12u
#define HP(a)  ((uint8_t *)(M + (a)))

static uint32_t empty_data(int W) { // data pointer of the shared empty rep (header placeholder + 12)
    static uint32_t e[2];
    int k = (W == 1) ? 0 : 1;
    if (!e[k])
        e[k] = rt_import_addr(W == 1 ? "__ZNSs4_Rep20_S_empty_rep_storageE"
                                     : "__ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE") +
               REPSZ;
    return e[k];
}
static inline int is_empty(int W, uint32_t d) {
    return d == empty_data(W);
}
static inline uint32_t s_len(uint32_t d) {
    return rd32(d - 12);
}
static inline uint32_t s_cap(uint32_t d) {
    return rd32(d - 8);
}
static inline int32_t s_ref(uint32_t d) {
    return (int32_t)rd32(d - 4);
}
static inline uint32_t ch(int W, uint32_t a, uint32_t i) {
    return W == 1 ? rd8(a + i) : rd32(a + 4 * i);
}
static inline void put(int W, uint32_t a, uint32_t i, uint32_t v) {
    if (W == 1)
        wr8(a + i, (uint8_t)v);
    else
        wr32(a + 4 * i, v);
}

static void s_die(const char *m) {
    fprintf(stderr, "[rt] std::string: %s\n", m);
    abort();
}

static uint32_t s_create(int W, uint32_t cap, uint32_t oldcap) {
    uint32_t max = ((0xFFFFFFFFu - REPSZ) / W - 1) / 4;
    if (cap > max)
        s_die("length_error");
    if (cap > oldcap && cap < 2 * oldcap)
        cap = 2 * oldcap;
    uint32_t p = gmalloc(REPSZ + (cap + 1) * W);
    if (!p)
        s_die("out of memory");
    wr32(p, 0);
    wr32(p + 4, cap);
    wr32(p + 8, 0);
    return p + REPSZ;
}
static void s_set_len(int W, uint32_t d, uint32_t n) { // length + "sharable" + terminator
    if (is_empty(W, d))
        return;
    wr32(d - 12, n);
    wr32(d - 4, 0);
    memset(HP(d + n * W), 0, W);
}
static void s_dispose(int W, uint32_t d) {
    if (is_empty(W, d))
        return;
    int32_t old = s_ref(d);
    wr32(d - 4, (uint32_t)(old - 1));
    if (old <= 0)
        gfree(d - REPSZ);
}
static uint32_t s_grab(int W, uint32_t d) { // copy-construct: share unless leaked
    if (is_empty(W, d))
        return d;
    if (s_ref(d) < 0) {
        uint32_t n = s_len(d), r = s_create(W, n, 0);
        memcpy(HP(r), HP(d), (size_t)n * W);
        s_set_len(W, r, n);
        return r;
    }
    wr32(d - 4, rd32(d - 4) + 1);
    return d;
}
static void s_mutate(int W, uint32_t self, uint32_t pos, uint32_t len1, uint32_t len2) {
    uint32_t d = rd32(self), old = s_len(d), nw = old + len2 - len1, how = old - pos - len1;
    if (nw > s_cap(d) || (!is_empty(W, d) && s_ref(d) > 0)) {
        uint32_t r = s_create(W, nw, s_cap(d));
        if (pos)
            memcpy(HP(r), HP(d), (size_t)pos * W);
        if (how)
            memcpy(HP(r + (pos + len2) * W), HP(d + (pos + len1) * W), (size_t)how * W);
        s_dispose(W, d);
        wr32(self, r);
        d = r;
    } else if (how && len1 != len2) {
        memmove(HP(d + (pos + len2) * W), HP(d + (pos + len1) * W), (size_t)how * W);
    }
    s_set_len(W, d, nw);
}
static void s_reserve(int W, uint32_t self, uint32_t res) {
    uint32_t d = rd32(self);
    if (res != s_cap(d) || (!is_empty(W, d) && s_ref(d) > 0)) {
        uint32_t len = s_len(d);
        if (res < len)
            res = len;
        uint32_t r = s_create(W, res, s_cap(d));
        memcpy(HP(r), HP(d), (size_t)len * W);
        s_dispose(W, d);
        wr32(self, r);
        s_set_len(W, r, len);
    }
}
static void s_leak_hard(int W, uint32_t self) {
    uint32_t d = rd32(self);
    if (is_empty(W, d))
        return;
    if (s_ref(d) > 0) {
        s_mutate(W, self, 0, 0, 0);
        d = rd32(self);
    }
    wr32(d - 4, (uint32_t)-1);
}
static uint32_t s_strlen(int W, uint32_t s) {
    uint32_t n = 0;
    while (ch(W, s, n))
        n++;
    return n;
}

// replace(pos, n1, s, n2): the source is copied first, so it may alias the string itself
static uint32_t s_replace_ptr(int W, uint32_t self, uint32_t pos, uint32_t n1, uint32_t s, uint32_t n2) {
    uint32_t size = s_len(rd32(self));
    if (pos > size)
        s_die("out_of_range");
    if (n1 > size - pos)
        n1 = size - pos;
    uint8_t *tmp = NULL;
    if (n2) {
        tmp = malloc((size_t)n2 * W);
        memcpy(tmp, HP(s), (size_t)n2 * W);
    }
    s_mutate(W, self, pos, n1, n2);
    if (n2) {
        memcpy(HP(rd32(self) + pos * W), tmp, (size_t)n2 * W);
        free(tmp);
    }
    return self;
}
static uint32_t s_replace_fill(int W, uint32_t self, uint32_t pos, uint32_t n1, uint32_t n2, uint32_t chr) {
    uint32_t size = s_len(rd32(self));
    if (pos > size)
        s_die("out_of_range");
    if (n1 > size - pos)
        n1 = size - pos;
    s_mutate(W, self, pos, n1, n2);
    uint32_t d = rd32(self);
    for (uint32_t i = 0; i < n2; i++)
        put(W, d, pos + i, chr);
    return self;
}
static void s_construct_copy(int W, uint32_t self, uint32_t s, uint32_t n) {
    if (n == 0) {
        wr32(self, empty_data(W));
        return;
    }
    uint32_t r = s_create(W, n, 0);
    memcpy(HP(r), HP(s), (size_t)n * W);
    s_set_len(W, r, n);
    wr32(self, r);
}
static uint32_t s_find(int W, uint32_t self, uint32_t s, uint32_t pos, uint32_t n) {
    uint32_t d = rd32(self), size = s_len(d);
    if (n == 0)
        return pos <= size ? pos : NPOS;
    if (n <= size) {
        for (; pos <= size - n; ++pos) {
            if (ch(W, d, pos) == ch(W, s, 0)) {
                uint32_t k = 1;
                while (k < n && ch(W, d, pos + k) == ch(W, s, k))
                    k++;
                if (k == n)
                    return pos;
            }
        }
    }
    return NPOS;
}
static int s_compare(int W, uint32_t d, uint32_t size, uint32_t s, uint32_t osize) {
    uint32_t len = size < osize ? size : osize;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = ch(W, d, i), b = ch(W, s, i);
        if (a != b)
            return W == 1 ? ((int)a - (int)b) : ((int32_t)a > (int32_t)b ? 1 : -1);
    }
    return (int)(size - osize);
}

// ================= std::string (char) =================
void I___ZNSs12_M_leak_hardEv(CPU *c) {
    s_leak_hard(1, ARG(0));
}
void I___ZNSs4_Rep10_M_destroyERKSaIcE(CPU *c) {
    gfree(ARG(0));
}
void I___ZNSs9_M_mutateEmmm(CPU *c) {
    s_mutate(1, ARG(0), ARG(1), ARG(2), ARG(3));
}
void I___ZNSs7reserveEm(CPU *c) {
    s_reserve(1, ARG(0), ARG(1));
}
void I___ZNSs5eraseEmm(CPU *c) {
    uint32_t self = ARG(0), pos = ARG(1), n = ARG(2), size = s_len(rd32(self));
    if (pos > size)
        s_die("out_of_range");
    if (n > size - pos)
        n = size - pos;
    s_mutate(1, self, pos, n, 0);
    RET(self);
}
void I___ZNSs6appendEPKcm(CPU *c) {
    uint32_t self = ARG(0);
    RET(s_replace_ptr(1, self, s_len(rd32(self)), 0, ARG(1), ARG(2)));
}
void I___ZNSs6appendERKSs(CPU *c) {
    uint32_t self = ARG(0), o = rd32(ARG(1));
    RET(s_replace_ptr(1, self, s_len(rd32(self)), 0, o, s_len(o)));
}
void I___ZNSs6assignEPKcm(CPU *c) {
    uint32_t self = ARG(0);
    RET(s_replace_ptr(1, self, 0, s_len(rd32(self)), ARG(1), ARG(2)));
}
void I___ZNSs6assignERKSs(CPU *c) {
    uint32_t self = ARG(0), sd = rd32(ARG(1)), d = rd32(self);
    if (sd != d) {
        uint32_t t = s_grab(1, sd);
        s_dispose(1, d);
        wr32(self, t);
    }
    RET(self);
}
void I___ZNSs7replaceEmmPKcm(CPU *c) {
    RET(s_replace_ptr(1, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)));
}
void I___ZNSs7replaceEmmmc(CPU *c) {
    RET(s_replace_fill(1, ARG(0), ARG(1), ARG(2), ARG(3), (uint8_t)ARG(4)));
}
void I___ZNSs9push_backEc(CPU *c) {
    uint32_t self = ARG(0), size = s_len(rd32(self));
    s_mutate(1, self, size, 0, 1);
    put(1, rd32(self), size, (uint8_t)ARG(1));
}
void I___ZNSsC1EPKcRKSaIcE(CPU *c) {
    uint32_t self = ARG(0), s = ARG(1);
    if (!s)
        s_die("basic_string::_S_construct NULL not valid");
    s_construct_copy(1, self, s, s_strlen(1, s));
    RET(self);
}
void I___ZNSsC1ERKSs(CPU *c) {
    uint32_t self = ARG(0);
    wr32(self, s_grab(1, rd32(ARG(1))));
    RET(self);
}
void I___ZNSsC1EmcRKSaIcE(CPU *c) {
    uint32_t self = ARG(0), n = ARG(1);
    if (n == 0)
        wr32(self, empty_data(1));
    else {
        uint32_t r = s_create(1, n, 0);
        memset(HP(r), (uint8_t)ARG(2), n);
        s_set_len(1, r, n);
        wr32(self, r);
    }
    RET(self);
}
void I___ZNSsD1Ev(CPU *c) {
    s_dispose(1, rd32(ARG(0)));
}
void I___ZNSsD2Ev(CPU *c) {
    s_dispose(1, rd32(ARG(0)));
}
void I___ZNKSs4findEPKcmm(CPU *c) {
    RET(s_find(1, ARG(0), ARG(1), ARG(2), ARG(3)));
}
void I___ZNKSs4findEcm(CPU *c) {
    uint32_t d = rd32(ARG(0)), size = s_len(d), pos = ARG(2), ret = NPOS;
    uint8_t cc = (uint8_t)ARG(1);
    for (uint32_t i = pos; pos < size && i < size; i++)
        if (rd8(d + i) == cc) {
            ret = i;
            break;
        }
    RET(ret);
}
void I___ZNKSs5rfindEcm(CPU *c) {
    uint32_t d = rd32(ARG(0)), size = s_len(d), pos = ARG(2), ret = NPOS;
    uint8_t cc = (uint8_t)ARG(1);
    if (size) {
        if (--size > pos)
            size = pos;
        for (++size; size-- > 0;)
            if (rd8(d + size) == cc) {
                ret = size;
                break;
            }
    }
    RET(ret);
}
void I___ZNKSs6substrEmm(CPU *c) { // returns std::string by hidden pointer; callee pops it
    uint32_t hid = ARG(0), self = ARG(1), pos = ARG(2), n = ARG(3), d = rd32(self), size = s_len(d);
    if (pos > size)
        s_die("out_of_range (substr)");
    if (n > size - pos)
        n = size - pos;
    s_construct_copy(1, hid, d + pos, n);
    RET(hid);
    c->esp += 4;
}
void I___ZNKSs7compareEPKc(CPU *c) {
    uint32_t d = rd32(ARG(0)), s = ARG(1);
    RET(s_compare(1, d, s_len(d), s, s_strlen(1, s)));
}
void I___ZNKSs7compareERKSs(CPU *c) {
    uint32_t d = rd32(ARG(0)), o = rd32(ARG(1));
    RET(s_compare(1, d, s_len(d), o, s_len(o)));
}

// ================= std::wstring (wchar_t = 32-bit) =================
void I___ZNKSbIwSt11char_traitsIwESaIwEE4findEPKwmm(CPU *c) {
    RET(s_find(4, ARG(0), ARG(1), ARG(2), ARG(3)));
}
void I___ZNKSbIwSt11char_traitsIwESaIwEE7compareERKS2_(CPU *c) {
    uint32_t d = rd32(ARG(0)), o = rd32(ARG(1));
    RET(s_compare(4, d, s_len(d), o, s_len(o)));
}
void I___ZNSbIwSt11char_traitsIwESaIwEE12_M_leak_hardEv(CPU *c) {
    s_leak_hard(4, ARG(0));
}
void I___ZNSbIwSt11char_traitsIwESaIwEE3endEv(CPU *c) { // _M_leak(); return data + size
    uint32_t self = ARG(0), d = rd32(self);
    if (s_ref(d) >= 0) {
        s_leak_hard(4, self);
        d = rd32(self);
    }
    RET(d + 4 * s_len(d));
}
void I___ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_destroyERKS1_(CPU *c) {
    gfree(ARG(0));
}
void I___ZNSbIwSt11char_traitsIwESaIwEE6resizeEmw(CPU *c) {
    uint32_t self = ARG(0), n = ARG(1), chr = ARG(2), size = s_len(rd32(self));
    if (size < n)
        s_replace_fill(4, self, size, 0, n - size, chr);
    else if (n < size)
        s_mutate(4, self, n, size - n, 0);
}
void I___ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmPKwm(CPU *c) {
    RET(s_replace_ptr(4, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)));
}
void I___ZNSbIwSt11char_traitsIwESaIwEE9_M_mutateEmmm(CPU *c) {
    s_mutate(4, ARG(0), ARG(1), ARG(2), ARG(3));
}
void I___ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKS1_(CPU *c) {
    uint32_t self = ARG(0), s = ARG(1);
    if (!s)
        s_die("basic_string::_S_construct NULL not valid");
    s_construct_copy(4, self, s, s_strlen(4, s));
    RET(self);
}
void I___ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_(CPU *c) {
    uint32_t self = ARG(0);
    wr32(self, s_grab(4, rd32(ARG(1))));
    RET(self);
}
void I___ZNSbIwSt11char_traitsIwESaIwEED2Ev(CPU *c) {
    s_dispose(4, rd32(ARG(0)));
}

// ================= misc =================
void I___ZNSt8ios_base4InitC1Ev(CPU *c) {
    RET(ARG(0));
}
void I___ZNSt8ios_base4InitD1Ev(CPU *c) {
    RET(ARG(0));
}

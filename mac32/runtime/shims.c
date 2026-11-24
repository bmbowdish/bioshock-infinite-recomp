#include <mach/mach_time.h>
// libc shims: guest (i386 cdecl, 32-bit pointers) -> host arm64 libc.
// Each shim reads its arguments with ARG(slot) (slot 0 = first argument;
// a double occupies two slots) and returns in C.eax / C.edx / x87 ST(0).
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <time.h>
#include <runetype.h>

void rt_set_data_import(const char *name, uint32_t guest_addr);

#define S(n)    ((char *)G2H(n)) // guest string -> host char*
#define RET(v)  (C.eax = (uint32_t)(v))
#define RETF(v) FPUSH((double)(v))

// ---------- guest FILE handles ----------
// A guest FILE* is a 16-byte guest block; word 0 = index into host table.
#define NFILES 4096 // guest FILE table; UE3 keeps many package files open while streaming
static FILE *hfiles[NFILES];
static uint32_t gfile_new(FILE *h) {
    if (!h)
        return 0;
    {
        static int warned, high;
        int used = 0;
        for (int i = 0; i < NFILES; i++)
            used += hfiles[i] != NULL;
        if (used >= NFILES - 1 && !warned++)
            fprintf(stderr, "[file] guest FILE table full (%d)\n", NFILES);
        if (used > high && (used % 32 == 0)) {
            high = used;
            fprintf(stderr, "[file] open guest FILEs high-water: %d\n", used);
        }
    }
    for (int i = 0; i < NFILES; i++)
        if (!hfiles[i]) {
            hfiles[i] = h;
            uint32_t g = gmalloc(16);
            wr32(g, (uint32_t)i);
            wr32(g + 4, 0x46494C45);
            return g;
        }
    return 0;
}
// A closed guest FILE keeps its memory (like libc's FILE pool) with a "closed" mark; any later use
// sees a dead stream (reads/writes fail) instead of crashing. Unknown pointers are logged.
#define GFILE_OPEN   0x46494C45u
#define GFILE_CLOSED 0x434C5344u
static FILE *dead_stream(void) {
    static FILE *d;
    if (!d)
        d = fopen("/dev/null", "r");
    return d;
}
static FILE *HF(uint32_t g) {
    if (g && rd32(g + 4) == GFILE_OPEN)
        return hfiles[rd32(g)];
    static int warned;
    if (warned++ < 20)
        fprintf(stderr, "[rt] stdio on %s guest FILE* 0x%x (caller %s) - treated as a closed stream\n",
                g && rd32(g + 4) == GFILE_CLOSED ? "closed" : "unknown", g,
                rt_cur ? rt_func_name(rd32(rt_cur->esp)) : "?");
    return dead_stream();
}
static void gfile_free(uint32_t g) {
    hfiles[rd32(g)] = NULL;
    wr32(g + 4, GFILE_CLOSED); /* memory kept: may be touched after close */
}

static uint32_t g_errno, g_stdin, g_stdout, g_stderr, g_tm, g_lconv;
static void sync_errno(void) {
    wr32(g_errno, (uint32_t)errno);
}

// ---- added for BioShock Infinite: guest wchar_t (32-bit) string -> UTF-8, for %ls / %S ----
static void w2u8(uint32_t p, char *o, size_t cap) {
    size_t n = 0;
    for (;; p += 4) {
        uint32_t ch = rd32(p);
        if (!ch)
            break;
        char t[4];
        int l;
        if (ch < 0x80) {
            t[0] = (char)ch;
            l = 1;
        } else if (ch < 0x800) {
            t[0] = (char)(0xC0 | (ch >> 6));
            t[1] = (char)(0x80 | (ch & 63));
            l = 2;
        } else if (ch < 0x10000) {
            t[0] = (char)(0xE0 | (ch >> 12));
            t[1] = (char)(0x80 | ((ch >> 6) & 63));
            t[2] = (char)(0x80 | (ch & 63));
            l = 3;
        } else {
            t[0] = (char)(0xF0 | (ch >> 18));
            t[1] = (char)(0x80 | ((ch >> 12) & 63));
            t[2] = (char)(0x80 | ((ch >> 6) & 63));
            t[3] = (char)(0x80 | (ch & 63));
            l = 4;
        }
        if (n + l + 1 > cap)
            break;
        memcpy(o + n, t, l);
        n += l;
    }
    o[n] = 0;
}
// ---- end additions ----
// ---------- printf-family formatting with guest varargs ----------
// va = guest address of the first variadic argument.
static int gformat(char *out, size_t cap, const char *fmt, uint32_t va) {
    size_t n = 0;
    char spec[64], tmp[4096];
    while (*fmt) {
        if (*fmt != '%') {
            if (n + 1 < cap)
                out[n] = *fmt;
            n++;
            fmt++;
            continue;
        }
        const char *st = fmt++;
        if (*fmt == '%') {
            if (n + 1 < cap)
                out[n] = '%';
            n++;
            fmt++;
            continue;
        }
        int k = 0;
        spec[k++] = '%';
        int star_vals[2], nstar = 0;
        while (strchr("-+ #0", *fmt))
            spec[k++] = *fmt++;
        if (*fmt == '*') {
            star_vals[nstar++] = (int32_t)rd32(va);
            va += 4;
            spec[k++] = '*';
            fmt++;
        } else
            while (isdigit((unsigned char)*fmt))
                spec[k++] = *fmt++;
        if (*fmt == '.') {
            spec[k++] = *fmt++;
            if (*fmt == '*') {
                star_vals[nstar++] = (int32_t)rd32(va);
                va += 4;
                spec[k++] = '*';
                fmt++;
            } else
                while (isdigit((unsigned char)*fmt))
                    spec[k++] = *fmt++;
        }
        int ll = 0, L = 0;
        for (;;) {
            if (*fmt == 'l') {
                ll++;
                fmt++;
            } else if (*fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j')
                fmt++;
            else if (*fmt == 'q') {
                ll = 2;
                fmt++;
            } else if (*fmt == 'L') {
                L = 1;
                fmt++;
            } else
                break;
        }
        char cv = *fmt++;
        int w = 0;
#define EMIT(...)                                                                                                      \
    do {                                                                                                               \
        if (nstar == 0)                                                                                                \
            w = snprintf(tmp, sizeof tmp, spec, __VA_ARGS__);                                                          \
        else if (nstar == 1)                                                                                           \
            w = snprintf(tmp, sizeof tmp, spec, star_vals[0], __VA_ARGS__);                                            \
        else                                                                                                           \
            w = snprintf(tmp, sizeof tmp, spec, star_vals[0], star_vals[1], __VA_ARGS__);                              \
    } while (0)
        switch (cv) {
        case 'd':
        case 'i':
            if (ll >= 2) {
                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = cv;
                spec[k] = 0;
                EMIT((long long)rd64(va));
                va += 8;
            } else {
                spec[k++] = cv;
                spec[k] = 0;
                EMIT((int32_t)rd32(va));
                va += 4;
            }
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'c':
            if (ll >= 2) {
                spec[k++] = 'l';
                spec[k++] = 'l';
                spec[k++] = cv;
                spec[k] = 0;
                EMIT((unsigned long long)rd64(va));
                va += 8;
            } else {
                spec[k++] = cv;
                spec[k] = 0;
                EMIT(rd32(va));
                va += 4;
            }
            break;
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            if (L) {
                fprintf(stderr, "[rt] long double printf unsupported\n");
                abort();
            }
            spec[k++] = cv;
            spec[k] = 0;
            EMIT(rdd(va));
            va += 8;
            break;
        case 's':
        case 'S':
            spec[k++] = 's';
            spec[k] = 0;
            { // %ls / %S take a wide string; plain %s takes a narrow one
                uint32_t p = rd32(va);
                va += 4;
                if (cv == 'S' || ll == 1) {
                    char wb[4096];
                    if (p)
                        w2u8(p, wb, sizeof wb);
                    else
                        strcpy(wb, "(null)");
                    EMIT(wb);
                } else
                    EMIT(p ? S(p) : "(null)");
            }
            break;
        case 'p':
            strcpy(spec + k, "#x");
            EMIT(rd32(va));
            va += 4;
            break;
        default:
            fprintf(stderr, "[rt] printf conversion %%%c unsupported (fmt=%s)\n", cv, st);
            abort();
        }
        for (int i = 0; i < w && i < (int)sizeof tmp - 1; i++) {
            if (n + 1 < cap)
                out[n] = tmp[i];
            n++;
        }
    }
    if (cap)
        out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

void I____sprintf_chk(CPU *c) { // (char *s, int flag, size_t slen, fmt, ...)
    int n = gformat(S(ARG(0)), ARG(2), S(ARG(3)), C.esp + 4 + 16);
    MC(ARG(0), (uint32_t)n + 1);
    RET(n);
}
void I__vsnprintf(CPU *c) { // (char *s, size_t n, fmt, va_list)
    int n = gformat(S(ARG(0)), ARG(1), S(ARG(2)), ARG(3));
    MC(ARG(0), (uint32_t)(n + 1 < (int)ARG(1) ? n + 1 : ARG(1)));
    RET(n);
}
void I__fprintf(CPU *c) {
    static char buf[1 << 16];
    int n = gformat(buf, sizeof buf, S(ARG(1)), C.esp + 4 + 8);
    RET(fputs(buf, HF(ARG(0))) < 0 ? -1 : n);
}
void I__fscanf(CPU *c) { // only the single-number form Lua uses: "%f"/"%lf"/"%g"
    const char *f = S(ARG(1));
    if (!strcmp(f, "%f") || !strcmp(f, "%g")) {
        float v;
        int r = fscanf(HF(ARG(0)), "%f", &v);
        if (r == 1)
            wrf(ARG(2), v);
        RET(r);
    } else if (!strcmp(f, "%lf")) {
        double v;
        int r = fscanf(HF(ARG(0)), "%lf", &v);
        if (r == 1)
            wrd(ARG(2), v);
        RET(r);
    } else {
        fprintf(stderr, "[rt] fscanf fmt %s unsupported\n", f);
        abort();
    }
}

// ---------- string / memory ----------
void I__strlen(CPU *c) {
    RET(strlen(S(ARG(0))));
}
void I__strcmp(CPU *c) {
    RET(strcmp(S(ARG(0)), S(ARG(1))));
}
void I__memcmp(CPU *c) {
    RET(memcmp(S(ARG(0)), S(ARG(1)), ARG(2)));
}
void I__memcpy(CPU *c) {
    MC(ARG(0), ARG(2));
    memmove(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I__memset(CPU *c) {
    MC(ARG(0), ARG(2));
    memset(S(ARG(0)), (int)ARG(1), ARG(2));
    RET(ARG(0));
}
void I__strcpy(CPU *c) {
    MC(ARG(0), (uint32_t)strlen(S(ARG(1))) + 1);
    strcpy(S(ARG(0)), S(ARG(1)));
    RET(ARG(0));
}
void I__strcat(CPU *c) {
    MC(ARG(0) + (uint32_t)strlen(S(ARG(0))), (uint32_t)strlen(S(ARG(1))) + 1);
    strcat(S(ARG(0)), S(ARG(1)));
    RET(ARG(0));
}
void I__strncpy(CPU *c) {
    MC(ARG(0), ARG(2));
    strncpy(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I____strncpy_chk(CPU *c) {
    MC(ARG(0), ARG(2));
    strncpy(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I__strncat(CPU *c) {
    MC(ARG(0) + (uint32_t)strlen(S(ARG(0))), ARG(2) + 1);
    strncat(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I__strcspn(CPU *c) {
    RET(strcspn(S(ARG(0)), S(ARG(1))));
}
#define RETP(base, hp) RET((hp) ? H2G(hp) : 0)
void I__strchr(CPU *c) {
    RETP(0, strchr(S(ARG(0)), (int)ARG(1)));
}
void I__strrchr(CPU *c) {
    RETP(0, strrchr(S(ARG(0)), (int)ARG(1)));
}
void I__strstr(CPU *c) {
    RETP(0, strstr(S(ARG(0)), S(ARG(1))));
}
void I__strpbrk(CPU *c) {
    RETP(0, strpbrk(S(ARG(0)), S(ARG(1))));
}
void I__memchr(CPU *c) {
    RETP(0, memchr(S(ARG(0)), (int)ARG(1), ARG(2)));
}
void I__strtoul(CPU *c) {
    char *end;
    unsigned long v = strtoul(S(ARG(0)), &end, (int)ARG(2));
    if (ARG(1))
        wr32(ARG(1), H2G(end));
    RET(v > 0xFFFFFFFFul ? 0xFFFFFFFFu : v);
}
void I__strtod_UNIX2003(CPU *c) {
    char *end;
    double v = strtod(S(ARG(0)), &end);
    if (ARG(1))
        wr32(ARG(1), H2G(end));
    RETF(v);
}
void I____maskrune(CPU *c) {
    RET(__maskrune((int)ARG(0), ARG(1)));
}
void I____tolower(CPU *c) {
    RET(__tolower((int)ARG(0)));
}
void I____toupper(CPU *c) {
    RET(__toupper((int)ARG(0)));
}
void I____error(CPU *c) {
    RET(g_errno);
}
void I__strerror_UNIX2003(CPU *c) {
    static uint32_t g;
    if (g)
        gfree(g);
    g = gstrdup(strerror((int)ARG(0)));
    RET(g);
}

// ---------- heap ----------
// Debug aid: report who asked for an absurd allocation (≥ 1 GiB) before the heap allocator aborts.
static void big_check(CPU *c, uint32_t n, const char *what) {
    if (n < 0x40000000u)
        return;
    fprintf(stderr, "[rt] huge %s(%u = 0x%x) from %s (0x%x), caller %s\n  stack:", what, n, n, rt_func_name(c->fn),
            c->fn, rt_func_name(rd32(c->esp)));
    for (uint32_t f = c->ebp, k = 0; f > 0x1000 && k < 10; f = rd32(f), k++)
        fprintf(stderr, "\n    %s", rt_func_name(rd32(f + 4)));
    fprintf(stderr, "\n");
    uint32_t a1 = rd32(c->ebp + 8), a2 = rd32(c->ebp + 12); // ⚠ assumes the caller's frame is still the current one
    fprintf(stderr, "  frame args: 0x%x 0x%x\n  arg2 words:", a1, a2);
    for (int i = 0; a2 > 0x1000 && i < 20; i++)
        fprintf(stderr, " %08x", rd32(a2 + 4 * i));
    fprintf(stderr, "\n");
    uint32_t f1 = rd32(c->ebp), ad = f1 > 0x1000 ? rd32(f1 + 8) : 0; // ⚠ guess: e1448's first arg is the adapter
    fprintf(stderr, "  adapter 0x%x words:", ad);
    for (int i = 0; ad > 0x1000 && i < 16; i++)
        fprintf(stderr, " %08x", rd32(ad + 4 * i));
    fprintf(stderr, "\n");
}
void I__malloc(CPU *c) {
    big_check(c, ARG(0), "malloc");
    RET(gmalloc(ARG(0)));
}
void I__calloc(CPU *c) {
    uint32_t n = ARG(0) * ARG(1);
    big_check(c, n, "calloc");
    uint32_t p = gmalloc(n);
    memset(M + p, 0, n);
    RET(p);
}
void I__realloc(CPU *c) {
    big_check(c, ARG(1), "realloc");
    RET(grealloc(ARG(0), ARG(1)));
}
void I__free(CPU *c) {
    gfree(ARG(0));
}

// ---------- setjmp/longjmp, process ----------
void I__longjmp(CPU *c) {
    GJmp *j = gjmp_slot(ARG(0));
    j->val = ARG(1) ? ARG(1) : 1;
    longjmp(j->host, 1);
}
const char *rt_func_name(uint32_t a);
void I____stack_chk_fail(CPU *c) {
    fprintf(stderr, "[rt] guest stack smashing detected in %s (ret 0x%x)\n", rt_func_name(rd32(c->esp)), rd32(c->esp));
    abort();
}
void I__exit(CPU *c) {
    exit((int)ARG(0));
}
void I__system_UNIX2003(CPU *c) {
    RET(ARG(0) ? (uint32_t)-1 : 0);
} // never spawn shells from the game
void I__getenv(CPU *c) {
    static uint32_t g;
    const char *v = getenv(S(ARG(0)));
    if (g)
        gfree(g);
    g = v ? gstrdup(v) : 0;
    RET(g);
}
void I__setlocale(CPU *c) {
    static uint32_t g;
    if (!g)
        g = gstrdup("C");
    RET(g);
}
void I__localeconv(CPU *c) {
    RET(g_lconv);
}

// ---------- math (float args are 4-byte slots, doubles 8-byte) ----------
#define F1(name, fn)                                                                                                   \
    void I__##name(CPU *c) {                                                                                           \
        RETF(fn(ARGF(0)));                                                                                             \
    }
F1(floorf, floorf)
F1(ceilf, ceilf) F1(sinf, sinf) F1(cosf, cosf) F1(tanf, tanf) F1(asinf, asinf) F1(acosf, acosf) F1(atanf, atanf)
    F1(sinhf, sinhf) F1(coshf, coshf) F1(tanhf, tanhf) F1(expf, expf) F1(logf, logf)
        F1(log10f, log10f) void I__pow(CPU *c) {
    RETF(pow(ARGD(0), ARGD(2)));
}
void I__fmod(CPU *c) {
    RETF(fmod(ARGD(0), ARGD(2)));
}
void I__atan2(CPU *c) {
    RETF(atan2(ARGD(0), ARGD(2)));
}
void I__ldexp(CPU *c) {
    RETF(ldexp(ARGD(0), (int)ARG(2)));
}
void I__frexp(CPU *c) {
    int e;
    double r = frexp(ARGD(0), &e);
    wr32(ARG(2), (uint32_t)e);
    RETF(r);
}
void I__modf(CPU *c) {
    double ip;
    double r = modf(ARGD(0), &ip);
    wrd(ARG(2), ip);
    RETF(r);
}
void I__rand(CPU *c) {
    RET(rand() & 0x7FFFFFFF);
}
void I__srand(CPU *c) {
    srand(ARG(0));
}

// ---------- time (i386: time_t/long are 32-bit) ----------
static void tm_to_guest(const struct tm *t, uint32_t g) {
    int32_t v[9] = {t->tm_sec,  t->tm_min,  t->tm_hour, t->tm_mday, t->tm_mon,
                    t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    memcpy(M + g, v, 36);
    wr32(g + 36, (uint32_t)t->tm_gmtoff);
    wr32(g + 40, 0);
}
static void tm_from_guest(uint32_t g, struct tm *t) {
    int32_t v[9];
    memcpy(v, M + g, 36);
    memset(t, 0, sizeof *t);
    t->tm_sec = v[0];
    t->tm_min = v[1];
    t->tm_hour = v[2];
    t->tm_mday = v[3];
    t->tm_mon = v[4];
    t->tm_year = v[5];
    t->tm_wday = v[6];
    t->tm_yday = v[7];
    t->tm_isdst = v[8];
}
void I__time(CPU *c) {
    time_t t = time(NULL);
    if (ARG(0))
        wr32(ARG(0), (uint32_t)t);
    RET(t);
}
void I__clock_UNIX2003(CPU *c) {
    RET(clock());
}
void I__difftime(CPU *c) {
    RETF(difftime((time_t)(int32_t)ARG(0), (time_t)(int32_t)ARG(1)));
}
void I__localtime(CPU *c) {
    time_t t = (int32_t)rd32(ARG(0));
    struct tm *r = localtime(&t);
    if (!r) {
        RET(0);
        return;
    }
    tm_to_guest(r, g_tm);
    RET(g_tm);
}
void I__gmtime(CPU *c) {
    time_t t = (int32_t)rd32(ARG(0));
    struct tm *r = gmtime(&t);
    if (!r) {
        RET(0);
        return;
    }
    tm_to_guest(r, g_tm);
    RET(g_tm);
}
void I__mktime_UNIX2003(CPU *c) {
    struct tm t;
    tm_from_guest(ARG(0), &t);
    time_t r = mktime(&t);
    tm_to_guest(&t, ARG(0));
    RET((int32_t)r);
}
void I__strftime_UNIX2003(CPU *c) {
    struct tm t;
    tm_from_guest(ARG(3), &t);
    RET(strftime(S(ARG(0)), ARG(1), S(ARG(2)), &t));
}

// ---------- stdio ----------
void I__fopen(CPU *c) {
    FILE *f = fopen(S(ARG(0)), S(ARG(1)));
    sync_errno();
    if (!f && errno != ENOENT)
        fprintf(stderr, "[file] fopen %s failed: %s\n", S(ARG(0)), strerror(errno));
    if (getenv("RT32_TRACE_FILES"))
        fprintf(stderr, "[file] fopen %s -> %d\n", S(ARG(0)), f != NULL);
    RET(gfile_new(f));
}
void I__fdopen(CPU *c) {
    RET(gfile_new(fdopen((int)ARG(0), S(ARG(1)))));
}
void I__tmpfile(CPU *c) {
    RET(gfile_new(tmpfile()));
}
void I__fclose(CPU *c) {
    uint32_t g = ARG(0);
    if (!g || rd32(g + 4) != GFILE_OPEN) {
        HF(g);
        RET(-1);
        return;
    } // double close: fail like libc (EOF)
    int r = fclose(hfiles[rd32(g)]);
    gfile_free(g);
    RET(r);
}
void rt_touch_write(uint32_t g, uint32_t n);
extern volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes;
void I__fread(CPU *c) {
    MC(ARG(0), ARG(1) * ARG(2));
    rt_touch_write(ARG(0), ARG(1) * ARG(2));
    uint64_t t0 = mach_absolute_time();
    size_t r = fread(S(ARG(0)), ARG(1), ARG(2), HF(ARG(3)));
    rt_hitch_io_ticks += mach_absolute_time() - t0;
    rt_hitch_io_bytes += (uint64_t)r * ARG(1);
    RET(r);
}
void I__fwrite_UNIX2003(CPU *c) {
    RET(fwrite(S(ARG(0)), ARG(1), ARG(2), HF(ARG(3))));
}
void I__fputs_UNIX2003(CPU *c) {
    RET(fputs(S(ARG(0)), HF(ARG(1))));
}
void I__fputc(CPU *c) {
    RET(fputc((int)ARG(0), HF(ARG(1))));
}
void I__fflush(CPU *c) {
    RET(fflush(ARG(0) ? HF(ARG(0)) : NULL));
}
void I__ferror(CPU *c) {
    RET(ferror(HF(ARG(0))));
}
void I__clearerr(CPU *c) {
    clearerr(HF(ARG(0)));
}
void I__getc(CPU *c) {
    RET(getc(HF(ARG(0))));
}
void I__ungetc(CPU *c) {
    RET(ungetc((int)ARG(0), HF(ARG(1))));
}
void I__fgets(CPU *c) {
    char *r = fgets(S(ARG(0)), (int)ARG(1), HF(ARG(2)));
    RET(r ? ARG(0) : 0);
}
void I__fseek(CPU *c) {
    RET(fseek(HF(ARG(0)), (int32_t)ARG(1), (int)ARG(2)));
}
void I__ftell(CPU *c) {
    RET((uint32_t)ftell(HF(ARG(0))));
}
void I__setvbuf(CPU *c) {
    RET(0);
}
void I__remove(CPU *c) {
    RET(remove(S(ARG(0))));
    sync_errno();
}
void I__rename(CPU *c) {
    RET(rename(S(ARG(0)), S(ARG(1))));
    sync_errno();
}
void I__tmpnam(CPU *c) {
    RET(0);
}

void rt_shims_init(void) {
    g_errno = gmalloc(4);
    wr32(g_errno, 0);
    g_tm = gmalloc(64);
    g_lconv = gmalloc(64);
    memset(M + g_lconv, 0, 64);
    wr32(g_lconv, gstrdup("."));
    uint32_t guard = gmalloc(4);
    wr32(guard, 0x0BADF00Du);
    rt_set_data_import("___stack_chk_guard", guard);
    g_stdin = gfile_new(stdin);
    g_stdout = gfile_new(stdout);
    g_stderr = gfile_new(stderr);
    uint32_t v;
    v = gmalloc(4);
    wr32(v, g_stdin);
    rt_set_data_import("___stdinp", v);
    v = gmalloc(4);
    wr32(v, g_stdout);
    rt_set_data_import("___stdoutp", v);
    v = gmalloc(4);
    wr32(v, g_stderr);
    rt_set_data_import("___stderrp", v);
    // i386 _RuneLocale: runetype[256] at +52, maplower at +1076, mapupper at +2100
    uint32_t rl = gmalloc(4096);
    memset(M + rl, 0, 4096);
    memcpy(M + rl, "RuneMagA", 8);
    for (int i = 0; i < 256; i++) {
        wr32(rl + 52 + 4 * i, _DefaultRuneLocale.__runetype[i]);
        wr32(rl + 1076 + 4 * i, (uint32_t)_DefaultRuneLocale.__maplower[i]);
        wr32(rl + 2100 + 4 * i, (uint32_t)_DefaultRuneLocale.__mapupper[i]);
    }
    rt_set_data_import("__DefaultRuneLocale", rl);
}

// ---------- HLE replacements for engine functions ----------

void H_OPENSSL_ia32_cpuid(CPU *c) {
    c->eax = 0;
    c->edx = 0;
} // report no MMX/SSE: OpenSSL uses integer paths
void H_OPENSSL_wipe_cpu(CPU *c) {
    c->eax = c->esp + 4;
}
uint32_t gmalloc_usable(uint32_t p);
void I__malloc_size(CPU *c) {
    RET(gmalloc_usable(ARG(0)));
}
void I__malloc_good_size(CPU *c) {
    RET(ARG(0));
}

// helpers shared with libc2.c
int gformat_ext(char *out, size_t cap, const char *fmt, uint32_t va) {
    return gformat(out, cap, fmt, va);
}
uint32_t gfile_wrap(FILE *f) {
    return gfile_new(f);
}
FILE *gfile_host(uint32_t g) {
    return HF(g);
}
uint32_t g_errno_addr(void) {
    return g_errno;
}

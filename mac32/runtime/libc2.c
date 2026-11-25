#include <mach/mach_time.h>
// More libSystem shims: files/dirs (i386 struct layouts), formatted I/O, 64-bit math helpers,
// sorting with guest callbacks, sockets (disabled), misc.
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <time.h>
#include <math.h>

#define S(n)   ((char *)G2H(n))
#define RET(v) (c->eax = (uint32_t)(v))
#define RET64(v)                                                                                                       \
    do {                                                                                                               \
        uint64_t _v = (uint64_t)(v);                                                                                   \
        c->eax = (uint32_t)_v;                                                                                         \
        c->edx = (uint32_t)(_v >> 32);                                                                                 \
    } while (0)
#define RETF(v)  FPUSH((double)(v))
#define ARG64(i) ((uint64_t)ARG(i) | ((uint64_t)ARG((i) + 1) << 32))

int gformat_ext(char *out, size_t cap, const char *fmt, uint32_t va); // shims.c
uint32_t gfile_wrap(FILE *f);
FILE *gfile_host(uint32_t g);
uint32_t g_errno_addr(void);
static void seterr(void) {
    wr32(g_errno_addr(), (uint32_t)errno);
}

// ---------- libgcc 64-bit division ----------
void I____udivdi3(CPU *c) {
    uint64_t b = ARG64(2);
    RET64(b ? ARG64(0) / b : 0);
}
void I____umoddi3(CPU *c) {
    uint64_t b = ARG64(2);
    RET64(b ? ARG64(0) % b : 0);
}
void I____divdi3(CPU *c) {
    int64_t b = (int64_t)ARG64(2);
    RET64(b ? (int64_t)ARG64(0) / b : 0);
}
void I____moddi3(CPU *c) {
    int64_t b = (int64_t)ARG64(2);
    RET64(b ? (int64_t)ARG64(0) % b : 0);
}

// ---------- strings / memory ----------
void I__memmove(CPU *c) {
    MC(ARG(0), ARG(2));
    memmove(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I____memmove_chk(CPU *c) {
    MC(ARG(0), ARG(2));
    memmove(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I____memcpy_chk(CPU *c) {
    MC(ARG(0), ARG(2));
    memmove(S(ARG(0)), S(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I____memset_chk(CPU *c) {
    MC(ARG(0), ARG(2));
    memset(S(ARG(0)), (int)ARG(1), ARG(2));
    RET(ARG(0));
}
void I____strcpy_chk(CPU *c) {
    MC(ARG(0), (uint32_t)strlen(S(ARG(1))) + 1);
    strcpy(S(ARG(0)), S(ARG(1)));
    RET(ARG(0));
}
void I____strcat_chk(CPU *c) {
    MC(ARG(0) + (uint32_t)strlen(S(ARG(0))), (uint32_t)strlen(S(ARG(1))) + 1);
    strcat(S(ARG(0)), S(ARG(1)));
    RET(ARG(0));
}
void I__bzero(CPU *c) {
    memset(S(ARG(0)), 0, ARG(1));
}
void I__strncmp(CPU *c) {
    RET(strncmp(S(ARG(0)), S(ARG(1)), ARG(2)));
}
void I__strcasecmp(CPU *c) {
    RET(strcasecmp(S(ARG(0)), S(ARG(1))));
}
void I__strncasecmp(CPU *c) {
    RET(strncasecmp(S(ARG(0)), S(ARG(1)), ARG(2)));
}
void I__strdup(CPU *c) {
    RET(gstrdup(S(ARG(0))));
}
void I__strtol(CPU *c) {
    char *e;
    long v = strtol(S(ARG(0)), &e, (int)ARG(2));
    if (ARG(1))
        wr32(ARG(1), H2G(e));
    RET(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v);
}
void I__strtoull(CPU *c) {
    char *e;
    unsigned long long v = strtoull(S(ARG(0)), &e, (int)ARG(2));
    if (ARG(1))
        wr32(ARG(1), H2G(e));
    RET64(v);
}
void I__atoi(CPU *c) {
    RET(atoi(S(ARG(0))));
}
void I__atof(CPU *c) {
    RETF(atof(S(ARG(0))));
}
void I__strtok(CPU *c) {
    static uint32_t last;
    uint32_t s = ARG(0) ? ARG(0) : last;
    if (!s) {
        RET(0);
        return;
    }
    const char *d = S(ARG(1));
    s += (uint32_t)strspn(S(s), d);
    if (!rd8(s)) {
        last = 0;
        RET(0);
        return;
    }
    uint32_t e = s + (uint32_t)strcspn(S(s), d);
    if (rd8(e)) {
        wr8(e, 0);
        last = e + 1;
    } else
        last = 0;
    RET(s);
}

// ---------- formatted output ----------
void I__sprintf(CPU *c) {
    int n = gformat_ext(S(ARG(0)), 1u << 30, S(ARG(1)), c->esp + 4 + 8);
    MC(ARG(0), (uint32_t)n + 1);
    RET(n);
}
void I__snprintf(CPU *c) {
    RET(gformat_ext(S(ARG(0)), ARG(1), S(ARG(2)), c->esp + 4 + 12));
}
void I__vsprintf(CPU *c) {
    int n = gformat_ext(S(ARG(0)), 1u << 30, S(ARG(1)), ARG(2));
    MC(ARG(0), (uint32_t)n + 1);
    RET(n);
}
void I__printf(CPU *c) {
    static char b[1 << 16];
    int n = gformat_ext(b, sizeof b, S(ARG(0)), c->esp + 4 + 4);
    fputs(b, stdout);
    RET(n);
}
void I__vfprintf(CPU *c) {
    static char b[1 << 16];
    int n = gformat_ext(b, sizeof b, S(ARG(1)), ARG(2));
    fputs(b, gfile_host(ARG(0)));
    RET(n);
}
void I__puts(CPU *c) {
    RET(puts(S(ARG(0))));
}
void I__NSLog(CPU *c) {
    fprintf(stderr, "[NSLog] (CFString 0x%x)\n", ARG(0));
}

// sscanf: one conversion at a time through the host sscanf, tracking consumption with %n
static int scan_core(CPU *c, const char *in, const char *f, uint32_t va, int wide);
void I__sscanf(CPU *c) {
    RET((uint32_t)scan_core(c, S(ARG(0)), S(ARG(1)), c->esp + 4 + 8, 0));
}
// swscanf: guest wchar_t is 4 bytes (UTF-32). Input and format are narrowed (non-ASCII becomes '?'),
// scanned like sscanf, and %s / %c / %[ results are written back as wide strings.
static char *narrow_w(uint32_t g) {
    size_t n = 0;
    while (rd32(g + 4 * (uint32_t)n))
        n++;
    char *s = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        uint32_t ch = rd32(g + 4 * (uint32_t)i);
        s[i] = ch < 128 ? (char)ch : '?';
    }
    s[n] = 0;
    return s;
}
void I__swscanf(CPU *c) {
    char *in = narrow_w(ARG(0)), *f = narrow_w(ARG(1));
    RET((uint32_t)scan_core(c, in, f, c->esp + 4 + 8, 1));
    free(in);
    free(f);
}
static int scan_core(CPU *c, const char *in, const char *f, uint32_t va, int wide) {
    int assigned = 0;
    size_t pos = 0;
    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)in[pos]))
                pos++;
            f++;
            continue;
        }
        if (*f != '%') {
            if (in[pos] != *f)
                break;
            pos++;
            f++;
            continue;
        }
        if (f[1] == '%') {
            if (in[pos] != '%')
                break;
            pos++;
            f += 2;
            continue;
        }
        char spec[64];
        int k = 0;
        spec[k++] = *f++;
        int suppress = 0;
        if (*f == '*') {
            suppress = 1;
            spec[k++] = *f++;
        }
        while (isdigit((unsigned char)*f))
            spec[k++] = *f++;
        int l = 0, h = 0;
        while (*f == 'l' || *f == 'h' || *f == 'L' || *f == 'q') {
            if (*f == 'l' || *f == 'q')
                l++;
            if (*f == 'h')
                h++;
            spec[k++] = *f++;
        }
        char conv = *f++;
        if (conv == '[') {
            spec[k++] = '[';
            if (*f == '^')
                spec[k++] = *f++;
            if (*f == ']')
                spec[k++] = *f++;
            while (*f && *f != ']')
                spec[k++] = *f++;
            if (*f)
                spec[k++] = *f++;
        } else
            spec[k++] = conv;
        strcpy(spec + k, "%n");
        int n = -1, r;
        union {
            int i;
            long long ll;
            short s;
            char ch;
            float fl;
            double d;
            char buf[4096];
        } u;
        if (conv == 'n') {
            if (!suppress)
                wr32(rd32(va), (uint32_t)pos);
            va += 4;
            continue;
        }
        r = sscanf(in + pos, spec, (void *)&u, &n);
        if (suppress) {
            if (n < 0)
                break;
            pos += n;
            continue;
        }
        if (r != 1 || n < 0)
            break;
        uint32_t dst = rd32(va);
        va += 4;
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            if (l >= 2)
                wr64(dst, (uint64_t)u.ll);
            else if (h)
                wr16(dst, (uint16_t)u.s);
            else
                wr32(dst, (uint32_t)u.i);
            break;
        case 'f':
        case 'g':
        case 'e':
        case 'E':
        case 'G':
        case 'a':
            if (l)
                wrd(dst, u.d);
            else
                wrf(dst, u.fl);
            break;
        case 'c':
            if (wide)
                wr32(dst, (uint8_t)u.ch);
            else
                wr8(dst, (uint8_t)u.ch);
            break;
        case 's':
        case '[':
            if (wide) {
                size_t i = 0;
                for (; u.buf[i]; i++)
                    wr32(dst + 4 * (uint32_t)i, (uint8_t)u.buf[i]);
                wr32(dst + 4 * (uint32_t)i, 0);
            } else
                strcpy(S(dst), u.buf);
            break;
        default:
            fprintf(stderr, "[rt] sscanf %%%c unsupported\n", conv);
            abort();
        }
        pos += n;
        assigned++;
    }
    return assigned ? assigned : (in[0] ? 0 : -1);
}

// ---------- math ----------
void I__floor(CPU *c) {
    RETF(floor(ARGD(0)));
}
void I__ceil(CPU *c) {
    RETF(ceil(ARGD(0)));
}
void I__sin(CPU *c) {
    RETF(sin(ARGD(0)));
}
void I__cos(CPU *c) {
    RETF(cos(ARGD(0)));
}
void I__tan(CPU *c) {
    RETF(tan(ARGD(0)));
}
void I__exp(CPU *c) {
    RETF(exp(ARGD(0)));
}
void I__log(CPU *c) {
    RETF(log(ARGD(0)));
}
void I__powf(CPU *c) {
    RETF(powf(ARGF(0), ARGF(1)));
}
void I__fmodf(CPU *c) {
    RETF(fmodf(ARGF(0), ARGF(1)));
}
void I____fpclassifyf(CPU *c) {
    RET(fpclassify(ARGF(0)));
}

// ---------- qsort with a guest comparator ----------
static void gqsort(CPU *c, uint32_t base, uint32_t n, uint32_t sz, uint32_t cmp) {
    // insertion sort for small, otherwise heap-free merge via temp buffer (stable enough)
    uint32_t tmp = gmalloc(sz);
    for (uint32_t i = 1; i < n; i++) {
        memcpy(M + tmp, M + base + i * sz, sz);
        uint32_t j = i;
        while (j > 0) {
            uint32_t a[2] = {base + (j - 1) * sz, tmp};
            if ((int32_t)guest_call(c, cmp, 2, a) <= 0)
                break;
            memcpy(M + base + j * sz, M + base + (j - 1) * sz, sz);
            j--;
        }
        memcpy(M + base + j * sz, M + tmp, sz);
    }
    gfree(tmp);
}
void I__qsort(CPU *c) {
    gqsort(c, ARG(0), ARG(1), ARG(2), ARG(3));
}

// ---------- time ----------
void I__gettimeofday(CPU *c) {
    struct timeval tv;
    int r = gettimeofday(&tv, NULL);
    if (ARG(0)) {
        wr32(ARG(0), (uint32_t)tv.tv_sec);
        wr32(ARG(0) + 4, (uint32_t)tv.tv_usec);
    }
    RET(r);
}
void I__nanosleep_UNIX2003(CPU *c) {
    struct timespec ts = {(time_t)(int32_t)rd32(ARG(0)), (long)(int32_t)rd32(ARG(0) + 4)};
    RET(nanosleep(&ts, NULL));
}
void I__Microseconds(CPU *c) { // UnsignedWide* {lo, hi} on Intel
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t us = (uint64_t)tv.tv_sec * 1000000u + tv.tv_usec;
    wr32(ARG(0), (uint32_t)us);
    wr32(ARG(0) + 4, (uint32_t)(us >> 32));
}
void I__asctime(CPU *c) {
    static uint32_t g;
    if (!g)
        g = gmalloc(64);
    strcpy(S(g), "Thu Jan  1 00:00:00 1970\n");
    RET(g);
}
void I__Gestalt(CPU *c) {
    uint32_t sel = ARG(0), v = 0;
    switch (sel) {
    case 0x73797376:
        v = 0x1068;
        break; // 'sysv'
    case 0x73797331:
        v = 10;
        break; // 'sys1'
    case 0x73797332:
        v = 6;
        break; // 'sys2'
    case 0x73797333:
        v = 8;
        break; // 'sys3'
    case 0x70636c6b:
        v = 2000000000;
        break; // 'pclk'
    case 0x6d637075:
        v = 2000;
        break; // 'mcpu'
    case 0x72616d20:
        v = 0x7FFFFFFF;
        break; // 'ram '
    }
    if (ARG(1))
        wr32(ARG(1), v);
    RET(0);
}
void I__sysctlbyname(CPU *c) {
    const char *name = S(ARG(0));
    uint32_t oldp = ARG(1), lenp = ARG(2);
    if (!strcmp(name, "hw.cpufrequency") || !strcmp(name, "hw.cpufrequency_max")) {
        uint64_t f = 2400000000ull;
        uint32_t cap = lenp ? rd32(lenp) : 8;
        if (oldp) {
            if (cap >= 8)
                wr64(oldp, f);
            else
                wr32(oldp, (uint32_t)f);
        }
        if (lenp)
            wr32(lenp, cap >= 8 ? 8 : 4);
        RET(0);
        return;
    }
    char buf[512];
    size_t len = sizeof buf;
    int r = sysctlbyname(name, buf, &len, NULL, 0);
    if (r == 0 && oldp && lenp) {
        uint32_t cap = rd32(lenp);
        if (len > cap)
            len = cap;
        memcpy(M + oldp, buf, len);
    }
    if (r == 0 && lenp)
        wr32(lenp, (uint32_t)len);
    RET(r);
}

// ---------- processes / misc ----------
void I__getpid(CPU *c) {
    RET(getpid());
}
void I__getuid(CPU *c) {
    RET(getuid());
}
void I__geteuid(CPU *c) {
    RET(geteuid());
}
void I__getgid(CPU *c) {
    RET(getgid());
}
void I__getegid(CPU *c) {
    RET(getegid());
}
void I__abort(CPU *c) {
    fprintf(stderr, "[rt] guest abort() from %s\n", rt_func_name(rd32(c->esp)));
    abort();
}
void I____assert_rtn(CPU *c) {
    fprintf(stderr, "[rt] guest assertion failed: %s (%s:%d in %s)\n", S(ARG(3)), S(ARG(1)), (int)ARG(2), S(ARG(0)));
    abort();
}
void I__signal(CPU *c) {
    RET(0);
}
void I__sigaction(CPU *c) {
    RET(0);
}
void I__madvise(CPU *c) {
    RET(0);
}
void I__tcgetattr(CPU *c) {
    RET(-1);
}
void I__tcsetattr(CPU *c) {
    RET(-1);
}

// ---------- file descriptors ----------
static int trace_files = -1;
static void tf(const char *what, const char *p, long r) {
    if (trace_files < 0)
        trace_files = getenv("RT32_TRACE_FILES") != NULL;
    if (trace_files)
        fprintf(stderr, "[file] %s %s -> %ld\n", what, p, r);
}
void I__open_UNIX2003(CPU *c) {
    int fd = open(S(ARG(0)), (int)ARG(1), (int)ARG(2));
    if (fd < 0) {
        if (errno != ENOENT)
            fprintf(stderr, "[file] open %s failed: %s\n", S(ARG(0)), strerror(errno));
        seterr();
    }
    tf("open", S(ARG(0)), fd);
    RET(fd);
}
void I__close_UNIX2003(CPU *c) {
    RET(close((int)ARG(0)));
}
void rt_touch_write(uint32_t g, uint32_t n);
extern volatile uint64_t rt_hitch_io_ticks, rt_hitch_io_bytes;
void I__read_UNIX2003(CPU *c) {
    MC(ARG(1), ARG(2));
    rt_touch_write(ARG(1), ARG(2));
    uint64_t t0 = mach_absolute_time();
    ssize_t r = read((int)ARG(0), S(ARG(1)), ARG(2));
    rt_hitch_io_ticks += mach_absolute_time() - t0;
    if (r > 0)
        rt_hitch_io_bytes += (uint64_t)r;
    RET(r);
}
void I__write_UNIX2003(CPU *c) {
    RET(write((int)ARG(0), S(ARG(1)), ARG(2)));
}
void I__lseek(CPU *c) {
    RET64(lseek((int)ARG(0), (off_t)(int64_t)ARG64(1), (int)ARG(3)));
}
void I__ftruncate(CPU *c) {
    RET(ftruncate((int)ARG(0), (off_t)(int64_t)ARG64(1)));
}
void I__fcntl_UNIX2003(CPU *c) {
    RET(fcntl((int)ARG(0), (int)ARG(1), (int)ARG(2)));
}
void I__access(CPU *c) {
    int r = access(S(ARG(0)), (int)ARG(1));
    if (r)
        seterr();
    RET(r);
}
void I__unlink(CPU *c) {
    RET(unlink(S(ARG(0))));
}
void I__mkdir(CPU *c) {
    int r = mkdir(S(ARG(0)), (mode_t)ARG(1));
    if (r)
        seterr();
    RET(r);
}
void I__chdir(CPU *c) {
    RET(chdir(S(ARG(0))));
}
void I__chmod_UNIX2003(CPU *c) {
    RET(chmod(S(ARG(0)), (mode_t)ARG(1)));
}
void I__getcwd(CPU *c) {
    char b[1024];
    if (!getcwd(b, sizeof b)) {
        RET(0);
        return;
    }
    uint32_t dst = ARG(0);
    if (!dst)
        dst = gmalloc((uint32_t)strlen(b) + 1);
    else if (strlen(b) + 1 > ARG(1)) {
        RET(0);
        return;
    }
    strcpy(S(dst), b);
    RET(dst);
}
// mmap: guest gets a private copy (writes are not written back; MAP_SHARED writers use msync)
void I__mmap_UNIX2003(CPU *c) {
    uint32_t len = ARG(1);
    int flags = (int)ARG(3), fd = (int)ARG(4);
    off_t off = (off_t)(int64_t)ARG64(5);
    uint32_t g = gmalloc(len);
    memset(M + g, 0, len);
    if (!(flags & MAP_ANON) && fd >= 0) {
        ssize_t r = pread(fd, M + g, len, off);
        (void)r;
    }
    RET(g);
}
void I__msync_UNIX2003(CPU *c) {
    RET(0);
}

static void stat_to_guest(const struct stat *st, uint32_t g, int inode64) {
    if (!inode64) { // legacy i386 struct stat (96 bytes)
        memset(M + g, 0, 96);
        wr32(g + 0, (uint32_t)st->st_dev);
        wr32(g + 4, (uint32_t)st->st_ino);
        wr16(g + 8, st->st_mode);
        wr16(g + 10, st->st_nlink);
        wr32(g + 12, st->st_uid);
        wr32(g + 16, st->st_gid);
        wr32(g + 20, (uint32_t)st->st_rdev);
        wr32(g + 24, (uint32_t)st->st_atimespec.tv_sec);
        wr32(g + 28, (uint32_t)st->st_atimespec.tv_nsec);
        wr32(g + 32, (uint32_t)st->st_mtimespec.tv_sec);
        wr32(g + 36, (uint32_t)st->st_mtimespec.tv_nsec);
        wr32(g + 40, (uint32_t)st->st_ctimespec.tv_sec);
        wr32(g + 44, (uint32_t)st->st_ctimespec.tv_nsec);
        wr64(g + 48, (uint64_t)st->st_size);
        wr64(g + 56, (uint64_t)st->st_blocks);
        wr32(g + 64, (uint32_t)st->st_blksize);
    } else { // i386 struct stat64 (108 bytes, 4-byte aligned 64-bit fields)
        memset(M + g, 0, 108);
        wr32(g + 0, (uint32_t)st->st_dev);
        wr16(g + 4, st->st_mode);
        wr16(g + 6, st->st_nlink);
        wr64(g + 8, st->st_ino);
        wr32(g + 16, st->st_uid);
        wr32(g + 20, st->st_gid);
        wr32(g + 24, (uint32_t)st->st_rdev);
        wr32(g + 28, (uint32_t)st->st_atimespec.tv_sec);
        wr32(g + 32, (uint32_t)st->st_atimespec.tv_nsec);
        wr32(g + 36, (uint32_t)st->st_mtimespec.tv_sec);
        wr32(g + 40, (uint32_t)st->st_mtimespec.tv_nsec);
        wr32(g + 44, (uint32_t)st->st_ctimespec.tv_sec);
        wr32(g + 48, (uint32_t)st->st_ctimespec.tv_nsec);
        wr32(g + 52, (uint32_t)st->st_birthtimespec.tv_sec);
        wr32(g + 56, (uint32_t)st->st_birthtimespec.tv_nsec);
        wr64(g + 60, (uint64_t)st->st_size);
        wr64(g + 68, (uint64_t)st->st_blocks);
        wr32(g + 76, (uint32_t)st->st_blksize);
    }
}
void I__stat(CPU *c) {
    struct stat st;
    int r = stat(S(ARG(0)), &st);
    tf("stat", S(ARG(0)), r);
    if (!r)
        stat_to_guest(&st, ARG(1), 0);
    else
        seterr();
    RET(r);
}
void I__stat_INODE64(CPU *c) {
    struct stat st;
    int r = stat(S(ARG(0)), &st);
    tf("stat", S(ARG(0)), r);
    if (!r)
        stat_to_guest(&st, ARG(1), 1);
    else
        seterr();
    RET(r);
}
void I__fstat(CPU *c) {
    struct stat st;
    int r = fstat((int)ARG(0), &st);
    if (!r)
        stat_to_guest(&st, ARG(1), 0);
    RET(r);
}
void I__fstat_INODE64(CPU *c) {
    struct stat st;
    int r = fstat((int)ARG(0), &st);
    if (!r)
        stat_to_guest(&st, ARG(1), 1);
    RET(r);
}

// ---------- directories: guest DIR = {magic, host index, dirent buffer...} ----------
#define NDIRS 64
static DIR *hdirs[NDIRS];
static uint32_t gopendir(const char *p) {
    DIR *d = opendir(p);
    if (!d) {
        seterr();
        return 0;
    }
    for (int i = 0; i < NDIRS; i++)
        if (!hdirs[i]) {
            hdirs[i] = d;
            uint32_t g = gmalloc(16 + 1080);
            wr32(g, 0x44495221);
            wr32(g + 4, (uint32_t)i);
            return g;
        }
    closedir(d);
    return 0;
}
void I__opendir_UNIX2003(CPU *c) {
    uint32_t d = gopendir(S(ARG(0)));
    tf("opendir", S(ARG(0)), d != 0);
    RET(d);
}
void I__opendir_INODE64_UNIX2003(CPU *c) {
    uint32_t d = gopendir(S(ARG(0)));
    tf("opendir", S(ARG(0)), d != 0);
    RET(d);
}
void I__closedir_UNIX2003(CPU *c) {
    uint32_t g = ARG(0);
    int i = (int)rd32(g + 4);
    closedir(hdirs[i]);
    hdirs[i] = NULL;
    gfree(g);
    RET(0);
}
static void greaddir(CPU *c, int inode64) {
    uint32_t g = ARG(0);
    struct dirent *e = readdir(hdirs[rd32(g + 4)]);
    if (!e) {
        RET(0);
        return;
    }
    uint32_t de = g + 16;
    memset(M + de, 0, 1080);
    if (!inode64) {
        wr32(de, (uint32_t)e->d_ino);
        wr16(de + 4, 264);
        wr8(de + 6, e->d_type);
        size_t n = strlen(e->d_name);
        if (n > 255)
            n = 255;
        wr8(de + 7, (uint8_t)n);
        memcpy(M + de + 8, e->d_name, n);
    } else {
        wr64(de, e->d_ino);
        wr64(de + 8, e->d_seekoff);
        wr16(de + 16, 1048);
        size_t n = strlen(e->d_name);
        if (n > 1023)
            n = 1023;
        wr16(de + 18, (uint16_t)n);
        wr8(de + 20, e->d_type);
        memcpy(M + de + 21, e->d_name, n);
    }
    RET(de);
}
void I__readdir(CPU *c) {
    greaddir(c, 0);
}
void I__readdir_INODE64(CPU *c) {
    greaddir(c, 1);
}

// ---------- stdio extras ----------
void I__fopen_DARWIN_EXTSN(CPU *c) {
    FILE *f = fopen(S(ARG(0)), S(ARG(1)));
    if (!f) {
        if (errno != ENOENT)
            fprintf(stderr, "[file] fopen %s failed: %s\n", S(ARG(0)), strerror(errno));
        seterr();
    }
    tf("fopen", S(ARG(0)), f != NULL);
    RET(gfile_wrap(f));
}
void I__feof(CPU *c) {
    RET(feof(gfile_host(ARG(0))));
}
void I__fgetc(CPU *c) {
    RET(fgetc(gfile_host(ARG(0))));
}
void I__fileno(CPU *c) {
    RET(fileno(gfile_host(ARG(0))));
}

// ---------- POSIX named semaphores ----------
#define NSEMS 256
static sem_t *hsems[NSEMS];
void I__sem_open(CPU *c) {
    sem_t *s = sem_open(S(ARG(0)), (int)ARG(1), (mode_t)ARG(2), ARG(3));
    if (s == SEM_FAILED) {
        seterr();
        RET(-1);
        return;
    }
    for (int i = 1; i < NSEMS; i++)
        if (!hsems[i]) {
            hsems[i] = s;
            RET(i);
            return;
        }
    RET(-1);
}
void I__sem_close(CPU *c) {
    int i = (int)ARG(0);
    RET(sem_close(hsems[i]));
    hsems[i] = NULL;
}
void I__sem_unlink(CPU *c) {
    RET(sem_unlink(S(ARG(0))));
}
void I__sem_post(CPU *c) {
    RET(sem_post(hsems[ARG(0)]));
}
void I__sem_wait_UNIX2003(CPU *c) {
    RET(sem_wait(hsems[ARG(0)]));
}
void I__sem_trywait(CPU *c) {
    RET(sem_trywait(hsems[ARG(0)]));
}

// ---------- sockets: networking features are disabled in the port ----------
#define NONET(n)                                                                                                       \
    void I__##n(CPU *c) {                                                                                              \
        errno = ENETDOWN;                                                                                              \
        seterr();                                                                                                      \
        RET(-1);                                                                                                       \
    }
NONET(socket)
NONET(connect_UNIX2003)
NONET(bind_UNIX2003)
NONET(listen_UNIX2003)
NONET(accept_UNIX2003)
NONET(send_UNIX2003)
NONET(sendto_UNIX2003) NONET(recv_UNIX2003) NONET(recvfrom_UNIX2003) NONET(setsockopt) NONET(getsockopt) NONET(shutdown)
    NONET(getpeername_UNIX2003) NONET(getsockname_UNIX2003) NONET(select_UNIX2003)
        NONET(select_DARWIN_EXTSN) void I__gethostbyname(CPU *c) {
    RET(0);
}

// ---- added for BioShock Infinite: Darwin / compiler-rt helpers ----
void I____bzero(CPU *c) {
    MC(ARG(0), ARG(1));
    memset(S(ARG(0)), 0, ARG(1));
}
void I____fixunsdfdi(CPU *c) { // double -> unsigned 64-bit, result in edx:eax
    double d = ARGD(0);
    uint64_t r = d <= 0 ? 0 : (uint64_t)d;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}
void I____fixunssfdi(CPU *c) { // float -> unsigned 64-bit, result in edx:eax
    float f = ARGF(0);
    uint64_t r = f <= 0 ? 0 : (uint64_t)f;
    c->eax = (uint32_t)r;
    c->edx = (uint32_t)(r >> 32);
}
void I____isfinitef(CPU *c) {
    float f = ARGF(0);
    RET((f == f && f - f == 0) ? 1 : 0);
}
// ---- end additions ----
// munmap pairs with the gmalloc-backed mmap above
void I__munmap_UNIX2003(CPU *c) {
    if (ARG(0))
        gfree(ARG(0));
    RET(0);
}

// Darwin memset_pattern4/8/16(void *b, const void *pattern, size_t len): repeat the pattern, last copy truncated
static void memset_pat(CPU *c, uint32_t psz) {
    uint32_t dst = ARG(0), pat = ARG(1), len = ARG(2);
    MC(dst, len);
    for (uint32_t i = 0; i < len; i++)
        M[dst + i] = M[pat + (i % psz)];
}
void I__memset_pattern4(CPU *c) {
    memset_pat(c, 4);
}
void I__memset_pattern8(CPU *c) {
    memset_pat(c, 8);
}
void I__memset_pattern16(CPU *c) {
    memset_pat(c, 16);
}

// BSD strlcpy/strlcat on guest memory; both return the length they tried to create
void I__strlcpy(CPU *c) {
    uint32_t dst = ARG(0), src = ARG(1), size = ARG(2), n = (uint32_t)strlen(S(src));
    if (size) {
        uint32_t k = n >= size ? size - 1 : n;
        MC(dst, k + 1);
        memmove(M + dst, M + src, k);
        M[dst + k] = 0;
    }
    RET(n);
}
void I__strlcat(CPU *c) {
    uint32_t dst = ARG(0), src = ARG(1), size = ARG(2), sl = (uint32_t)strlen(S(src)), dl = 0;
    while (dl < size && M[dst + dl])
        dl++;
    if (dl == size) {
        RET(size + sl);
        return;
    } // dst not NUL-terminated within size
    uint32_t room = size - dl - 1, k = sl > room ? room : sl;
    MC(dst + dl, k + 1);
    memmove(M + dst + dl, M + src, k);
    M[dst + dl + k] = 0;
    RET(dl + sl);
}

// ---- added for BioShock Infinite: wide-character (wchar_t = 32-bit) libc on guest memory ----
#include <wchar.h>
#define WP(a)   ((wchar_t *)(M + (a)))
#define G_OF(p) ((uint32_t)((uint8_t *)(p) - M))
void I__wcslen(CPU *c) {
    RET(wcslen(WP(ARG(0))));
}
void I__wcscpy(CPU *c) {
    MC(ARG(0), 4 * ((uint32_t)wcslen(WP(ARG(1))) + 1));
    wcscpy(WP(ARG(0)), WP(ARG(1)));
    RET(ARG(0));
}
void I__wcsncpy(CPU *c) {
    MC(ARG(0), 4 * ARG(2));
    wcsncpy(WP(ARG(0)), WP(ARG(1)), ARG(2));
    RET(ARG(0));
}
void I__wcscat(CPU *c) {
    MC(ARG(0), 4 * ((uint32_t)(wcslen(WP(ARG(0))) + wcslen(WP(ARG(1)))) + 1));
    wcscat(WP(ARG(0)), WP(ARG(1)));
    RET(ARG(0));
}
void I__wcschr(CPU *c) {
    wchar_t *r = wcschr(WP(ARG(0)), (wchar_t)ARG(1));
    RET(r ? G_OF(r) : 0);
}
void I__wcsrchr(CPU *c) {
    wchar_t *r = wcsrchr(WP(ARG(0)), (wchar_t)ARG(1));
    RET(r ? G_OF(r) : 0);
}
void I__wcsstr(CPU *c) {
    wchar_t *r = wcsstr(WP(ARG(0)), WP(ARG(1)));
    RET(r ? G_OF(r) : 0);
}
void I__wcscmp(CPU *c) {
    RET(wcscmp(WP(ARG(0)), WP(ARG(1))));
}
void I__wcsncmp(CPU *c) {
    RET(wcsncmp(WP(ARG(0)), WP(ARG(1)), ARG(2)));
}
void I__wcslcpy(CPU *c) {
    uint32_t dst = ARG(0), src = ARG(1), size = ARG(2), n = (uint32_t)wcslen(WP(src));
    if (size) {
        uint32_t k = n >= size ? size - 1 : n;
        MC(dst, 4 * (k + 1));
        memmove(M + dst, M + src, 4 * k);
        wr32(dst + 4 * k, 0);
    }
    RET(n);
}
void I__wcslcat(CPU *c) {
    uint32_t dst = ARG(0), src = ARG(1), size = ARG(2), sl = (uint32_t)wcslen(WP(src)), dl = 0;
    while (dl < size && rd32(dst + 4 * dl))
        dl++;
    if (dl == size) {
        RET(size + sl);
        return;
    }
    uint32_t room = size - dl - 1, k = sl > room ? room : sl;
    MC(dst + 4 * dl, 4 * (k + 1));
    memmove(M + dst + 4 * dl, M + src, 4 * k);
    wr32(dst + 4 * (dl + k), 0);
    RET(dl + sl);
}
static int w_in_sep(uint32_t sep, uint32_t ch) {
    for (uint32_t k = 0; rd32(sep + 4 * k); k++)
        if (rd32(sep + 4 * k) == ch)
            return 1;
    return 0;
}
void I__wcstok(CPU *c) { // (ws, sep, wchar_t **last); the saved position is a 32-bit guest pointer
    uint32_t s = ARG(0), sep = ARG(1), lastp = ARG(2);
    if (!s)
        s = rd32(lastp);
    if (!s) {
        RET(0);
        return;
    }
    while (rd32(s) && w_in_sep(sep, rd32(s)))
        s += 4;
    if (!rd32(s)) {
        wr32(lastp, 0);
        RET(0);
        return;
    }
    uint32_t tok = s;
    while (rd32(s) && !w_in_sep(sep, rd32(s)))
        s += 4;
    if (rd32(s)) {
        wr32(s, 0);
        wr32(lastp, s + 4);
    } else
        wr32(lastp, 0);
    RET(tok);
}
static void w_endptr(uint32_t ep, wchar_t *end) {
    if (ep)
        wr32(ep, G_OF(end));
}
void I__wcstol(CPU *c) {
    wchar_t *e;
    long v = wcstol(WP(ARG(0)), &e, (int)ARG(2));
    w_endptr(ARG(1), e);
    RET((uint32_t)(v > 0x7FFFFFFFL ? 0x7FFFFFFFL : v < -0x80000000L ? -0x80000000L : v));
}
void I__wcstoul(CPU *c) {
    wchar_t *e;
    unsigned long v = wcstoul(WP(ARG(0)), &e, (int)ARG(2));
    w_endptr(ARG(1), e);
    RET((uint32_t)(v > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : v));
}
void I__wcstoll(CPU *c) {
    wchar_t *e;
    long long v = wcstoll(WP(ARG(0)), &e, (int)ARG(2));
    w_endptr(ARG(1), e);
    RET64(v);
}
void I__wcstoull(CPU *c) {
    wchar_t *e;
    unsigned long long v = wcstoull(WP(ARG(0)), &e, (int)ARG(2));
    w_endptr(ARG(1), e);
    RET64(v);
}
void I__wcstod(CPU *c) {
    wchar_t *e;
    double v = wcstod(WP(ARG(0)), &e);
    w_endptr(ARG(1), e);
    RETF(v);
}
void I__wcstombs(CPU *c) { // UTF-8 encode; dst == 0 counts only
    uint32_t dst = ARG(0), src = ARG(1), cap = ARG(2), n = 0;
    for (;; src += 4) {
        uint32_t ch = rd32(src);
        char t[4];
        uint32_t l;
        if (!ch) {
            if (dst && n < cap)
                M[dst + n] = 0;
            break;
        }
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
        if (dst) {
            if (n + l > cap)
                break;
            memcpy(M + dst + n, t, l);
        }
        n += l;
    }
    RET(n);
}

// wide printf family: the wide format becomes a UTF-8 format, gformat_ext formats it, and the result is decoded back
static int wformat(uint32_t dst, uint32_t cap, uint32_t wfmt, uint32_t va, char *out, size_t outcap) {
    char nf[4096];
    size_t n = 0;
    for (uint32_t p = wfmt; n + 5 < sizeof nf; p += 4) {
        uint32_t ch = rd32(p);
        if (!ch)
            break;
        if (ch < 0x80)
            nf[n++] = (char)ch;
        else if (ch < 0x800) {
            nf[n++] = (char)(0xC0 | (ch >> 6));
            nf[n++] = (char)(0x80 | (ch & 63));
        } else if (ch < 0x10000) {
            nf[n++] = (char)(0xE0 | (ch >> 12));
            nf[n++] = (char)(0x80 | ((ch >> 6) & 63));
            nf[n++] = (char)(0x80 | (ch & 63));
        } else {
            nf[n++] = (char)(0xF0 | (ch >> 18));
            nf[n++] = (char)(0x80 | ((ch >> 12) & 63));
            nf[n++] = (char)(0x80 | ((ch >> 6) & 63));
            nf[n++] = (char)(0x80 | (ch & 63));
        }
    }
    nf[n] = 0;
    int len = gformat_ext(out, outcap, nf, va);
    if (len < 0 || (size_t)len >= outcap)
        return -1;
    if (!dst)
        return len;
    uint32_t w = 0; // decode UTF-8 into dst (cap wide chars including the terminator)
    for (int i = 0; i < len;) {
        unsigned char b = (unsigned char)out[i];
        uint32_t cp;
        int l;
        if (b < 0x80) {
            cp = b;
            l = 1;
        } else if ((b >> 5) == 6 && i + 1 < len) {
            cp = ((b & 31) << 6) | (out[i + 1] & 63);
            l = 2;
        } else if ((b >> 4) == 14 && i + 2 < len) {
            cp = ((b & 15) << 12) | ((out[i + 1] & 63) << 6) | (out[i + 2] & 63);
            l = 3;
        } else if ((b >> 3) == 30 && i + 3 < len) {
            cp = ((b & 7) << 18) | ((out[i + 1] & 63) << 12) | ((out[i + 2] & 63) << 6) | (out[i + 3] & 63);
            l = 4;
        } else {
            cp = b;
            l = 1;
        }
        if (w + 1 >= cap) {
            if (cap)
                wr32(dst + 4 * (cap - 1), 0);
            return -1;
        } // swprintf: -1 when it does not fit
        wr32(dst + 4 * w++, cp);
        i += l;
    }
    if (cap)
        wr32(dst + 4 * w, 0);
    return (int)w;
}
void I__swprintf(CPU *c) {
    static _Thread_local char out[1 << 16];
    RET(wformat(ARG(0), ARG(1), ARG(2), c->esp + 4 + 12, out, sizeof out));
}
void I__vswprintf(CPU *c) {
    static _Thread_local char out[1 << 16];
    RET(wformat(ARG(0), ARG(1), ARG(2), ARG(3), out, sizeof out));
}
void I__wprintf(CPU *c) {
    static _Thread_local char out[1 << 16];
    int n = wformat(0, 0, ARG(0), c->esp + 4 + 4, out, sizeof out);
    if (n >= 0)
        fputs(out, stdout);
    RET(n);
}
// ---- end additions ----

// ---- remaining libm: doubles take 2 arg slots, floats 1; results go back on the x87 stack ----
void I__acos(CPU *c) {
    RETF(acos(ARGD(0)));
}
void I__asin(CPU *c) {
    RETF(asin(ARGD(0)));
}
void I__atan(CPU *c) {
    RETF(atan(ARGD(0)));
}
void I__log10(CPU *c) {
    RETF(log10(ARGD(0)));
}
void I__atan2f(CPU *c) {
    RETF(atan2f(ARGF(0), ARGF(1)));
}

// ---- small libc batch (BioShock Infinite) ----
#include <sys/time.h>
#include <CommonCrypto/CommonDigest.h>
void I__strspn(CPU *c) {
    RET(strspn(S(ARG(0)), S(ARG(1))));
}
void I__strtoll(CPU *c) { // (str, char **end, base) -> long long in edx:eax
    char *e = NULL;
    long long v = strtoll(S(ARG(0)), ARG(1) ? &e : NULL, (int)ARG(2));
    if (ARG(1))
        wr32(ARG(1), ARG(0) + (uint32_t)(e - S(ARG(0))));
    c->eax = (uint32_t)(uint64_t)v;
    c->edx = (uint32_t)((uint64_t)v >> 32);
}
void I__putchar(CPU *c) {
    RET(putchar((int)ARG(0)));
}
void I__gethostname(CPU *c) {
    int r = gethostname(S(ARG(0)), (size_t)ARG(1));
    if (r)
        seterr();
    RET(r);
}
void I__utimes(CPU *c) { // (path, const struct timeval[2]) : 32-bit timeval is {int32 sec, int32 usec}
    struct timeval tv[2], *p = NULL;
    if (ARG(1)) {
        for (int i = 0; i < 2; i++) {
            tv[i].tv_sec = (int32_t)rd32(ARG(1) + 8 * i);
            tv[i].tv_usec = (int32_t)rd32(ARG(1) + 8 * i + 4);
        }
        p = tv;
    }
    int r = utimes(S(ARG(0)), p);
    if (r)
        seterr();
    RET(r);
}
void I___exit(CPU *c) {
    fprintf(stderr, "[rt] guest called _exit(%d)\n", (int)ARG(0));
    fflush(stderr);
    _exit((int)ARG(0));
}
void I__ptrace(CPU *c) {
    RET(0);
} // PT_DENY_ATTACH anti-debug: pretend it worked
void I__dlsym(CPU *c) {
    fprintf(stderr, "[rt] dlsym(\"%s\") -> NULL\n", S(ARG(1)));
    RET(0);
}
void I__flockfile(CPU *c) {
    (void)c;
}
void I__funlockfile(CPU *c) {
    (void)c;
}
void I__CC_MD5(CPU *c) {
    CC_MD5(G2H(ARG(0)), (CC_LONG)ARG(1), (unsigned char *)G2H(ARG(2)));
    RET(ARG(2));
}
// networking: report "no network" (BioShock Infinite is single-player; online features fail gracefully)
void I__getifaddrs(CPU *c) {
    if (ARG(0))
        wr32(ARG(0), 0);
    errno = ENOTSUP;
    seterr();
    RET((uint32_t)-1);
}
void I__freeifaddrs(CPU *c) {
    (void)c;
}
void I__ldexpf(CPU *c) {
    RETF(ldexpf(ARGF(0), (int)ARG(1)));
}
void I__exp2f(CPU *c) {
    RETF(exp2f(ARGF(0)));
}

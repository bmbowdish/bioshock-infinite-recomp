// System shims: Mach, sysctl, pthreads (guest threads run on host threads, one CPU each).
#include "rt.h"
#include "tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <unistd.h>

#define RET(v) (c->eax = (uint32_t)(v))

// ---------- guest-object -> host-object map (mutexes, conds, threads) ----------
typedef struct {
    uint32_t key;
    void *val;
} Ent;
static Ent *tab;
static uint32_t tabcap;
static pthread_mutex_t tablock = PTHREAD_MUTEX_INITIALIZER;
static void *map_get(uint32_t key, void *(*mk)(uint32_t)) {
    pthread_mutex_lock(&tablock);
    if (!tab) {
        tabcap = 1 << 16;
        tab = calloc(tabcap, sizeof(Ent));
    }
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key)
        h = (h + 1) & (tabcap - 1);
    void *v = tab[h].val;
    if (!tab[h].key && mk) {
        v = mk(key);
        tab[h].key = key;
        tab[h].val = v;
    }
    pthread_mutex_unlock(&tablock);
    return v;
}
static void map_set(uint32_t key, void *v) {
    pthread_mutex_lock(&tablock);
    if (!tab) {
        tabcap = 1 << 16;
        tab = calloc(tabcap, sizeof(Ent));
    }
    uint32_t h = (key * 2654435761u) & (tabcap - 1);
    while (tab[h].key && tab[h].key != key)
        h = (h + 1) & (tabcap - 1);
    tab[h].key = key;
    tab[h].val = v;
    pthread_mutex_unlock(&tablock);
}

// ---------- time / machine ----------
void I__mach_absolute_time(CPU *c) {
    uint64_t t = mach_absolute_time();
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
}
void I__mach_timebase_info(CPU *c) {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    wr32(ARG(0), i.numer);
    wr32(ARG(0) + 4, i.denom);
    RET(0);
}
void I__mach_host_self(CPU *c) {
    RET(1);
}
void I__mach_task_self(CPU *c) {
    RET(2);
}
void I__host_page_size(CPU *c) {
    wr32(ARG(1), 4096);
    RET(0);
}
void I__host_statistics(CPU *c) { // (host, flavor, info_out, count*) : report 1 GiB free, little used
    uint32_t out = ARG(2), n = rd32(ARG(3));
    memset(M + out, 0, 4 * n);
    if (n > 0)
        wr32(out + 0, (1u << 30) / 4096); // free_count
    if (n > 1)
        wr32(out + 4, (256u << 20) / 4096); // active_count
    RET(0);
}
void I__mstats(CPU *c) { // struct mstats returned via hidden pointer (callee pops it)
    uint32_t p = ARG(0);
    memset(M + p, 0, 20);
    RET(p);
    c->esp += 4;
}
void I__sysctl(CPU *c) { // (int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
    int name[8];
    uint32_t n = ARG(1);
    if (n > 8) {
        RET(-1);
        return;
    }
    for (uint32_t i = 0; i < n; i++)
        name[i] = (int)rd32(ARG(0) + 4 * i);
    uint32_t oldp = ARG(2), oldlenp = ARG(3);
    if (n == 2 && name[0] == CTL_HW && (name[1] == HW_PHYSMEM || name[1] == HW_USERMEM)) {
        if (oldp)
            wr32(oldp, 0x7FFFFFFFu); // a 32-bit process sees at most 2 GiB
        if (oldlenp)
            wr32(oldlenp, 4);
        RET(0);
        return;
    }
    if (n == 2 && name[0] == CTL_HW && name[1] == HW_MEMSIZE) {
        if (oldp)
            wr64(oldp, 2ull << 30);
        if (oldlenp)
            wr32(oldlenp, 8);
        RET(0);
        return;
    }
    uint8_t buf[256];
    size_t len = sizeof buf;
    int r = sysctl(name, n, buf, &len, NULL, 0);
    if (r == 0 && oldp && oldlenp) {
        uint32_t cap = rd32(oldlenp);
        if (len > cap)
            len = cap;
        memcpy(M + oldp, buf, len);
        wr32(oldlenp, (uint32_t)len);
    }
    RET(r);
}
void I__sched_yield(CPU *c) {
    RET(sched_yield());
}
void I__usleep_UNIX2003(CPU *c) {
    RET(usleep(ARG(0)));
}
void I__usleep(CPU *c) {
    RET(usleep(ARG(0)));
}

// ---------- mutexes (all made recursive-capable per attr type) ----------
// guest pthread_mutexattr_t: we keep the type in its first word
void I__pthread_mutexattr_init(CPU *c) {
    wr32(ARG(0), 0);
    wr32(ARG(0) + 4, 0);
    RET(0);
}
void I__pthread_mutexattr_destroy(CPU *c) {
    RET(0);
}
void I__pthread_mutexattr_settype(CPU *c) {
    wr32(ARG(0) + 4, ARG(1));
    RET(0);
}
static pthread_mutex_t *new_mutex(int type) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type == PTHREAD_MUTEX_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
    pthread_mutex_init(m, &a);
    return m;
}
static void *mk_mutex(uint32_t k) {
    (void)k;
    return new_mutex(PTHREAD_MUTEX_NORMAL);
}
static pthread_mutex_t *MX(uint32_t g) {
    return map_get(g, mk_mutex);
}
void I__pthread_mutex_init(CPU *c) {
    uint32_t attr = ARG(1);
    map_set(ARG(0), new_mutex(attr ? (int)rd32(attr + 4) : 0));
    RET(0);
}
void I__pthread_mutex_destroy(CPU *c) {
    RET(0);
}
void I__pthread_mutex_lock(CPU *c) {
    RET(pthread_mutex_lock(MX(ARG(0))));
}
void I__pthread_mutex_trylock(CPU *c) {
    RET(pthread_mutex_trylock(MX(ARG(0))));
}
void I__pthread_mutex_unlock(CPU *c) {
    RET(pthread_mutex_unlock(MX(ARG(0))));
}

// ---------- condition variables ----------
static void *mk_cond(uint32_t k) {
    (void)k;
    pthread_cond_t *v = malloc(sizeof *v);
    pthread_cond_init(v, NULL);
    return v;
}
static pthread_cond_t *CV(uint32_t g) {
    return map_get(g, mk_cond);
}
void I__pthread_cond_init_UNIX2003(CPU *c) {
    map_set(ARG(0), mk_cond(0));
    RET(0);
}
void I__pthread_cond_init(CPU *c) {
    map_set(ARG(0), mk_cond(0));
    RET(0);
}
void I__pthread_cond_destroy(CPU *c) {
    RET(0);
}
void I__pthread_cond_signal(CPU *c) {
    RET(pthread_cond_signal(CV(ARG(0))));
}
void I__pthread_cond_broadcast(CPU *c) {
    RET(pthread_cond_broadcast(CV(ARG(0))));
}
void I__pthread_cond_wait_UNIX2003(CPU *c) {
    RET(pthread_cond_wait(CV(ARG(0)), MX(ARG(1))));
}
void I__pthread_cond_timedwait_UNIX2003(CPU *c) {
    struct timespec ts = {(time_t)(int32_t)rd32(ARG(2)), (long)(int32_t)rd32(ARG(2) + 4)};
    RET(pthread_cond_timedwait(CV(ARG(0)), MX(ARG(1)), &ts));
}

// ---------- threads ----------
// guest pthread_t is a small integer handle; attr keeps the stack size in word 1
typedef struct {
    pthread_t host;
    CPU *cpu;
    uint32_t start, arg, ret;
} GThread;
static GThread threads[256];
static int nthreads = 1; // handle 1 = main thread
static _Thread_local uint32_t cur_handle = 1;
static void *thread_main(void *p) {
    GThread *t = p;
    static const char *dummy;
    (void)dummy;
    cur_handle = (uint32_t)(t - threads) + 1;
    uint32_t a[1] = {t->arg};
    t->ret = guest_call(t->cpu, t->start, 1, a);
    return NULL;
}
void I__pthread_attr_init(CPU *c) {
    wr32(ARG(0), 0);
    wr32(ARG(0) + 4, 512 * 1024);
    RET(0);
}
void I__pthread_attr_destroy(CPU *c) {
    RET(0);
}
void I__pthread_attr_setstacksize(CPU *c) {
    wr32(ARG(0) + 4, ARG(1));
    RET(0);
}
void I__pthread_create(CPU *c) { // (pthread_t *thr, attr, start, arg)
    int h = __atomic_fetch_add(&nthreads, 1, __ATOMIC_SEQ_CST);
    GThread *t = &threads[h];
    uint32_t ss = ARG(1) ? rd32(ARG(1) + 4) : 512 * 1024;
    if (ss < 256 * 1024)
        ss = 256 * 1024;
    t->cpu = cpu_new(ss);
    t->start = ARG(2);
    t->arg = ARG(3);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20); // host stack for deep recursion
    int r = pthread_create(&t->host, &a, thread_main, t);
    wr32(ARG(0), (uint32_t)h + 1);
    RET(r);
}
void I__pthread_join_UNIX2003(CPU *c) {
    GThread *t = &threads[ARG(0) - 1];
    int r = pthread_join(t->host, NULL);
    if (ARG(1))
        wr32(ARG(1), t->ret);
    RET(r);
}
void I__pthread_self(CPU *c) {
    RET(cur_handle);
}
void I__pthread_exit(CPU *c) {
    pthread_exit(NULL);
}
void I__pthread_detach(CPU *c) {
    RET(0);
}

// ---- added for BioShock Infinite: UNIX2003 variants and small pthread/rlimit shims ----
void I__pthread_mutexattr_destroy_UNIX2003(CPU *c) {
    RET(0);
}
void I__pthread_cancel_UNIX2003(CPU *c) {
    RET(0);
} // threads are never cancelled
void I__pthread_testcancel_UNIX2003(CPU *c) {
    (void)c;
}
void I__getrlimit_UNIX2003(CPU *c) { // struct rlimit { rlim_t cur, max; } with 64-bit rlim_t: report "unlimited"
    uint32_t p = ARG(1);
    if (p) {
        wr64(p, 0x7FFFFFFFFFFFFFFFull);
        wr64(p + 8, 0x7FFFFFFFFFFFFFFFull);
    }
    RET(0);
}
void I__setrlimit_UNIX2003(CPU *c) {
    RET(0);
}
// ---- end additions ----

// ---- added for BioShock Infinite: thread-local storage keys and remaining pthread attribute shims ----
// The guest key is the host pthread_key_t value; stored values are 32-bit guest words. Destructors are not run.
void I__pthread_key_create(CPU *c) {
    pthread_key_t k;
    int r = pthread_key_create(&k, NULL);
    if (r == 0)
        wr32(ARG(0), (uint32_t)k);
    RET(r);
}
void I__pthread_key_delete(CPU *c) {
    RET(pthread_key_delete((pthread_key_t)ARG(0)));
}
void I__pthread_getspecific(CPU *c) {
    RET((uint32_t)(uintptr_t)pthread_getspecific((pthread_key_t)ARG(0)));
}
void I__pthread_setspecific(CPU *c) {
    RET(pthread_setspecific((pthread_key_t)ARG(0), (void *)(uintptr_t)ARG(1)));
}
void I__pthread_main_np(CPU *c) {
    RET(pthread_main_np());
}
void I__pthread_equal(CPU *c) {
    RET(ARG(0) == ARG(1));
}
void I__pthread_setname_np(CPU *c) {
    RET(0);
}
void I__pthread_condattr_init(CPU *c) {
    RET(0);
}
void I__pthread_condattr_destroy(CPU *c) {
    RET(0);
}
void I__pthread_attr_setdetachstate(CPU *c) {
    RET(0);
}
void I__pthread_attr_setinheritsched(CPU *c) {
    RET(0);
}
void I__pthread_attr_setschedparam(CPU *c) {
    RET(0);
}
void I__pthread_attr_setschedpolicy(CPU *c) {
    RET(0);
}
void I__pthread_attr_getinheritsched(CPU *c) {
    if (ARG(1))
        wr32(ARG(1), 0);
    RET(0);
}
void I__pthread_attr_getschedparam(CPU *c) {
    if (ARG(1))
        wr32(ARG(1), 0);
    RET(0);
}
// ---- end additions ----

// ---------- OSAtomic* on guest memory, and Mach semaphores (port names are 32-bit, so they pass straight through) ----------
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <pthread.h>
static pthread_mutex_t atom64_lock = PTHREAD_MUTEX_INITIALIZER; // fallback for 64-bit values not 8-byte aligned
#define A64(slot) ((int64_t)((uint64_t)ARG(slot) | ((uint64_t)ARG((slot) + 1) << 32)))
static void ret64(CPU *c, int64_t v) {
    c->eax = (uint32_t)(uint64_t)v;
    c->edx = (uint32_t)((uint64_t)v >> 32);
}
void I__OSAtomicAdd32(CPU *c) {
    c->eax = (uint32_t)__atomic_add_fetch((int32_t *)G2H(ARG(1)), (int32_t)ARG(0), __ATOMIC_SEQ_CST);
}
void I__OSAtomicAdd32Barrier(CPU *c) {
    I__OSAtomicAdd32(c);
}
void I__OSAtomicAdd64(CPU *c) { // (int64_t amount, volatile int64_t *p) -> new value
    uint32_t p = ARG(2);
    int64_t amt = A64(0), r;
    if ((p & 7) == 0)
        r = __atomic_add_fetch((int64_t *)G2H(p), amt, __ATOMIC_SEQ_CST);
    else {
        pthread_mutex_lock(&atom64_lock);
        int64_t v;
        memcpy(&v, G2H(p), 8);
        r = v + amt;
        memcpy(G2H(p), &r, 8);
        pthread_mutex_unlock(&atom64_lock);
    }
    ret64(c, r);
}
void I__OSAtomicCompareAndSwap32Barrier(CPU *c) { // (old, new, volatile int32_t *p) -> bool
    int32_t old = (int32_t)ARG(0);
    c->eax = __atomic_compare_exchange_n((int32_t *)G2H(ARG(2)), &old, (int32_t)ARG(1), 0, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST);
}
void I__OSAtomicCompareAndSwap64Barrier(CPU *c) { // (int64 old, int64 new, volatile int64_t *p) -> bool
    uint32_t p = ARG(4);
    int64_t old = A64(0), nw = A64(2);
    int ok;
    if ((p & 7) == 0)
        ok = __atomic_compare_exchange_n((int64_t *)G2H(p), &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    else {
        pthread_mutex_lock(&atom64_lock);
        int64_t v;
        memcpy(&v, G2H(p), 8);
        ok = v == old;
        if (ok)
            memcpy(G2H(p), &nw, 8);
        pthread_mutex_unlock(&atom64_lock);
    }
    c->eax = (uint32_t)ok;
}
void I__semaphore_create(CPU *c) { // (task, semaphore_t *out, policy, value)
    semaphore_t s = 0;
    kern_return_t k = semaphore_create(mach_task_self(), &s, (int)ARG(2), (int)ARG(3));
    if (ARG(1))
        wr32(ARG(1), (uint32_t)s);
    c->eax = (uint32_t)k;
}
void I__semaphore_destroy(CPU *c) {
    c->eax = (uint32_t)semaphore_destroy(mach_task_self(), (semaphore_t)ARG(1));
}
void I__semaphore_signal(CPU *c) {
    c->eax = (uint32_t)semaphore_signal((semaphore_t)ARG(0));
}
void I__semaphore_wait(CPU *c) {
    c->eax = (uint32_t)semaphore_wait((semaphore_t)ARG(0));
}
// Scheduling policy limits: host values (macOS reports the same range the 2013 game expected).
void I__sched_get_priority_max(CPU *c) {
    c->eax = (uint32_t)sched_get_priority_max((int)ARG(0));
}
void I__sched_get_priority_min(CPU *c) {
    c->eax = (uint32_t)sched_get_priority_min((int)ARG(0));
}

// ---------- zlib: one-shot calls, plus streams with a host z_stream behind each guest (32-bit) z_stream ----------
// Guest z_stream layout (56 bytes): next_in 0, avail_in 4, total_in 8, next_out 12, avail_out 16, total_out 20, msg 24,
// state 28, zalloc 32, zfree 36, opaque 40, data_type 44, adler 48, reserved 52. Guest zalloc/zfree are ignored.
#include <zlib.h>
#define NZS 256
static struct {
    uint32_t g;
    z_stream h;
    int used, deflate;
} zs[NZS];
static pthread_mutex_t zs_lock = PTHREAD_MUTEX_INITIALIZER;
static int zs_find(uint32_t g) {
    for (int i = 0; i < NZS; i++)
        if (zs[i].used && zs[i].g == g)
            return i;
    return -1;
}
static int zs_new(uint32_t g, int def) {
    pthread_mutex_lock(&zs_lock);
    int i = zs_find(g);
    if (i < 0)
        for (i = 0; i < NZS && zs[i].used; i++)
            ;
    if (i == NZS) {
        pthread_mutex_unlock(&zs_lock);
        fprintf(stderr, "[zlib] too many streams\n");
        abort();
    }
    memset(&zs[i].h, 0, sizeof zs[i].h);
    zs[i].g = g;
    zs[i].used = 1;
    zs[i].deflate = def;
    pthread_mutex_unlock(&zs_lock);
    return i;
}
static void zs_in(int i) { // guest -> host buffer positions
    uint32_t g = zs[i].g;
    z_stream *h = &zs[i].h;
    h->next_in = rd32(g) ? (Bytef *)G2H(rd32(g)) : Z_NULL;
    h->avail_in = rd32(g + 4);
    h->next_out = rd32(g + 12) ? (Bytef *)G2H(rd32(g + 12)) : Z_NULL;
    h->avail_out = rd32(g + 16);
}
static void zs_out(int i, uint32_t in0, uint32_t ai0, uint32_t out0, uint32_t ao0) { // host -> guest progress
    uint32_t g = zs[i].g;
    z_stream *h = &zs[i].h;
    uint32_t used_in = ai0 - h->avail_in, used_out = ao0 - h->avail_out;
    wr32(g, in0 + used_in);
    wr32(g + 4, h->avail_in);
    wr32(g + 8, rd32(g + 8) + used_in);
    wr32(g + 12, out0 + used_out);
    wr32(g + 16, h->avail_out);
    wr32(g + 20, rd32(g + 20) + used_out);
    wr32(g + 24, 0);
    wr32(g + 44, (uint32_t)h->data_type);
    wr32(g + 48, (uint32_t)h->adler);
}
static int zs_get(CPU *c, uint32_t g) {
    int i = zs_find(g);
    if (i < 0) {
        fprintf(stderr, "[zlib] unknown stream 0x%x\n", g);
        abort();
    }
    return i;
}
void I__inflateInit_(CPU *c) { // (strm, version, stream_size)
    int i = zs_new(ARG(0), 0);
    wr32(ARG(0) + 8, 0);
    wr32(ARG(0) + 20, 0);
    zs_in(i);
    int r = inflateInit(&zs[i].h);
    wr32(ARG(0) + 28, 0x5A000000u | (uint32_t)i);
    c->eax = (uint32_t)r;
}
void I__inflateInit2_(CPU *c) { // (strm, windowBits, version, stream_size)
    int i = zs_new(ARG(0), 0);
    wr32(ARG(0) + 8, 0);
    wr32(ARG(0) + 20, 0);
    zs_in(i);
    int r = inflateInit2(&zs[i].h, (int)ARG(1));
    wr32(ARG(0) + 28, 0x5A000000u | (uint32_t)i);
    c->eax = (uint32_t)r;
}
void I__deflateInit2_(CPU *c) { // (strm, level, method, windowBits, memLevel, strategy, version, stream_size)
    int i = zs_new(ARG(0), 1);
    wr32(ARG(0) + 8, 0);
    wr32(ARG(0) + 20, 0);
    zs_in(i);
    int r = deflateInit2(&zs[i].h, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5));
    wr32(ARG(0) + 28, 0x5A000000u | (uint32_t)i);
    c->eax = (uint32_t)r;
}
void I__inflate(CPU *c) {
    uint32_t g = ARG(0);
    int i = zs_get(c, g);
    uint32_t in0 = rd32(g), ai = rd32(g + 4), out0 = rd32(g + 12), ao = rd32(g + 16);
    zs_in(i);
    int r = inflate(&zs[i].h, (int)ARG(1));
    zs_out(i, in0, ai, out0, ao);
    c->eax = (uint32_t)r;
}
void I__deflate(CPU *c) {
    uint32_t g = ARG(0);
    int i = zs_get(c, g);
    uint32_t in0 = rd32(g), ai = rd32(g + 4), out0 = rd32(g + 12), ao = rd32(g + 16);
    zs_in(i);
    int r = deflate(&zs[i].h, (int)ARG(1));
    zs_out(i, in0, ai, out0, ao);
    c->eax = (uint32_t)r;
}
void I__inflateReset(CPU *c) {
    int i = zs_get(c, ARG(0));
    int r = inflateReset(&zs[i].h);
    wr32(ARG(0) + 8, 0);
    wr32(ARG(0) + 20, 0);
    c->eax = (uint32_t)r;
}
static void zs_free(int i) {
    pthread_mutex_lock(&zs_lock);
    zs[i].used = 0;
    zs[i].g = 0;
    pthread_mutex_unlock(&zs_lock);
}
void I__inflateEnd(CPU *c) {
    int i = zs_find(ARG(0));
    if (i < 0) {
        c->eax = (uint32_t)Z_STREAM_ERROR;
        return;
    }
    c->eax = (uint32_t)inflateEnd(&zs[i].h);
    zs_free(i);
}
void I__deflateEnd(CPU *c) {
    int i = zs_find(ARG(0));
    if (i < 0) {
        c->eax = (uint32_t)Z_STREAM_ERROR;
        return;
    }
    c->eax = (uint32_t)deflateEnd(&zs[i].h);
    zs_free(i);
}
void I__uncompress(CPU *c) { // (dest, uLongf *destLen (32-bit), source, sourceLen)
    uLongf n = rd32(ARG(1));
    int r = uncompress((Bytef *)G2H(ARG(0)), &n, (const Bytef *)G2H(ARG(2)), (uLong)ARG(3));
    wr32(ARG(1), (uint32_t)n);
    c->eax = (uint32_t)r;
}
void I__compress2(CPU *c) { // (dest, uLongf *destLen, source, sourceLen, level)
    uLongf n = rd32(ARG(1));
    int r = compress2((Bytef *)G2H(ARG(0)), &n, (const Bytef *)G2H(ARG(2)), (uLong)ARG(3), (int)ARG(4));
    wr32(ARG(1), (uint32_t)n);
    c->eax = (uint32_t)r;
}

// Debug: on SIGTERM / SIGUSR1, show where every guest thread is (current function + ebp call chain). Racy by nature.
CPU *rt_main_cpu;
static void dump_one(const char *tag, CPU *c) {
    if (!c)
        return;
    fprintf(stderr, "[threads] %s: in %s (0x%x) esp=%x\n", tag, rt_func_name(c->fn), c->fn, c->esp);
    for (uint32_t f = c->ebp, k = 0; f > 0x1000 && f < 0xF0000000u && k < 14; f = rd32(f), k++)
        fprintf(stderr, "            %s\n", rt_func_name(rd32(f + 4)));
}
void rt_dump_threads(void) {
    dump_one("main", rt_main_cpu);
    for (int i = 1; i < nthreads && i < 256; i++) {
        char t[64];
        snprintf(t, sizeof t, "thread %d (start %s)", i + 1, rt_func_name(threads[i].start));
        dump_one(t, threads[i].cpu);
    }
}

// ---- RT32_PROF=<seconds>: built-in sampling profiler ----
// A host thread reads every guest thread's current function (CPU.fn, set at each guest function entry) 1000x a
// second and prints, every <seconds>, the busiest threads with their top functions. A sample is counted as
// "[host GL]" when that thread is inside a generated GL wrapper. Note: CPU.fn is the most recently ENTERED
// function, so time after a callee returns is charged to that callee (approximate self time).
#include <mach/mach_time.h>
extern volatile CPU *gl_in_cpu;
extern volatile uint64_t gl_ticks_total;
#define PROF_SLOTS 8192
static int
    prof_frames; // RT32_PROF_FRAMES=1: attribute by stack frame (return address) instead of last-entered function
typedef struct {
    uint32_t fn[PROF_SLOTS];
    uint32_t n[PROF_SLOTS];
    uint32_t total, gl;
} ProfTab;
static ProfTab *prof[257];
static void prof_add(ProfTab *t, uint32_t fn) {
    uint32_t h = (fn * 2654435761u) & (PROF_SLOTS - 1);
    for (int k = 0; k < 64; k++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (t->fn[h] == fn) {
            t->n[h]++;
            return;
        }
        if (!t->fn[h]) {
            t->fn[h] = fn;
            t->n[h] = 1;
            return;
        }
    }
}
static void prof_report(int secs) {
    static uint64_t gl_last;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t gl_now_t = gl_ticks_total;
    double gl_ms = (double)(gl_now_t - gl_last) * tb.numer / tb.denom / 1e6;
    gl_last = gl_now_t;
    fprintf(stderr, "[prof] ---- last %d s: %.0f ms inside host GL calls (%.1f%% of one core) ----\n", secs, gl_ms,
            gl_ms / (secs * 10.0));
    for (int i = 0; i < 257; i++) {
        ProfTab *t = prof[i];
        if (!t || !t->total)
            continue;
        // skip threads that spent > 97% in a single function (idle waiters)
        uint32_t best = 0;
        for (int k = 0; k < PROF_SLOTS; k++)
            if (t->n[k] > best)
                best = t->n[k];
        if (best * 100 > t->total * 97 && !t->gl) {
            memset(t, 0, sizeof *t);
            continue;
        }
        fprintf(stderr, "[prof] thread %d (%s): %u samples, %.1f%% in host GL\n", i + 1,
                i == 0 ? "main" : rt_func_name(threads[i].start), t->total, 100.0 * t->gl / t->total);
        for (int r = 0; r < 15; r++) {
            int bi = -1;
            uint32_t bn = 0;
            for (int k = 0; k < PROF_SLOTS; k++)
                if (t->n[k] > bn) {
                    bn = t->n[k];
                    bi = k;
                }
            if (bi < 0 || bn * 200 < t->total)
                break;
            fprintf(stderr, "[prof]     %5.1f%%  %s (0x%x)\n", 100.0 * bn / t->total,
                    t->fn[bi] == 0xFFFFFFFEu ? "[host GL]"
                    : prof_frames            ? "frame ret"
                                             : rt_func_name(t->fn[bi]),
                    t->fn[bi]);
            t->n[bi] = 0;
        }
        memset(t, 0, sizeof *t);
    }
}
static void *prof_thread(void *arg) {
    int secs = (int)(intptr_t)arg;
    uint64_t ticks = 0;
    for (;;) {
        usleep(1000);
        for (int i = 0; i < nthreads && i < 256; i++) {
            CPU *c = i == 0 ? rt_main_cpu : threads[i].cpu;
            if (!c)
                continue;
            if (!prof[i])
                prof[i] = calloc(1, sizeof(ProfTab));
            ProfTab *t = prof[i];
            t->total++;
            if (gl_in_cpu == c) {
                t->gl++;
                prof_add(t, 0xFFFFFFFEu);
            } else if (
                prof_frames) { // key = return address of the current stack frame: the call site whose callee is running
                uint32_t e = c->ebp;
                prof_add(t, (e > 0x1000 && e < 0xF0000000u) ? (rd32(e + 4) | 0) : 0xFFFFFFFDu);
            } else
                prof_add(t, c->fn);
        }
        if (++ticks % (uint64_t)(secs * 1000) == 0)
            prof_report(secs);
    }
    return NULL;
}
void rt_prof_start(void) {
    const char *s = getenv("RT32_PROF");
    if (!s)
        return;
    int secs = atoi(s);
    if (secs <= 0)
        secs = 10;
    prof_frames = getenv("RT32_PROF_FRAMES") != NULL;
    pthread_t th;
    pthread_create(&th, NULL, prof_thread, (void *)(intptr_t)secs);
    pthread_detach(th);
    fprintf(stderr, "[prof] sampling profiler on, report every %d s\n", secs);
}

// Which guest thread a CPU belongs to (for fatal dumps)
void rt_thread_desc(CPU *c, char *buf, size_t n) {
    if (c == rt_main_cpu) {
        snprintf(buf, n, "main thread");
        return;
    }
    for (int i = 1; i < nthreads && i < 256; i++)
        if (threads[i].cpu == c) {
            snprintf(buf, n, "guest thread %d (start %s)", i + 1, rt_func_name(threads[i].start));
            return;
        }
    snprintf(buf, n, "unregistered CPU %p (callback/host-created?)", (void *)c);
}

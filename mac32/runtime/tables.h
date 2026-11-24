#pragma once
#include <stdint.h>
#include "rt.h"
typedef struct {
    uint32_t addr;
    gfn fn;
    const char *name;
} GFunc;
typedef struct {
    uint32_t addr;
    gfn fn;
    const char *name;
} GStub;
typedef struct {
    uint32_t addr;
    const char *name;
} GPtr;
typedef struct {
    const char *name;
    uint32_t va, vs, fo, fs;
} GSeg;
typedef struct {
    const char *name;
    gfn fn;
} GImport; // fn == 0: data symbol
extern const GImport gimports[];
extern const int ngimports;
extern const GFunc gfuncs[];
extern const int ngfuncs;
extern const GStub gstubs[];
extern const int ngstubs;
extern const GPtr gptrs[];
extern const int ngptrs;
extern const GSeg gsegs[];
extern const int ngsegs;
extern const uint32_t gmodinit[];
extern const int ngmodinit;
extern const GPtr gextrel[];
extern const int ngextrel;
typedef struct {
    const char *name;
    uint32_t addr;
} GDef; // data symbol defined in the game binary
extern const GDef gdefs[];
extern const int ngdefs;

void unimpl_import(CPU *c, const char *name);
CPU *rt_init(const char *binpath);
uint32_t guest_call(CPU *c, uint32_t addr, int nargs, const uint32_t *args);
CPU *cpu_new(uint32_t stack_size);
void cpu_cpuid(CPU *c);
uint32_t guest_func(const char *symbol);
uint32_t gmalloc(uint32_t n);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t n);
uint32_t gstrdup(const char *s);
uint32_t rt_host_fn(gfn f); // native function -> guest-callable address (cdecl, ret popped by caller of call_ind)
uint32_t rt_host_fn_ctx(void (*f)(CPU *, uint32_t), uint32_t ctx);
uint32_t rt_import_addr(const char *name); // stub address for functions, placeholder storage for data
// C++ exception tables (from __eh_frame / __gcc_except_tab)
typedef struct {
    uint32_t lo, hi;
} GFde;
typedef struct {
    uint32_t lo, hi, lp, fn;
    int32_t act, nact;
} GEHSite; // nact 0: cleanup only
typedef struct {
    int32_t filter;
    uint32_t ti, indirect;
} GEHAct; // ti 0 with filter > 0: catch (...)
extern const GFde gfdes[];
extern const int ngfdes;
extern const GEHSite gehsites[];
extern const int ngehsites;
extern const GEHAct gehacts[];
extern const int ngehacts;

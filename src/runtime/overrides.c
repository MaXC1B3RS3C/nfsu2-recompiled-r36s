#include <pthread.h>
/*
 * overrides.c - guest functions replaced by hand-written C.
 *
 * Every entry needs a reason that is a fact about the hardware or the host,
 * not "it crashed": an override hides whatever the original did.
 *
 * Calling convention inside the lifted model: the caller has pushed a return
 * slot, arguments are at g_esp+4.., the body leaves its result in g_eax and
 * pops what `ret N` would (g_esp += 4 + N).
 */
#include <string.h>
#include "recomp_types.h"
#include "nfs_runtime.h"
#include "nfs_exe_addr.h"   /* v10 addresses -> the lifted exe's (config/exe_functions.txt) */

/* VA of the indirect call in flight. The guest CPU state is one struct (g_cpu) when a build defines
 * RECOMP_TLS_STRUCT, otherwise separate thread-local variables; guest_runtime.cpp defines both. */
#ifdef RECOMP_TLS_STRUCT
#define ov_icall_va (g_cpu.icall_va)
#else
extern RECOMP_TLS uint32_t nfs_icall_va;
#define ov_icall_va (nfs_icall_va)
#endif

/* 0x006F5D25 (v12 0x006F5FC5): Cyrix detection.
 *   xor eax,eax; sahf; mov eax,5; mov ebx,2; div bl; lahf; cmp ah,2; ...
 * Cyrix parts leave the flags untouched across DIV, so AH reads back 2 (the
 * value SAHF loaded, plus the always-set bit 1); Intel/AMD do not. DIV's flags
 * are architecturally undefined and the lifter does not model them, so the
 * lifted probe always answers "Cyrix". The host is never a Cyrix: return 0. */
static void ov_is_cyrix(void) {
    g_eax = 0;
    g_esp += 4;
}

#if defined(NFS_R36S) || defined(NFS_FX_PRESHADER_ONLY)
/* Diagnostics (NFSU2_FX_STATS=1): D3DX effect BeginPass (vtable method 0x0066A9F6, args effect, pass):
 * calls per frame, distinct effects and how often the same effect+pass is applied twice in a row. */
#include <stdio.h>
#include <stdlib.h>
extern void sub_0066A9F6(void);
extern void nfs_fx_param_set(uint32_t handle);
extern void nfs_fx_global_dirty(void);
extern int nfs_fx_cache_mode(void);
extern uint32_t getD3D9PresentCount(void);
static __attribute__((unused)) void ov_fx_begin_pass(void) {  /* R36S only (NFSU2_FX_STATS) */
    static uint32_t frame, calls, repeats, lastFx, lastPass, frames;
    static uint32_t fx[64]; static uint32_t nfx;
    const uint32_t effect = *(const uint32_t*)ADDR(g_esp + 4), pass = *(const uint32_t*)ADDR(g_esp + 8);
    const uint32_t now = getD3D9PresentCount();
    { static uint32_t dumped[16]; static unsigned nd; unsigned k; const uint32_t vt = *(const uint32_t*)ADDR(effect);
      for (k = 0; k < nd && dumped[k] != vt; ++k) {}
      if (k == nd && nd < 16) { dumped[nd++] = vt; printf("[NFSU2 FX] effect %08x vtable %08x:", effect, vt);
        for (k = 0; k < 80; ++k) printf(" %u=%08x", k, *(const uint32_t*)ADDR(vt + 4 * k)); printf("\n"); } }
    if (now != frame) {
        if (++frames % 60 == 0 && calls)
            printf("[NFSU2 FX] BeginPass %u/frame, same effect+pass as previous %u/frame, %u distinct effects\n", calls / 60, repeats / 60, nfx);
        if (frames % 60 == 0) { calls = repeats = 0; nfx = 0; }
        frame = now;
    }
    ++calls;
    const int wasRepeat = effect == lastFx && pass == lastPass;
    if (wasRepeat) ++repeats;
    lastFx = effect; lastPass = pass;
    unsigned i; for (i = 0; i < nfx && fx[i] != effect; ++i) {}
    if (i == nfx && nfx < 64) fx[nfx++] = effect;
    static int skip = -1; if (skip < 0) { const char* e = getenv("NFSU2_FX_SKIP"); skip = e && e[0] == '1'; }
    if (skip && calls > 1 && effect == lastFx && pass == lastPass && wasRepeat) { g_eax = 0; g_esp += 12; return; }
    sub_0066A9F6();
}

/* Experiment: which effect methods 20..58 the game calls per frame, with which handles. */
static const struct { int idx; uint32_t va; } k_fxMethods[] = {
    {20, NFS_VA(0x0066C619)},
    {21, NFS_VA(0x0066C685)},
    {22, NFS_VA(0x0066DE12)},
    {23, NFS_VA(0x0066DF0C)},
    {24, NFS_VA(0x0066DF9E)},
    {25, NFS_VA(0x0066E2DE)},
    {26, NFS_VA(0x0066DA93)},
    {27, NFS_VA(0x0066DC20)},
    {28, NFS_VA(0x0066E5BD)},
    {29, NFS_VA(0x0066E912)},
    {30, NFS_VA(0x0066F215)},
    {31, NFS_VA(0x0066F33F)},
    {32, NFS_VA(0x0066EBDC)},
    {33, NFS_VA(0x0066EF72)},
    {34, NFS_VA(0x0066D017)},
    {35, NFS_VA(0x0066D277)},
    {36, NFS_VA(0x0066C74C)},
    {37, NFS_VA(0x0066C90F)},
    {38, NFS_VA(0x0066CE4E)},
    {39, NFS_VA(0x0066CEE6)},
    {40, NFS_VA(0x0066CA52)},
    {41, NFS_VA(0x0066CAF2)},
    {42, NFS_VA(0x0066CB51)},
    {43, NFS_VA(0x0066CBF1)},
    {44, NFS_VA(0x0066CF2E)},
    {45, NFS_VA(0x0066CFCF)},
    {46, NFS_VA(0x0066CC50)},
    {47, NFS_VA(0x0066CCF0)},
    {48, NFS_VA(0x0066CD4F)},
    {49, NFS_VA(0x0066CDEF)},
    {50, NFS_VA(0x0066D3CA)},
    {51, NFS_VA(0x0066D465)},
    {52, NFS_VA(0x0066D4FD)},
    {53, NFS_VA(0x0066D5D1)},
    {54, NFS_VA(0x0066D673)},
    {55, NFS_VA(0x0066D7C8)},
    {56, NFS_VA(0x0066D883)},
    {57, NFS_VA(0x0066D9D8)},
    {58, NFS_VA(0x0066C6E1)},
    {59, NFS_VA(0x0065C819)},
    {60, NFS_VA(0x00661BA0)},
    {61, NFS_VA(0x0065C015)},
    {62, NFS_VA(0x00660462)},
    {63, NFS_VA(0x00660319)},
    {64, NFS_VA(0x0066BD01)},
    {70, NFS_VA(0x0065C848)},
    {71, NFS_VA(0x0065C5AE)},
    {72, NFS_VA(0x0065E257)},
    {73, NFS_VA(0x00662075)},
    {74, NFS_VA(0x0066208C)},
    {75, NFS_VA(0x00665DD6)},
    {76, NFS_VA(0x0065E571)},
    {77, NFS_VA(0x0065E6D0)},
    {78, NFS_VA(0x0067036F)},
    {79, NFS_VA(0x00670B57)}
};
static inline __attribute__((unused)) void recomp_call_va(uint32_t va) { recomp_func_t f = recomp_lookup(va); if (f) f(); }
static uint32_t fxMethodCalls[80], fxFrame, fxFrames;
static void ov_fx_method(void) {
    /* Hot (~4k calls per frame): method index and lifted target from a small direct-mapped cache. */
    enum { NC = 256 };
    static uint32_t cVa[NC]; static int cIdx[NC]; static recomp_func_t cFn[NC];
    const uint32_t va = ov_icall_va;
    const unsigned h = ((va >> 1) ^ (va >> 9)) & (NC - 1);
    if (cVa[h] != va) {
        unsigned i;
        for (i = 0; i < sizeof k_fxMethods / sizeof k_fxMethods[0] && k_fxMethods[i].va != va; ++i) {}
        cIdx[h] = i < sizeof k_fxMethods / sizeof k_fxMethods[0] ? k_fxMethods[i].idx : -1;
        cFn[h] = recomp_lookup(va);
        cVa[h] = va;
    }
    const int idx = cIdx[h];
    if (idx >= 0) {
        ++fxMethodCalls[idx];
        if (!(idx & 1) && idx <= 56) nfs_fx_param_set(*(const uint32_t*)ADDR(g_esp + 8));  /* Set*(this, handle, ...) */
        else if (idx >= 56) nfs_fx_global_dirty();
    }
    static int stats = -1; if (stats < 0) { const char* e = getenv("NFSU2_FX_STATS"); stats = e && e[0] == '1'; }
    if (stats) {
        const uint32_t now = getD3D9PresentCount();
        if (now != fxFrame) {
            fxFrame = now;
            if (++fxFrames % 60 == 0) { printf("[NFSU2 FXM] effect method calls/frame:"); for (int k = 0; k < 80; ++k) if (fxMethodCalls[k]) printf(" m%d=%u", k, fxMethodCalls[k] / 60); printf("\n"); for (int k = 0; k < 80; ++k) fxMethodCalls[k] = 0; }
        }
    }
    if (cFn[h]) cFn[h]();
}
#endif /* NFS_R36S || NFS_FX_PRESHADER_ONLY */

/* 0x0075CCF0 (v12 0x0075CFD0): the static CRT memcpy/memmove(dst, src, n) (cdecl, returns dst).
 * It handles overlap by copying backward, and reaches its unrolled dword moves
 * through computed jumps into the middle of straight-line blocks (tables at
 * v12 0x75D11C, 0x75D268, 0x75D2B8...). Not every target is a lifted entry, so an
 * overlapping move with 8+ dwords stopped at "ITAIL unresolved 0x0075D27C" (v12).
 * Its contract is exactly the host memmove over guest memory. */
static void ov_crt_memmove(void) {
    uint32_t dst = MEM32(g_esp + 4), src = MEM32(g_esp + 8), n = MEM32(g_esp + 12);
    if (n) memmove((void *)ADDR(dst), (const void *)ADDR(src), n);
    g_eax = dst;
    g_esp += 4;
}

static const struct { uint32_t va; nfs_override_fn fn; } k_overrides[] = {
    { NFS_VA(0x006F5D25), ov_is_cyrix },
#if defined(NFS_R36S)
    { NFS_VA(0x0066A9F6), ov_fx_begin_pass },
#endif
    { NFS_VA(0x0075CCF0), ov_crt_memmove },
};

/* Called on every indirect call that misses its call-site cache (polymorphic vtable sites), and for every
 * effect-method call (overrides are never site-cached): a hash of the ~55 overridden VAs instead of a linear
 * scan of the method table for any target in the D3DX range. Built once; read-only afterwards (the sound
 * threads call this too). */
#if defined(NFS_R36S)
extern int nfs_fx_lazy_enabled(void);
extern void nfs_fx_lazy_begin(void); extern void nfs_fx_lazy_begin_pass(void); extern void nfs_fx_lazy_commit(void);
extern void nfs_fx_lazy_end_pass(void); extern void nfs_fx_lazy_end(void);
#endif
enum { OV_SLOTS = 256 };
static uint32_t ovKey[OV_SLOTS]; static nfs_override_fn ovFn[OV_SLOTS];
static volatile int ovReady;
static unsigned ov_slot(uint32_t va) { return (va * 0x9e3779b1u) >> 24; }
static void ov_put(uint32_t va, nfs_override_fn fn) {
    unsigned h = ov_slot(va);
    while (ovKey[h] && ovKey[h] != va) h = (h + 1) & (OV_SLOTS - 1);
    if (!ovKey[h]) { ovKey[h] = va; ovFn[h] = fn; }  /* first entry wins, like the scans did */
}
static void ov_build(void) {
#if defined(NFS_R36S)
    const char* e = getenv("NFSU2_FX_STATS");
    const int stats = e && e[0] == '1';
    if (stats || nfs_fx_cache_mode() != 0)
        for (unsigned i = 0; i < sizeof k_fxMethods / sizeof k_fxMethods[0]; ++i)
            if (k_fxMethods[i].va != NFS_VA(0x0065E26C)) ov_put(k_fxMethods[i].va, ov_fx_method);
    for (unsigned i = 0; i < sizeof k_overrides / sizeof k_overrides[0]; i++)
        if (k_overrides[i].va != NFS_VA(0x0066A9F6) || stats) ov_put(k_overrides[i].va, k_overrides[i].fn);
    /* lazy effect passes (hud_half.c): Begin, BeginPass, CommitChanges, EndPass, End of the D3DX effect */
    if (!stats && nfs_fx_lazy_enabled()) {
        ov_put(NFS_VA(0x006644A6), nfs_fx_lazy_begin); ov_put(NFS_VA(0x0066A9F6), nfs_fx_lazy_begin_pass);
        ov_put(NFS_VA(0x0066A93B), nfs_fx_lazy_commit); ov_put(NFS_VA(0x0065AAEC), nfs_fx_lazy_end_pass); ov_put(NFS_VA(0x0065AA06), nfs_fx_lazy_end);
    }
#else
    /* Android: the effect Set* hooks feed the preshader cache (fx_cache.c, NFS_FX_PRESHADER_ONLY). */
#if defined(NFS_FX_PRESHADER_ONLY)
    if (nfs_fx_cache_mode() != 0)
        for (unsigned i = 0; i < sizeof k_fxMethods / sizeof k_fxMethods[0]; ++i)
            if (k_fxMethods[i].va != NFS_VA(0x0065E26C)) ov_put(k_fxMethods[i].va, ov_fx_method);
#endif
    for (unsigned i = 0; i < sizeof k_overrides / sizeof k_overrides[0]; i++)
        ov_put(k_overrides[i].va, k_overrides[i].fn);
#endif
    __atomic_store_n(&ovReady, 1, __ATOMIC_RELEASE);
}
nfs_override_fn nfs_override_lookup(uint32_t va) {
    if (__builtin_expect(!__atomic_load_n(&ovReady, __ATOMIC_ACQUIRE), 0)) {
        static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); if (!ovReady) ov_build(); pthread_mutex_unlock(&m);
    }
    if (!va) return 0;
    for (unsigned h = ov_slot(va);; h = (h + 1) & (OV_SLOTS - 1)) {
        const uint32_t k = __atomic_load_n(&ovKey[h], __ATOMIC_ACQUIRE);
        if (!k) break;
        if (k == va) return ovFn[h];
    }
    return 0;
}

int nfs_override_count(void) { return (int)(sizeof k_overrides / sizeof k_overrides[0]); }

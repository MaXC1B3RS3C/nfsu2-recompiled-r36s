/*
 * fx_cache.c - skip D3DX effect preshader evaluations whose inputs did not change (R36S; Android: the
 * preshader part only, NFS_FX_PRESHADER_ONLY).
 *
 * The game applies a D3DX effect pass (ID3DXEffect::BeginPass, vtable 66 = 0x0066A9F6) once per drawn
 * object, ~900 times per race frame, and 97% of the time it is the same pass as before. Each application
 * evaluates every "always dirty" state of the pass: ~7.8k preshader evaluations per frame
 * (sub_00664743 -> sub_00663756 gathers the inputs, sub_00660766 runs the expression VM), about a third of
 * the game thread on the R36S, and 98.8% of them produce the value they produced last time.
 *
 * sub_00664743(this = effect in ecx, pass, state) evaluates one state assignment. Layout (D3DX9 as
 * linked into SPEED2.EXE): pass = [effect+0x2c] + pass*0x64; state = [pass+4] + state*0x64.
 * Preshader states have ([state+0x30] & 4) == 0 and a preshader object at [state+0x38]; their inputs
 * are bindings at [state+0x34] ([state+0x4e] entries of 32 bytes), each naming a parameter struct at
 * [entry+0xc] whose data pointer is [param+0]. The result is written to [state+0x48] + [pass+0].
 *
 * A parameter only changes through the effect's Set* methods (vtable 20..56, even indices; hooked in
 * overrides.c -> nfs_fx_param_set with the handle, a pointer handle being ~param). Every set bumps an
 * epoch for that parameter's data pointer (shared pool parameters share it). A state is skipped when all
 * its inputs are older than its last evaluation and its output still holds the value that evaluation
 * wrote. Verified in the VM: 0 mismatches over boot, menus, loading and a race with NFSU2_FX_CACHE=1
 * (evaluate everything, count would-be-wrong skips). NFSU2_FX_CACHE=0 turns it off.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void sub_00664743_orig(void);
extern uint32_t getD3D9PresentCount(void);

#define PSLOTS 16384
static uint32_t pKey[PSLOTS], pEpoch[PSLOTS];
static uint32_t counter = 1, globalEpoch = 1;
static int validGuest(uint32_t va) { return va >= 0x400000u && va < 0x30000000u; }
static uint32_t paramEpoch(uint32_t key) {
    uint32_t i = (key * 2654435761u) >> 18;
    for (unsigned n = 0; n < PSLOTS; ++n, i = (i + 1) & (PSLOTS - 1)) {
        if (pKey[i] == key) return pEpoch[i];
        if (!pKey[i]) return 0;
    }
    return 0xffffffffu;
}
void nfs_fx_global_dirty(void) { globalEpoch = ++counter; }
void nfs_fx_param_set(uint32_t handle) {
    ++counter;
    if ((int32_t)handle >= 0) { globalEpoch = counter; return; }  /* name handles: everything dirty */
    const uint32_t param = ~handle;                                /* pointer handles are stored inverted */
    if (!validGuest(param)) { globalEpoch = counter; return; }
    const uint32_t key = *(const uint32_t*)ADDR(param);            /* the parameter's data pointer */
    uint32_t i = (key * 2654435761u) >> 18;
    for (unsigned n = 0; n < PSLOTS; ++n, i = (i + 1) & (PSLOTS - 1)) {
        if (pKey[i] == key || !pKey[i]) { pKey[i] = key; pEpoch[i] = counter; return; }
    }
    globalEpoch = counter;  /* table full: everything dirty */
}
int nfs_fx_cache_mode(void) {
    static int mode = -1;  /* 0 off, 1 verify (evaluate always, count wrong predictions), 2 skip */
    if (mode < 0) { const char* e = getenv("NFSU2_FX_CACHE"); mode = e ? atoi(e) : 2; }
    return mode;
}

#define SSLOTS 16384
static uint32_t sKey[SSLOTS], sEval[SSLOTS], sOut[SSLOTS][4];
static uint32_t frame, frames, evals, predicted, mismatches, skipped;
void sub_00664743(void) {
    const int mode = nfs_fx_cache_mode();
    if (!mode) { sub_00664743_orig(); return; }
    const uint32_t effect = g_ecx;
    const uint32_t pass = *(const uint32_t*)ADDR(g_esp + 4), state = *(const uint32_t*)ADDR(g_esp + 8);
    const uint32_t passes = *(const uint32_t*)ADDR(effect + 0x2c);
    if (!validGuest(passes)) { sub_00664743_orig(); return; }
    const uint32_t passPtr = passes + (pass & 0xffff) * 0x64;
    const uint32_t st = *(const uint32_t*)ADDR(passPtr + 4) + (state & 0xffff) * 0x64;
    if (!validGuest(st)) { sub_00664743_orig(); return; }
    const uint32_t flags = *(const uint32_t*)ADDR(st + 0x30), pre = *(const uint32_t*)ADDR(st + 0x38);
    if ((flags & 4) || !pre) { sub_00664743_orig(); return; }
    const uint32_t out = *(const uint32_t*)ADDR(st + 0x48) + *(const uint32_t*)ADDR(passPtr);
    const uint32_t nb = *(const uint16_t*)ADDR(st + 0x4e), bind = *(const uint32_t*)ADDR(st + 0x34);
    if (!validGuest(out)) { sub_00664743_orig(); return; }
    const uint32_t h = (st >> 2) & (SSLOTS - 1);
    int unchanged = sKey[h] == st && sEval[h] >= globalEpoch && nb <= 32 && (nb == 0 || validGuest(bind));
    for (uint32_t i = 0; unchanged && i < nb; ++i) {
        const uint32_t param = *(const uint32_t*)ADDR(bind + i * 32 + 0xc);
        if (!validGuest(param) || paramEpoch(*(const uint32_t*)ADDR(param)) > sEval[h]) unchanged = 0;
    }
    const int sameOut = unchanged && !memcmp((const void*)ADDR(out), sOut[h], 16);
    if (mode == 1) {
        const uint32_t now = getD3D9PresentCount();
        if (now != frame) {
            frame = now;
            if (++frames % 120 == 0 && evals) {
                printf("[NFSU2 FX] preshader states/frame %u, skippable %u, wrong predictions %u\n", evals / 120, predicted / 120, mismatches);
                evals = predicted = mismatches = 0;
            }
        }
        ++evals;
        if (sameOut) ++predicted;
    } else if (sameOut) { ++skipped; g_eax = 0; g_esp += 12; return; }  /* ret 8 */
    sub_00664743_orig();
    uint32_t v[4];
    memcpy(v, (const void*)ADDR(out), 16);
    if (sameOut && memcmp(v, sOut[h], 16)) ++mismatches;
    sKey[h] = st; sEval[h] = counter; memcpy(sOut[h], v, 16);
}

/* NFS_FX_PRESHADER_ONLY (Android): only the preshader cache above. The rest of this file replays device
 * calls through the R36S render thread's queue (d3d9_render_thread.inc), which the Android backends lack. */
#if !defined(NFS_FX_PRESHADER_ONLY)
/*
 * ---- Effect state application memo (sub_00668FAA) ----
 *
 * sub_00668FAA(this = effect in ecx, pass, state, index) applies one state assignment of a pass to the
 * device: when the state is dirty ([st+0] != 0) it re-evaluates it first ([st+0] = [st+4], preshader via
 * sub_00664743), then always issues the matching device call from the stored value. The state type is
 * [st+0x58] >> 24 (jump table at 0x669F0B): 1 SetRenderState, 2 SetTextureStageState, 3 SetSamplerState,
 * 9 SetTexture (from a parameter), 14 shader constants, 7/8 material/light, ... Every BeginPass re-applies
 * every state of the pass: ~25k device calls per race frame, 90% of them redundant.
 *
 * For types 1-3 (~16k applications per frame) the single device call one application made is recorded
 * (d3dDispatchFromCallVa reports it here while a capture is open) and replayed natively while the state's
 * inputs are unchanged: same state and index, no global effect change since, and no preshader input set
 * since. VM verify (NFSU2_FX_STATE=1): 0 wrong predictions. Other types (parameter-sourced textures and
 * constants) mispredict with these rules and always run the lifted code.
 * NFSU2_FX_STATE: 0 off, 1 verify, 2 replay (default). The memo is 32 bytes per entry (cache footprint
 * matters on the A35: a first version with 700-byte entries cost more than it saved).
 */
extern void sub_00668FAA_orig(void);
extern int nfsD3dReplay(uint32_t token, const uint32_t* args);  /* guest_runtime.cpp: deferred dispatch */
extern int nfsD3dDeviceCall(uint32_t method, const uint32_t* args, uint32_t* result);  /* d3d9_render_thread.inc */
int nfs_fx_cap_depth;
typedef struct { int n, bad; uint32_t token, count, a[4]; } FxCap;
static FxCap cap;  /* only the innermost replayable application captures */
/* Called for every D3D9 COM call while a capture is open (deferred = it was queued or filtered). */
typedef struct { uint32_t method, a[4]; } DevRec;
static DevRec bigList[1024];
static int bigN, bigBad, bigActive;
void nfs_fx_capture(uint32_t token, const uint32_t* args, uint32_t count, int deferred) {
    if (bigActive) {
        const uint32_t m = ((token - 0x7e000000u) % 4096) / 16, kind = (token - 0x7e000000u) / 4096;
        if (!deferred || !args || kind != 2 || bigN >= 1024) { bigBad = 1; return; }
        DevRec* r = &bigList[bigN++]; r->method = m;
        for (uint32_t i = 0; i < 4; ++i) r->a[i] = i < count ? args[i] : 0;
        return;
    }
    if (cap.bad) return;
    const uint32_t m = ((token - 0x7e000000u) % 4096) / 16, kind = (token - 0x7e000000u) / 4096;
    if (!deferred || !args || cap.n || count > 4 || kind != 2 || (m != 57 && m != 67 && m != 69)) { cap.bad = 1; return; }
    cap.token = token; cap.count = count;
    for (uint32_t i = 0; i < 4; ++i) cap.a[i] = i < count ? args[i] : 0;
    cap.n = 1;
}

#define MSLOTS 8192
typedef struct { uint32_t st, idx, eval, token, a[4]; } FxMemo;  /* 32 bytes */
static FxMemo memo[MSLOTS];
static uint32_t vFrame, vFrames;
static uint32_t vCalls[32], vBad[32], vPred[32], vWrong[32], vReplay[32];
static int g_fxStateMode = -1;
static inline int fxStateMode(void) {
    if (__builtin_expect(g_fxStateMode < 0, 0)) { const char* e = getenv("NFSU2_FX_STATE"); g_fxStateMode = e ? atoi(e) : 2; }
    return g_fxStateMode;
}
static void fxStateStats(void) {
    if (g_fxStateMode != 1) return;
    const uint32_t now = getD3D9PresentCount();
    if (now == vFrame) return;
    vFrame = now;
    if (++vFrames % 240) return;
    printf("[NFSU2 FXS] per frame by state type (lifted uncacheable predicted WRONG(total) replayed):");
    for (int t = 0; t < 32; ++t) if (vCalls[t] || vReplay[t])
        printf(" t%d=%u/%u/%u/%u/%u", t, vCalls[t] / 240, vBad[t] / 240, vPred[t] / 240, vWrong[t], vReplay[t] / 240);
    printf("\n");
    memset(vCalls, 0, sizeof vCalls); memset(vBad, 0, sizeof vBad); memset(vPred, 0, sizeof vPred); memset(vReplay, 0, sizeof vReplay);
}
void sub_00668FAA(void) {
    const int mode = fxStateMode();
    if (!mode) { sub_00668FAA_orig(); return; }
    const uint32_t effect = g_ecx;
    const uint32_t passes = validGuest(effect) ? *(const uint32_t*)ADDR(effect + 0x2c) : 0;
    if (nfs_fx_cap_depth) cap.bad = 1;  /* nested application: the outer one is not replayable */
    if (!validGuest(passes)) { sub_00668FAA_orig(); return; }
    const uint32_t pass = *(const uint32_t*)ADDR(g_esp + 4) & 0xffff, state = *(const uint32_t*)ADDR(g_esp + 8) & 0xffff;
    const uint32_t idx = *(const uint32_t*)ADDR(g_esp + 12);
    const uint32_t passPtr = passes + pass * 0x64;
    const uint32_t st = *(const uint32_t*)ADDR(passPtr + 4) + state * 0x64;
    if (!validGuest(st)) { sub_00668FAA_orig(); return; }
    const uint32_t type = (*(const uint32_t*)ADDR(st + 0x58) >> 24) & 31;
    if (type < 1 || type > 3) { ++vCalls[type]; sub_00668FAA_orig(); fxStateStats(); return; }
    FxMemo* m = &memo[((st >> 2) ^ (idx * 0x9e37u)) & (MSLOTS - 1)];
    int pred = m->st == st && m->idx == idx && m->eval >= globalEpoch;
    if (pred) {
        const uint32_t flags = *(const uint32_t*)ADDR(st + 0x30), pre = *(const uint32_t*)ADDR(st + 0x38);
        if (!(flags & 4) && pre) {
            const uint32_t nb = *(const uint16_t*)ADDR(st + 0x4e), bind = *(const uint32_t*)ADDR(st + 0x34);
            if (nb > 32 || (nb && !validGuest(bind))) pred = 0;
            for (uint32_t i = 0; pred && i < nb; ++i) {
                const uint32_t param = *(const uint32_t*)ADDR(bind + i * 32 + 0xc);
                if (!validGuest(param) || paramEpoch(*(const uint32_t*)ADDR(param)) > m->eval) pred = 0;
            }
        }
    }
    if (mode == 2 && pred) {
        { uint32_t r; if (!nfsD3dDeviceCall(((m->token - 0x7e000000u) % 4096) / 16, m->a, &r)) nfsD3dReplay(m->token, m->a); }
        if (*(const uint32_t*)ADDR(st)) *(uint32_t*)ADDR(st) = *(const uint32_t*)ADDR(st + 4);
        ++vReplay[type]; fxStateStats();
        g_eax = 0; g_esp += 16; return;  /* ret 0xc */
    }
    cap.n = 0; cap.bad = 0;
    const uint32_t evalAt = counter;
    ++nfs_fx_cap_depth;
    sub_00668FAA_orig();
    --nfs_fx_cap_depth;
    ++vCalls[type];
    if (cap.bad || cap.n != 1 || g_eax != 0) { ++vBad[type]; m->st = 0; }
    else {
        if (pred) {
            ++vPred[type];
            if (m->token != cap.token || memcmp(m->a, cap.a, sizeof m->a)) {
                static int shown; ++vWrong[type];
                if (shown++ < 20) printf("[NFSU2 FXS] wrong prediction type %u st=%08x a1=%08x a2=%08x a3=%08x (was %08x %08x %08x)\n",
                    type, st, cap.a[1], cap.a[2], cap.a[3], m->a[1], m->a[2], m->a[3]);
            }
        }
        m->st = st; m->idx = idx; m->eval = evalAt; m->token = cap.token; memcpy(m->a, cap.a, sizeof m->a);
    }
    if (nfs_fx_cap_depth) cap.bad = 1;
    fxStateStats();
}

/*
 * ---- Native D3DX effect engine, part 1: sub_006689E4 ----
 *
 * Applies the sampler states (type 3, SetSamplerState) and textures (type 9, SetTexture from a texture
 * parameter) of the sub-passes that a shader's samplers reference: for each entry e in [a1, a2) (0x64
 * bytes), the passes [effect+0x2c] + (u16[c] + u16[c+2] - 1)*0x64 ... + u16[e+0x56] passes, where
 * c = [e+0x48] + [a0]; the first sampler index is u16[e+0x54], +1 per pass. Dirty sampler states are
 * re-evaluated first (sub_00664743, cached). ~18% of BeginPass in the lifted version (v16 device).
 * NFSU2_FX_NATIVE: 0 lifted, 1 verify (run the lifted code with its device calls captured, then compute
 * the native call list without issuing it and compare), 2 native (default).
 */
extern void sub_006689E4_orig(void);
extern recomp_func_t recomp_lookup_cacheable(uint32_t va);
#define M32(a) (*(uint32_t*)ADDR(a))
#define M16(a) (*(uint16_t*)ADDR(a))
#define M8(a) (*(uint8_t*)ADDR(a))
static int g_fxNative = -1;
static inline int fxNativeMode(void) {
    if (__builtin_expect(g_fxNative < 0, 0)) { const char* e = getenv("NFSU2_FX_NATIVE"); g_fxNative = e ? atoi(e) : 2; }
    return g_fxNative;
}
/* A device method call: queued natively when possible, otherwise the regular guest COM path. */
static uint32_t devCall(uint32_t method, int n, const uint32_t* a) {
    uint32_t r;
    if (nfs_fx_cap_depth) cap.bad = 1;  /* not visible to an enclosing capture */
    if (nfsD3dDeviceCall(method, a, &r)) return r;
    const uint32_t token = 0x7e002000u + method * 16;
    for (int i = n - 1; i >= 0; --i) { g_esp -= 4; M32(g_esp) = a[i]; }
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;
    g_cpu.icall_va = token;
    recomp_lookup_cacheable(token)();
    return g_eax;
}
static uint32_t call664743(uint32_t fx, uint32_t pass, uint32_t state) {
    g_esp -= 4; M32(g_esp) = state;
    g_esp -= 4; M32(g_esp) = pass;
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;
    g_ecx = fx;
    sub_00664743();
    return g_eax;
}
/* dry: record the calls into out[] instead of issuing them (verify mode; dirty states make it give up). */
static int32_t native6689E4(uint32_t fx, uint32_t a0, uint32_t a1, uint32_t a2, int dry, DevRec* out, int* nout) {
    const uint32_t passes = M32(fx + 0x2c), dev = M32(fx + 8);
    for (uint32_t e = a1; e < a2; e += 0x64) {
        const uint32_t c = M32(e + 0x48) + M32(a0);
        uint32_t sampler = M16(e + 0x54);
        uint32_t edi = passes + (uint32_t)(M16(c + 2) + M16(c)) * 0x64 - 0x64;
        const uint32_t end = edi + (uint32_t)M16(e + 0x56) * 0x64;
        for (; edi < end; edi += 0x64, ++sampler) {
            const uint32_t cnt = M16(edi + 0x36);
            uint32_t esi = M32(edi + 4);
            for (uint32_t k = 0; k < cnt; ++k, esi += 0x64) {
                const uint32_t t = M8(esi + 0x5b);
                uint32_t r = 0, a[4];
                if (t == 3) {
                    if (M32(esi) && !dry) {  /* dry (verify): the lifted run just evaluated it */
                        r = call664743(fx, (uint16_t)((edi - passes) / 0x64), k);
                        if ((int32_t)r < 0) return (int32_t)r;
                        M32(esi) = M32(esi + 4);
                    }
                    a[0] = dev; a[1] = sampler; a[2] = M32(esi + 0x58) & 0xFFFFFFu; a[3] = M32(M32(esi + 0x48) + M32(edi));
                    if (dry) { if (*nout >= 1024) return -3; out[*nout].method = 69; for (int i = 0; i < 4; ++i) out[*nout].a[i] = a[i]; ++*nout; }
                    else r = devCall(69, 4, a);
                } else if (t == 9) {
                    const uint32_t p = M32(M32(fx + 0xb0) + M32(esi + 0x1c) * 4), q = M32(p + 0x40);
                    const uint32_t i0 = M32(M32(esi + 8));
                    const uint32_t ti = q ? M32(M32(q + 8) + i0 * 4) : i0;
                    a[0] = dev; a[1] = sampler; a[2] = M32(M32(fx + 0x18) + ti * 24 + 4); a[3] = 0;
                    if (dry) { if (*nout >= 1024) return -3; out[*nout].method = 65; for (int i = 0; i < 4; ++i) out[*nout].a[i] = a[i]; ++*nout; }
                    else r = devCall(65, 3, a);
                } else continue;
                if ((int32_t)r < 0) return (int32_t)r;
            }
        }
    }
    return 0;
}
static DevRec dryList[1024];
static uint32_t nvCalls, nvSame, nvDiff, nvGaveUp, nvFrame, nvFrames;
void sub_006689E4(void) {
    const int mode = fxNativeMode();
    const uint32_t fx = g_ecx, a0 = M32(g_esp + 4), a1 = M32(g_esp + 8), a2 = M32(g_esp + 12);
    if (mode == 2) {
        const int32_t r = native6689E4(fx, a0, a1, a2, 0, 0, 0);
        g_eax = (uint32_t)r; g_esp += 16; return;  /* ret 0xc */
    }
    if (mode != 1 || bigActive) { sub_006689E4_orig(); return; }
    bigActive = 1; bigN = 0; bigBad = 0; ++nfs_fx_cap_depth;  /* makes d3dDispatchFromCallVa report calls */
    sub_006689E4_orig();
    bigActive = 0; --nfs_fx_cap_depth;
    const uint32_t eax = g_eax;
    int n = 0;
    const int32_t r = native6689E4(fx, a0, a1, a2, 1, dryList, &n);
    ++nvCalls;
    if (r == -2 || bigBad) ++nvGaveUp;
    else if (n == bigN && !memcmp(dryList, bigList, sizeof(DevRec) * (size_t)n) && (uint32_t)r == eax) ++nvSame;
    else {
        static int shown;
        ++nvDiff;
        if (shown++ < 20) {
            printf("[NFSU2 FXN] 6689E4 mismatch: lifted %d calls (eax %08x), native %d (r %08x)\n", bigN, eax, n, (uint32_t)r);
            for (int i = 0; i < bigN || i < n; ++i)
                printf("   %d: lifted m%u %08x %08x %08x | native m%u %08x %08x %08x\n", i, i < bigN ? bigList[i].method : 0, i < bigN ? bigList[i].a[1] : 0, i < bigN ? bigList[i].a[2] : 0, i < bigN ? bigList[i].a[3] : 0,
                       i < n ? dryList[i].method : 0, i < n ? dryList[i].a[1] : 0, i < n ? dryList[i].a[2] : 0, i < n ? dryList[i].a[3] : 0);
        }
    }
    g_eax = eax;
    const uint32_t now = getD3D9PresentCount();
    if (now != nvFrame) { nvFrame = now; if (++nvFrames % 240 == 0) printf("[NFSU2 FXN] 6689E4 verify: %u calls, %u same, %u DIFFERENT, %u not checkable\n", nvCalls, nvSame, nvDiff, nvGaveUp); }
}

/*
 * ---- Native D3DX effect engine, part 2: sub_00668EC5 (apply all states of a pass) ----
 *
 * sub_00668EC5(this = effect, pass, index): optional pre-steps ([effect+0x118] & 8: sub_00663D60 /
 * sub_00663EBB; [pass+8] & 8: sub_00666B1B / sub_00667A75; mode [effect+0x3c] == 4 with [pass+8] & 1:
 * sub_00668B21 instead of the loop) - those cases run the lifted function - then sub_00668FAA(pass, i,
 * index) for each of the u16 [pass+0x36] states, stopping at the first failure unless mode == 1.
 * The loop runs natively here: replayable states (types 1-3 with an unchanged memo) are applied inline,
 * the rest go to the sub_00668FAA wrapper as before. Saves the wrapper's guest call and lookups for the
 * ~75% of applications that are replays (the wrapper alone was 4% of the game thread on the device, v17).
 */
extern void sub_00668EC5_orig(void);
static inline int fastApply(uint32_t passPtr, uint32_t i, uint32_t idx) {
    const uint32_t st = M32(passPtr + 4) + i * 0x64;
    const uint32_t type = M8(st + 0x5b) & 31;
    if (type < 1 || type > 3) return 0;
    FxMemo* m = &memo[((st >> 2) ^ (idx * 0x9e37u)) & (MSLOTS - 1)];
    if (m->st != st || m->idx != idx || m->eval < globalEpoch) return 0;
    const uint32_t flags = M32(st + 0x30), pre = M32(st + 0x38);
    if (!(flags & 4) && pre) {
        const uint32_t nb = M16(st + 0x4e), bind = M32(st + 0x34);
        if (nb > 32 || (nb && !validGuest(bind))) return 0;
        for (uint32_t k = 0; k < nb; ++k) {
            const uint32_t param = M32(bind + k * 32 + 0xc);
            if (!validGuest(param) || paramEpoch(M32(param)) > m->eval) return 0;
        }
    }
    uint32_t r;
    if (!nfsD3dDeviceCall(((m->token - 0x7e000000u) % 4096) / 16, m->a, &r)) nfsD3dReplay(m->token, m->a);
    if (M32(st)) M32(st) = M32(st + 4);
    return 1;
}
static uint32_t guestApply(uint32_t fx, uint32_t passArg, uint32_t i, uint32_t idx) {
    g_esp -= 4; M32(g_esp) = idx;
    g_esp -= 4; M32(g_esp) = i;
    g_esp -= 4; M32(g_esp) = passArg;
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;
    g_ecx = fx;
    sub_00668FAA();
    return g_eax;
}
extern void sub_00663D60(void);
extern void sub_00663EBB(void);
extern void sub_00666B1B(void);
extern void sub_00667A75(void);
/* thiscall/stdcall guest call: args[0] is the first argument (pushed last). */
static uint32_t guestCallN(recomp_func_t fn, uint32_t thisPtr, int n, const uint32_t* args) {
    for (int i = n - 1; i >= 0; --i) { g_esp -= 4; M32(g_esp) = args[i]; }
    g_esp -= 4; M32(g_esp) = RECOMP_RETADDR;
    g_ecx = thisPtr;
    fn();
    return g_eax;
}
void sub_00668EC5(void) {
    const uint32_t fx = g_ecx;
    if (fxStateMode() != 2 || fxNativeMode() != 2 || nfs_fx_cap_depth || !validGuest(fx)) { sub_00668EC5_orig(); return; }
    const uint32_t passArg = M32(g_esp + 4), idx = M32(g_esp + 8);
    const uint32_t passes = M32(fx + 0x2c);
    if (!validGuest(passes)) { sub_00668EC5_orig(); return; }
    const uint32_t passPtr = passes + (passArg & 0xffff) * 0x64;
    const uint32_t mode = M32(fx + 0x3c);
    if ((mode == 4 && (M8(passPtr + 8) & 1)) || !validGuest(M32(passPtr + 4))) { sub_00668EC5_orig(); return; }
    /* Pass shaders ([effect+0x118] & 8) and pass pre-step ([pass+8] & 8): the lifted helpers, as the
     * original does (the v18 device run showed [effect+0x118] & 8 set on nearly every pass, which made
     * the native loop fall back to the lifted one). */
    if (M8(fx + 0x118) & 8) {
        uint32_t a[2] = {M16(passPtr + 0x18) != 0xFFFFu, passPtr + 0x1c};
        uint32_t r = guestCallN(sub_00663D60, fx, 2, a);
        if ((int32_t)r < 0) { g_eax = r; g_esp += 12; return; }
        a[0] = M16(passPtr + 0x1a) != 0xFFFFu; a[1] = passPtr + 0x24;
        r = guestCallN(sub_00663EBB, fx, 2, a);
        if ((int32_t)r < 0) { g_eax = r; g_esp += 12; return; }
    }
    if (M8(passPtr + 8) & 8) {
        const uint32_t a[1] = {passArg & 0xffff};
        const uint32_t r = guestCallN(M16(passPtr + 0x18) == 0xFFFFu ? sub_00667A75 : sub_00666B1B, fx, 1, a);
        if ((int32_t)r < 0) { g_eax = r; g_esp += 12; return; }
    }
    const uint32_t n = M16(passPtr + 0x36);
    uint32_t result = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (fastApply(passPtr, i, idx)) continue;
        const uint32_t r = guestApply(fx, passArg, i, idx);
        if ((int32_t)r < 0 && mode != 1) { result = r; break; }
    }
    g_eax = result; g_esp += 12;  /* ret 8 */
}
#endif /* !NFS_FX_PRESHADER_ONLY */

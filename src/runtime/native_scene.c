/*
 * native_scene.c - native versions of the hottest lifted scene functions on the game thread (R36S).
 *
 * A VM profile of driving (2026-10-22) has no hot simulation function: the simulation tree spreads over
 * hundreds of small functions. The hottest self-contained lifted code is the scene work below. These ports do
 * the same reads, writes and calls in the same order; lifted helpers are called through the guest stack as in
 * the original. The lifted definitions are renamed *_orig (build_guest_vm.sh wrap list).
 *   NFSU2_NATIVE_SCENE=0  lifted functions
 *                      1  native (default)
 *                      2  verify: run both and compare ([NFSU2 SCENE])
 *
 *   sub_00440EA0  bPool allocation (cdecl: pool): free-list head, use counters, optional clearing. An empty
 *                 pool (the grow path) runs the lifted function.
 *   sub_005CE860  queue one render item (cdecl, 9 arguments): when the material's current group has another
 *                 shader, a new group from pool [0x870918] is linked into its bucket (0x870D30 / 0x8712F0 +
 *                 (layer * 23 + shader type) * 8); the item from pool [0x87091C] goes at the group's tail.
 *   sub_005C5600  submit one model (thiscall, 5 arguments): texture swaps on, per solid the frustum test,
 *                 texture and shader choice, sub_005CE860; texture swaps off.
 *   sub_005D6700  walk the scenery tree for one view (thiscall: view): box tests down the nodes (stack of
 *                 nodes with their visibility), sub_005D62C0 for each object of a visible node.
 * Verify mode: sub_00440EA0 / sub_005CE860 write natively with an undo log, undo, run the lifted function and
 * compare every logged address and the registers; sub_005C5600 / sub_005D6700 compute their calls without
 * making them (the texture swap calls are made, then undone by the lifted run's own pair) and compare them with
 * the calls the lifted function makes.
 */
#include "recomp_types.h"
#include "nfs_exe_addr.h"   /* v10 addresses -> the lifted exe's (config/exe_functions.txt) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern void sub_00440EA0_orig(void);
extern void sub_005CE860_orig(void);
extern void sub_005C5600_orig(void);
extern void sub_005D6700_orig(void);
extern void sub_005D62C0_orig(void);
extern void sub_005BC010(void);   /* frustum test (native_math.c) */
extern void sub_00490560(void);   /* texture swaps on */
extern void sub_00488DD0(void);   /* texture swaps off */
extern uint32_t getD3D9PresentCount(void);
#define M8(a) (*(uint8_t*)ADDR(a))
#define M16(a) (*(uint16_t*)ADDR(a))
#define M32(a) (*(uint32_t*)ADDR(a))

static int g_mode = -1;
static inline int mode(void) {
    if (g_mode < 0) { const char* e = getenv("NFSU2_NATIVE_SCENE"); g_mode = e ? atoi(e) : 1; }
    return g_mode;
}
/* NFSU2_SCENE_TIMING=1: time per call of sub_005C5600 and sub_005D6700 (callees included), lifted or native,
 * with the generic timer; printed every 600 frames ([NFSU2 SCENE] timing) */
static int g_timing = -1;
static uint64_t tTicks[2], tCalls[2];
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline int timing(void) {
    if (g_timing < 0) { const char* e = getenv("NFSU2_SCENE_TIMING"); g_timing = e && e[0] == '1'; }
    return g_timing;
}
static void timingReport(void) {
    static uint32_t f;
    const uint32_t now = getD3D9PresentCount();
    if (now == f) return;
    f = now;
    if (now % 600 || !tCalls[0]) return;
    uint64_t freq; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    printf("[NFSU2 SCENE] timing (%s): model %llu calls %.3f us/call, walk %llu calls %.3f us/call\n", mode() ? "native" : "lifted",
           (unsigned long long)tCalls[0], tTicks[0] * 1e6 / freq / tCalls[0], (unsigned long long)tCalls[1], tCalls[1] ? tTicks[1] * 1e6 / freq / tCalls[1] : 0.0);
}

/* ---- guest calls ---- */
static inline void push32(uint32_t v) { g_esp -= 4; M32(g_esp) = v; }
/* thiscall / stdcall: the callee pops its arguments */
static inline uint32_t calleePops(void (*fn)(void), uint32_t self, int n, const uint32_t* a) {
    for (int i = n - 1; i >= 0; --i) push32(a[i]);
    push32(RECOMP_RETADDR);
    g_ecx = self;
    fn();
    return g_eax;
}
static inline uint32_t frustum(uint32_t self, uint32_t mn, uint32_t mx, uint32_t xform) {
    const uint32_t a[3] = {mn, mx, xform};
    return calleePops(sub_005BC010, self, 3, a);
}

/* ---- verify support ---- */
static uint32_t vCalls[4], vBad[4];
static const char* const vName[4] = {"pool", "item", "model", "walk"};
static void verifyReport(void) {
    static uint32_t f;
    const uint32_t now = getD3D9PresentCount();
    if (now == f) return;
    f = now;
    if (now % 600 == 0)
        printf("[NFSU2 SCENE] verify: pool %u/%u bad, item %u/%u, model %u/%u, walk %u/%u\n",
               vCalls[0], vBad[0], vCalls[1], vBad[1], vCalls[2], vBad[2], vCalls[3], vBad[3]);
}
static void bad(int k, const char* what, uint32_t a, uint32_t native, uint32_t lifted) {
    if (vBad[k]++ < 12) printf("[NFSU2 SCENE] %s mismatch: %s %08x native %08x lifted %08x\n", vName[k], what, a, native, lifted);
}
/* write log (verify): native writes, then undone before the lifted run */
#define WLOG_MAX 16384
static struct { uint32_t a, old, val; } wlog[WLOG_MAX];
static int nlog = -1;   /* -1: not logging */
static inline void W32(uint32_t a, uint32_t v) {
    if (__builtin_expect(nlog >= 0, 0) && nlog < WLOG_MAX) { wlog[nlog].a = a; wlog[nlog].old = M32(a); wlog[nlog].val = v; ++nlog; }
    M32(a) = v;
}
static void undoLog(void) { for (int i = nlog - 1; i >= 0; --i) M32(wlog[i].a) = wlog[i].old; }
static void checkLog(int k) {
    if (nlog >= WLOG_MAX) { bad(k, "write log full", 0, 0, 0); return; }
    /* the last native write of each address against the lifted function's result */
    for (int i = 0; i < nlog; ++i) {
        int last = 1;
        for (int j = i + 1; j < nlog; ++j) if (wlog[j].a == wlog[i].a) { last = 0; break; }
        if (last && M32(wlog[i].a) != wlog[i].val) bad(k, "write", wlog[i].a, wlog[i].val, M32(wlog[i].a));
    }
}

/* ---- bPool ---- */
static inline uint32_t poolAlloc(uint32_t pool) {   /* the free list is not empty */
    const uint32_t blk = M32(pool + 0x10);
    const uint32_t next = M32(blk);
    const uint32_t cnt = M32(pool + 0x18) + 1;
    if ((int32_t)cnt > (int32_t)M32(pool + 0x1C)) W32(pool + 0x1C, cnt);
    W32(pool + 0x18, cnt);
    W32(pool + 0x10, next);
    if (M8(pool + 0x14) & 2) {
        const int32_t n = (int32_t)M32(pool + 0x28) >> 2;
        for (int32_t i = 0; i < n; ++i) W32(blk + 4u * (uint32_t)i, 0);
    }
    return blk;
}
/* ecx as the original leaves it: the next block, or the clearing loop's count */
static inline uint32_t poolEcx(uint32_t pool, uint32_t next) {
    if (!(M8(pool + 0x14) & 2)) return next;
    const int32_t n = (int32_t)M32(pool + 0x28) >> 2;
    return n > 0 ? 0 : (uint32_t)n;
}
void sub_00440EA0(void) {
    const int m = mode();
    const uint32_t pool = M32(g_esp + 4);
    if (m == 0 || !M32(pool + 0x10)) { sub_00440EA0_orig(); return; }
    const uint32_t next = M32(M32(pool + 0x10));
    if (m == 2) {
        nlog = 0;
        const uint32_t blk = poolAlloc(pool);
        const uint32_t ecx = poolEcx(pool, next);
        undoLog();
        sub_00440EA0_orig();
        ++vCalls[0];
        checkLog(0);
        if (g_eax != blk) bad(0, "eax", pool, blk, g_eax);
        if (g_ecx != ecx) bad(0, "ecx", pool, ecx, g_ecx);
        nlog = -1;
        verifyReport();
        return;
    }
    const uint32_t blk = poolAlloc(pool);
    g_eax = blk; g_edx = blk; g_ecx = poolEcx(pool, next);
    g_esp += 4;
}

/* ---- render items ---- */
#define CAP_MAX 1024
static int capItems = 0;   /* verify of sub_005C5600: record the item calls */
static uint32_t capList[CAP_MAX][9]; static int capN;
static uint32_t capDbg[CAP_MAX][3];   /* verify diagnostics: blend/type/flag/k, sel, s before the last rules */
static void addItemNative(const uint32_t* a) {
    const uint32_t solid = a[0], shader = a[3], mat = a[4];
    const uint32_t matData = M32(mat);
    const uint32_t cur = M32(matData + 0x1C);
    if (!cur || M32(cur + 0x14) != shader) {
        const uint32_t g = poolAlloc(M32(0x870918));
        W32(g + 0x18, g + 0x18); W32(g + 0x1C, g + 0x18);
        W32(g + 0x10, mat); W32(g + 8, 0); W32(g + 0xC, 0);
        W32(matData + 0x1C, g);
        const uint32_t idx = (uint32_t)((int32_t)(int8_t)M8(mat + 0x51) * 0x17) + M32(shader + 4);
        const uint32_t bucket = idx * 8 + (M8(mat + 0x54) ? 0x8712F0u : 0x870D30u);
        const uint32_t tail = M32(bucket + 4);
        W32(tail, g); W32(bucket + 4, g); W32(g + 4, tail); W32(g, bucket);
        W32(0x87094C, M32(0x87094C) + 1);
    }
    const uint32_t grp = M32(matData + 0x1C);
    const uint32_t it = poolAlloc(M32(0x87091C));
    W32(it + 0x10, a[1]); W32(it + 0x14, a[2]); W32(it + 0x1C, a[6]); W32(it + 0x20, a[7]);
    W32(it + 8, solid); W32(it + 0xC, 0); W32(it + 0x24, a[5]); W32(it + 0x18, shader); W32(it + 0x28, a[8]);
    const uint32_t tail = M32(grp + 0x1C);
    W32(tail, it); W32(grp + 0x1C, it); W32(it, grp + 0x18); W32(it + 4, tail);
    const uint32_t cnt = M32(grp + 8) + 1, acc = M32(grp + 0xC);
    W32(grp + 0x14, shader); W32(grp + 8, cnt);
    const uint32_t add = M32(solid + 0xC);
    W32(grp + 0xC, acc + add);
    g_eax = acc + add; g_ecx = cnt; g_edx = add;
}
void sub_005CE860(void) {
    const int m = mode();
    uint32_t a[9];
    for (int i = 0; i < 9; ++i) a[i] = M32(g_esp + 4 + 4u * (uint32_t)i);
    if (capItems && capN < CAP_MAX) memcpy(capList[capN++], a, sizeof a);
    if (m == 0) { sub_005CE860_orig(); return; }
    /* an empty pool (grow path), or one pool for both: the lifted function */
    const uint32_t matData = M32(a[4]), cur = M32(matData + 0x1C);
    const int needGroup = !cur || M32(cur + 0x14) != a[3];
    const uint32_t poolG = M32(0x870918), poolI = M32(0x87091C);
    if ((needGroup && (!M32(poolG + 0x10) || poolG == poolI)) || !M32(poolI + 0x10)) { sub_005CE860_orig(); return; }
    if (m == 2) {
        nlog = 0;
        addItemNative(a);
        const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        undoLog();
        sub_005CE860_orig();
        ++vCalls[1];
        checkLog(1);
        if (g_eax != eax || g_ecx != ecx || g_edx != edx) bad(1, "registers", a[0], eax ^ ecx ^ edx, g_eax ^ g_ecx ^ g_edx);
        nlog = -1;
        verifyReport();
        return;
    }
    addItemNative(a);
    g_esp += 4;   /* ret (cdecl) */
}

/* ---- sub_005C5600: submit one model ---- */
static uint32_t jumpShader[4];   /* the switch at 0x5C57D7 (shader type 0..3 -> shader slot) */
static void initJumpShader(void) {
    if (jumpShader[0]) return;
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t t = M32(NFS_VA(0x005C58B8) + 4 * k);
        jumpShader[k] = t == NFS_VA(0x005C57DE) ? 0x86EB4C : t == NFS_VA(0x005C57E6) ? 0x86EB50 :
                        t == NFS_VA(0x005C5813) ? 0x86EB58 : t == NFS_VA(0x005C581B) ? 0x86EB54 : 0;
    }
}
/* dry: no item calls (recorded into capList) and no write of [M+0x38]; returns 0 when the model had nothing */
static int modelSubmit(uint32_t self, uint32_t sp, int dry, uint32_t* m38Out) {
    /* arguments: a0 the model instance, a1 its matrix, a2 the light context, a3 flags (its slot then holds the
     * chosen shader), a4 passed on to every item */
    const uint32_t a0 = M32(sp + 4), a1 = M32(sp + 8), a2 = M32(sp + 0xC), flags = M32(sp + 0x10), a4 = M32(sp + 0x14);
    const uint32_t P = M32(a0 + 0xC);
    if (!P) return 0;
    const uint32_t M = M32(P);
    const uint32_t typ = M32(M32(M + 0x3C) + 4);
    uint32_t m38 = M32(M + 0x38);
    if ((flags & 0x200) && M32(0x870CC4)) m38 = M32(0x870CD4) ? 0x7FF090u : 0x7FF0B4u;
    else if (!M32(0x870CD4)) m38 = 0x7FF0D8u;
    if (!dry) M32(M + 0x38) = m38;
    const int flagX = M8(0x8707CD) && M32(M32(0x870848)) == 0;
    uint32_t sel = M32(M + 0x3C);
    if (m38) {
        const uint32_t e = M32(m38 + typ * 4);
        if (e != 0xFFFFFFFFu) sel = M32(0x86EB28 + e * 4);
        m38 = 0;
        if (!dry) M32(M + 0x38) = 0;
    }
    *m38Out = m38;
    if (M32(a0 + 0x10)) calleePops(sub_00490560, a0, 0, NULL);
    if (M32(a0 + 8) == 0xBE43EDBBu && M32(0x8A1C8C) == 4 && M32(0x870D14)) sel = M32(0x86EB78);   /* sub_0060FB70 */
    if (M32(sel + 0x10) == 0) goto done;
    if (M32(M32(0x870848)) && M32(sel + 4) != 5) sel = M32(0x86EB44);
    if ((int32_t)M32(M + 0x10) <= 0) goto done;
    initJumpShader();
    uint32_t light = a2;   /* ebx: a2, or [this+0x54] once a type-5 shader comes without one */
    for (int32_t i = 0; i < (int32_t)M32(M + 0x10); ++i) {
        const uint32_t solid = M32(M + 0x14) + (uint32_t)i * 0x3C;
        if (!(flags & 4) && !(M8(solid + 0x38) & 1) && !frustum(self, solid, solid + 0x10, a1)) continue;
        const uint32_t mat = M32(M32(P + 0x2C) + (uint32_t)M8(solid + 0x1C) * 8 + 4);
        if (M32(mat + 0x24) == M32(0x870A18)) continue;
        const uint8_t ti = M8(solid + 0x20);
        const uint32_t tex = ti == 0xFF ? M32(0x7F399C) : M32(M32(P + 0x3C) + (uint32_t)ti * 8 + 4);   /* sub_0048AC20 */
        const uint8_t blend = M8(mat + 0x56);
        uint32_t s = sel;
        if (blend == 2 && (M32(s + 4) == 1 || M32(s + 4) == 0)) s = M32(0x86EB44);
        if (flagX) {
            if (blend != 2 && (typ == 2 || typ == 3)) {
                const uint32_t k = M32(s + 4);
                if (k <= 3 && jumpShader[k]) s = M32(jumpShader[k]);
            } else {
                const uint32_t k = M32(s + 4);
                int tail = blend == 0;   /* 0x5C5808 directly */
                if (!tail) {
                    if (k == 0) s = M32(0x86EB5C);
                    else if (k == 1 || k == 7) s = M32(0x86EB60);
                    else tail = 1;
                }
                if (tail) { if (k == 2) s = M32(0x86EB54); else if (k == 3) s = M32(0x86EB58); }
            }
        }
        if (M32(s + 4) == 5 && !light) light = M32(self + 0x54);
        { const uint32_t h = M32(mat + 0x24); if (h == 0x06D57415u || h == 0xBD3D01A8u) s = M32(0x86EB80); }
        const uint32_t args[9] = {solid, P, M32(solid + 0x38), s, mat, a1, light, tex, a4};
        if (dry) { if (capN < CAP_MAX) { capDbg[capN][0] = (uint32_t)blend << 24 | (typ & 0xFF) << 16 | (uint32_t)flagX << 8 | (M32(sel + 4) & 0xFF); capDbg[capN][1] = sel; capDbg[capN][2] = M32(mat + 0x24); memcpy(capList[capN++], args, sizeof args); } }
        else {
            for (int k = 8; k >= 0; --k) push32(args[k]);
            push32(RECOMP_RETADDR);
            sub_005CE860();
            g_esp += 36;
        }
    }
done:
    calleePops(sub_00488DD0, a0, 0, NULL);
    return 1;
}
static uint32_t verList[CAP_MAX][9], verDbg[CAP_MAX][3];
static void modelEntry(void) {
    const int m = mode();
    if (m == 0) { sub_005C5600_orig(); return; }
    const uint32_t self = g_ecx, sp = g_esp;
    if (m == 2) {
        uint32_t m38 = 0;
        const uint32_t P = M32(M32(sp + 4) + 0xC);
        capN = 0;
        const int had = modelSubmit(self, sp, 1, &m38);
        const int n = capN;
        memcpy(verList, capList, sizeof(capList[0]) * (size_t)n);
        memcpy(verDbg, capDbg, sizeof(capDbg[0]) * (size_t)n);
        g_ecx = self; g_esp = sp;
        capN = 0; capItems = 1;
        sub_005C5600_orig();
        capItems = 0;
        ++vCalls[2];
        if (n != capN) bad(2, "item count", P, (uint32_t)n, (uint32_t)capN);
        else for (int i = 0; i < n; ++i) for (int k = 0; k < 9; ++k)
            if (verList[i][k] != capList[i][k]) {
                if (vBad[2] < 12) printf("[NFSU2 SCENE] model item %d: blend/type/flag/k %08x sel %08x mat+24 %08x\n", i, verDbg[i][0], verDbg[i][1], verDbg[i][2]);
                bad(2, "item argument", (uint32_t)(i * 16 + k), verList[i][k], capList[i][k]); i = n; break; }
        if (had && M32(M32(P) + 0x38) != m38) bad(2, "[M+0x38]", P, m38, M32(M32(P) + 0x38));
        verifyReport();
        return;
    }
    uint32_t m38;
    if (!modelSubmit(self, sp, 0, &m38)) g_eax = 0;
    g_esp = sp + 0x18;   /* ret 0x14 */
}

/* ---- sub_005D6700: scenery tree walk ---- */
static int capWalk = 0;
static uint32_t walkList[CAP_MAX][4]; static int walkN;
void sub_005D62C0(void) {   /* only to record the calls in verify mode */
    if (capWalk && walkN < CAP_MAX) {
        walkList[walkN][0] = g_ecx; walkList[walkN][1] = M32(g_esp + 4); walkList[walkN][2] = M32(g_esp + 8); walkList[walkN][3] = M32(g_esp + 12);
        ++walkN;
    }
    sub_005D62C0_orig();
}
#define WALK_MAX 256
static void sceneWalk(uint32_t self, uint32_t view, int dry) {
    uint32_t node[WALK_MAX]; uint8_t vis[WALK_MAX];
    int n = 1;
    node[0] = M32(self + 0x28); vis[0] = 1;
    while (n > 0) {
        --n;
        const uint32_t nd = node[n];
        uint32_t v = vis[n];
        if (v == 1) v = frustum(M32(view + 0x40), nd, nd + 0xC, 0) & 0xFF;
        if (!v) continue;
        const int32_t count = (int16_t)M16(nd + 0x18);
        for (int32_t j = 0; j < count; ++j) {
            const int32_t c = (int16_t)M16(nd + 0x1A + 2u * (uint32_t)j);
            if (c >= 0) {
                if (dry) {
                    if (walkN < CAP_MAX) { walkList[walkN][0] = self; walkList[walkN][1] = (uint32_t)c; walkList[walkN][2] = view; walkList[walkN][3] = v; ++walkN; }
                } else {
                    const uint32_t a[3] = {(uint32_t)c, view, v};
                    calleePops(sub_005D62C0_orig, self, 3, a);
                }
            } else if (n < WALK_MAX) {
                node[n] = M32(self + 0x28) - (uint32_t)(c * 36);
                vis[n] = (uint8_t)v;
                ++n;
            }
        }
    }
}
static uint32_t walkVer[CAP_MAX][4];
static void walkEntry(void) {
    const int m = mode();
    if (m == 0) { sub_005D6700_orig(); return; }
    const uint32_t self = g_ecx, sp = g_esp, view = M32(sp + 4);
    if (m == 2) {
        walkN = 0;
        sceneWalk(self, view, 1);
        const int n = walkN;
        memcpy(walkVer, walkList, sizeof(walkList[0]) * (size_t)n);
        g_ecx = self; g_esp = sp;
        walkN = 0; capWalk = 1;
        sub_005D6700_orig();
        capWalk = 0;
        ++vCalls[3];
        if (n != walkN) bad(3, "object count", self, (uint32_t)n, (uint32_t)walkN);
        else for (int i = 0; i < n; ++i) for (int k = 0; k < 4; ++k)
            if (walkVer[i][k] != walkList[i][k]) { bad(3, "object call", (uint32_t)(i * 16 + k), walkVer[i][k], walkList[i][k]); i = n; break; }
        verifyReport();
        return;
    }
    sceneWalk(self, view, 0);
    g_esp = sp + 8;   /* ret 4 */
}

void sub_005C5600(void) {
    if (!timing()) { modelEntry(); return; }
    const uint64_t t = ticks(); modelEntry(); tTicks[0] += ticks() - t; ++tCalls[0]; timingReport();
}
void sub_005D6700(void) {
    if (!timing()) { walkEntry(); return; }
    const uint64_t t = ticks(); walkEntry(); tTicks[1] += ticks() - t; ++tCalls[1]; timingReport();
}

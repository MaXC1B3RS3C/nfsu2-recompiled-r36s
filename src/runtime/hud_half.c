/*
 * hud_half.c - draw the front-end/HUD renderer at half rate while driving (R36S).
 *
 * sub_005361B0 (called from sub_00537550, the FEng render) draws the HUD: ~200 textured quads, ~8% of
 * the game thread in a race. While driving it runs every other frame; on the frames in between, the
 * D3D9 calls it made last time are replayed natively (d3d9_render_thread.inc, hudRecordBegin, hudReplay), so
 * the HUD is still drawn every frame but its contents update at 15 Hz. Menus are not affected.
 * NFSU2_HUD_HALF=0 turns it off.
 */
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
extern void sub_005361B0_orig(void);
extern void nfsHudRecordBegin(void);
extern void nfsHudRecordEnd(void);
extern int nfsHudReplay(void);
extern int nfsGuestDriving(void);
extern uint32_t getD3D9PresentCount(void);

/*
 * ---- Lazy effect passes in the HUD (NFSU2_HUD_LAZY: 1 default, 0 off, 2 verify) ----
 * Every HUD quad (sub_005C4CF0) does CommitChanges, BeginPass(0), DrawPrimitiveUP, EndPass on the same
 * D3DX effect; BeginPass re-emits every state and constant of the pass (~2/3 of the HUD's time). While the
 * HUD runs, EndPass is deferred, and a following BeginPass of the same effect and pass becomes a
 * CommitChanges inside the still-open pass, unless a device state that the pass sets was changed in
 * between (the bridge watches them: nfsPassWatch) or anything else happened (another effect, Begin/End,
 * synchronous calls, Present). Verify (2): after each such CommitChanges the pass is re-begun and its device
 * calls must all be redundant (counted otherwise). Outside the HUD the overrides call the D3DX methods.
 */
extern void sub_006644A6(void); extern void sub_0066A9F6(void); extern void sub_0066A93B(void);
extern void sub_0065AAEC(void); extern void sub_0065AA06(void);
__attribute__((weak)) void nfsPassWatch(int mode) { (void)mode; }   /* d3d9_render_thread.inc */
__attribute__((weak)) int nfsPassConflict(void) { return 1; }
extern uint64_t nfsD3dPushedCount(void);
static int lazyMode = -1, lazyActive;
static uint32_t pFx, pPass; static int pOpen, pPending;
static uint32_t stLazyCommits, stLazyReal, stLazyConflicts, stLazyChecked, stLazyMissed;
int nfs_fx_lazy_enabled(void) {
    if (lazyMode < 0) { const char* e = getenv("NFSU2_HUD_LAZY"); lazyMode = e ? atoi(e) : 1; }
    return lazyMode != 0;
}
static inline void push32h(uint32_t v) { g_esp -= 4; *(uint32_t*)ADDR(g_esp) = v; }
static void realEndPass(uint32_t fx) { push32h(fx); push32h(RECOMP_RETADDR); sub_0065AAEC(); }
static void flushPending(void) {
    if (pPending) { pPending = 0; nfsPassWatch(0); realEndPass(pFx); }
    pOpen = 0;
}
void nfs_fx_lazy_begin_pass(void) {
    const uint32_t fx = *(const uint32_t*)ADDR(g_esp + 4), pass = *(const uint32_t*)ADDR(g_esp + 8);
    if (lazyActive && pPending && fx == pFx && pass == pPass) {
        pPending = 0;
        const int conflict = nfsPassConflict();
        nfsPassWatch(0);
        if (!conflict) {
            push32h(fx); push32h(RECOMP_RETADDR); sub_0066A93B();          /* CommitChanges */
            if (lazyMode == 2) {
                realEndPass(fx);
                const uint64_t p0 = nfsD3dPushedCount();
                push32h(pass); push32h(fx); push32h(RECOMP_RETADDR); sub_0066A9F6();
                const uint32_t missed = (uint32_t)(nfsD3dPushedCount() - p0);
                ++stLazyChecked; stLazyMissed += missed;
                if (missed) { static int shown; if (shown++ < 10) printf("[NFSU2 HUD] lazy pass: BeginPass after CommitChanges still changed %u device states\n", missed); }
            }
            ++stLazyCommits;
            g_eax = 0; g_esp += 12; return;
        }
        ++stLazyConflicts;
        realEndPass(pFx); pOpen = 0;
    } else if (pPending) flushPending();
    if (!lazyActive) { sub_0066A9F6(); return; }
    nfsPassWatch(1);
    sub_0066A9F6();
    nfsPassWatch(0);
    ++stLazyReal;
    pFx = fx; pPass = pass; pOpen = g_eax == 0;
}
void nfs_fx_lazy_end_pass(void) {
    const uint32_t fx = *(const uint32_t*)ADDR(g_esp + 4);
    if (lazyActive && pOpen && !pPending && fx == pFx) { pPending = 1; nfsPassWatch(2); g_eax = 0; g_esp += 8; return; }
    if (pPending) flushPending();
    pOpen = 0;
    sub_0065AAEC();
}
void nfs_fx_lazy_commit(void) {
    /* inside the deferred pass: the effect's own device calls are not conflicts */
    if (pPending) { nfsPassWatch(0); sub_0066A93B(); nfsPassWatch(3); return; }
    sub_0066A93B();
}
void nfs_fx_lazy_begin(void) { if (pPending || pOpen) flushPending(); sub_006644A6(); }
void nfs_fx_lazy_end(void) { if (pPending || pOpen) flushPending(); sub_0065AA06(); }
static void lazyRegion(int on) {
    if (on) { lazyActive = lazyMode > 0; return; }
    if (pPending || pOpen) flushPending();
    lazyActive = 0;
    static uint32_t n;
    if (++n % 240 == 0 && lazyMode > 0) {
        printf("[NFSU2 HUD] lazy passes per HUD run: committed %.1f, real %.1f (conflicts %.1f)", stLazyCommits / 240.0, stLazyReal / 240.0, stLazyConflicts / 240.0);
        if (lazyMode == 2) printf("; verified %u, missed device states %u", stLazyChecked, stLazyMissed);
        printf("\n");
        stLazyCommits = stLazyReal = stLazyConflicts = 0;
    }
}
static void hudOrig(void) { nfs_fx_lazy_enabled(); lazyRegion(1); sub_005361B0_orig(); lazyRegion(0); }

static uint32_t lastFrame = 0xffffffffu, lastRecorded = 0xffffffffu, replays, runs;
void sub_005361B0(void) {
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("NFSU2_HUD_HALF"); mode = e ? atoi(e) : 1; }
    const uint32_t frame = getD3D9PresentCount();
    const int firstCallThisFrame = frame != lastFrame;
    lastFrame = frame;
    if (!mode || !nfsGuestDriving() || !firstCallThisFrame) { hudOrig(); return; }
    if (frame == lastRecorded + 1 && nfsHudReplay()) {
        ++replays;
        g_esp += 4; return;  /* ret */
    }
    nfsHudRecordBegin();
    hudOrig();
    nfsHudRecordEnd();
    lastRecorded = frame;
    if (++runs % 120 == 0) printf("[NFSU2 HUD] half rate: %u runs, %u replays\n", runs, replays);
}

/*
 * world_cull.c - distance culling of the game's draw lists before they are drawn (R36S).
 *
 * sub_005CF150 flushes the render lists: for each of 23 shader slots ([0x86EB28 + 4*s]) a circular
 * list of material groups (head at 0x870D30 + 8*s); each group (material [g+0x10], set up by
 * sub_005BC940) owns a circular list of items at g+0x18. Per item ~55 us of game-thread work follows
 * (effect setup, BeginPass per pass, DrawIndexedPrimitive): item [+0x24] world matrix (0x872E90 = the
 * identity), [+0x10] -> [solid] with CPU vertex data [solid+0x28], byte size [solid+0x30], vertex
 * count [solid+0x34]. Items go back to the pool [0x87091C], groups to [0x870918] (sub_0043F970).
 * The current view is [[0x870848]+4]; its matrices are at [0x832DE0 + 0x70*view] -> view matrix
 * (D3D row-major), projection at +0x40.
 *
 * Items whose bounding sphere lies entirely beyond NFSU2_DRAW_DIST metres from the view's camera are
 * removed from the lists here (returned to the pool, as the flush does after drawing them), so the game
 * never does their per-item work. The GLES layer fades fog to opaque before that distance so nothing
 * visibly pops. NFSU2_DRAW_DIST=0 (default for now) only counts.
 */
#include "recomp_types.h"
#include "nfs_exe_addr.h"   /* v10 addresses -> the lifted exe's (config/exe_functions.txt) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
extern void sub_005CF150_orig(void);
extern uint32_t getD3D9PresentCount(void);
#define M32(a) (*(uint32_t*)ADDR(a))
#define MF(a) (*(float*)ADDR(a))
static int validGuest(uint32_t va) { return va >= 0x400000u && va < 0x30000000u; }

/* Per-solid bounding sphere (object space), computed once from its CPU vertex copy. */
#define BSLOTS 8192
typedef struct { uint32_t solid, data; float c[3], r; int ok; } Bound;
static Bound bounds[BSLOTS];
static const Bound* solidBound(uint32_t solid) {
    Bound* b = &bounds[(solid >> 3) & (BSLOTS - 1)];
    const uint32_t data = M32(solid + 0x28);
    if (b->solid == solid && b->data == data) return b;
    b->solid = solid; b->data = data; b->ok = 0;
    const uint32_t size = M32(solid + 0x30), n = M32(solid + 0x34);
    if (!validGuest(data) || !n || size < n * 12 || size % n || size / n > 256) return b;
    const uint32_t stride = size / n;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < n; ++i) {
        const float* p = (const float*)ADDR(data + i * stride);
        for (int k = 0; k < 3; ++k) { if (!(p[k] == p[k]) || fabsf(p[k]) > 1e6f) return b; if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
    }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) { b->c[k] = 0.5f * (mn[k] + mx[k]); const float h = 0.5f * (mx[k] - mn[k]); r2 += h * h; }
    b->r = sqrtf(r2); b->ok = 1;
    return b;
}

static uint32_t slotItems[23], slotGroups[23], stDiff[10], stDiffIdent;
static float drawDist = -1;
static uint32_t stFrame, stFrames, stItems, stCulled, stNoBound, stHist[8];

/*
 * ---- Native draw-list flush (port of sub_005CF150) ----
 * Same calls, same order, same globals: lifted helpers are called through the guest stack
 * (cdecl/thiscall/stdcall as in the original), device and effect methods through their vtables.
 * NFSU2_NATIVE_FLUSH=0 runs the lifted function. Buckets whose shader type is 0x14 (the path with x87
 * work at 0x5CF3A7) make the whole flush fall back to the lifted function.
 */
extern recomp_func_t recomp_lookup(uint32_t va);
extern recomp_func_t recomp_lookup_manual(uint32_t va);
extern recomp_func_t recomp_lookup_import(uint32_t va);
extern recomp_func_t recomp_lookup_cacheable(uint32_t va);
extern void sub_005BC940(void); extern void sub_005BDAF0(void); extern void sub_005BD7B0(void);
extern void sub_005BCE50(void); extern void sub_005BD1B0(void); extern void sub_005BD9E0(void);
extern void sub_0048B1B0(void); extern void sub_005CE750(void); extern void sub_0043CE60(void);
#define M8(a) (*(uint8_t*)ADDR(a))
static inline void push32(uint32_t v) { g_esp -= 4; M32(g_esp) = v; }
/* cdecl: args[0] first; the caller pops */
static uint32_t ccall(recomp_func_t fn, int n, const uint32_t* a) {
    for (int i = n - 1; i >= 0; --i) push32(a[i]);
    push32(RECOMP_RETADDR);
    fn();
    g_esp += 4u * (uint32_t)n;
    return g_eax;
}
/* indirect call to a guest address (vtable entry), as RECOMP_ICALL resolves it; callee cleans (COM/thiscall) */
static uint32_t icall(uint32_t va, uint32_t thisEcx, int n, const uint32_t* a) {
    for (int i = n - 1; i >= 0; --i) push32(a[i]);
    push32(RECOMP_RETADDR);
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_cacheable(va);
    if (!fn) fn = recomp_lookup_import(va);
    g_ecx = thisEcx;
    g_cpu.icall_va = va;
    if (fn) fn(); else { g_esp += 4u + 4u * (uint32_t)n; g_eax = 0; }
    return g_eax;
}
static uint32_t devMethod(uint32_t method, int n, const uint32_t* a) {  /* a[0] = device */
    return icall(M32(M32(a[0]) + method * 4), 0, n, a);
}
static uint32_t fxMethod(uint32_t fx, uint32_t index, int n, const uint32_t* a) {  /* a[0] = effect */
    (void)fx;
    return icall(M32(M32(a[0]) + index * 4), 0, n, a);
}
static int flushNeedsLifted(void) {
    for (uint32_t b = 0; b < 368; ++b) {
        const uint32_t head = 0x870D30u + 8 * b;
        if (M32(head) == head) continue;
        const uint32_t shader = M32(0x86EB28u + 4 * (b % 23));
        if (!validGuest(shader) || M32(shader + 4) == 0x14) return 1;
    }
    return 0;
}
typedef struct { uint32_t kind, method, a[9], off[2], len[2], arg[2]; } CapRec;
extern void nfsCapBegin(void);
extern int nfsCapEnd(void);
extern const void* nfsCapRecs(void);
extern const uint8_t* nfsCapBytes(void);
extern int nfsD3dDeviceCall(uint32_t method, const uint32_t* args, uint32_t* result);
extern int nfsD3dCall(uint32_t kind, uint32_t method, const uint32_t* args);
extern int nfsD3dDrawInstanced(const uint32_t* dip, uint32_t registerCount, const uint16_t* registers, uint32_t instances, const float* data);   /* d3d9_render_thread.inc */
extern uint32_t nfsD3dTextureChurn(void);              /* d3d9_bridge.cpp: texture objects created or retired so far */
extern uint32_t nfsD3dObjectSerial(uint32_t handle);   /* d3d9_bridge.cpp: per-object serial, 0 = no such object */
static uint32_t currentTextureChurn(void);
/*
 * ---- Item runs with a learned constant model (NFSU2_FLUSH_MERGE, default 1) ----
 * Per item the game spends ~55 us on the device: effect parameters from the world matrix (sub_005BDAF0 and
 * friends), CommitChanges, BeginPass (state re-application, preshaders), DIP, EndPass. Consecutive items of a
 * group mostly differ in nothing but the world matrix (instanced scenery: same mesh, other placement); the
 * D3D calls BeginPass emits for them are then identical except the vertex/pixel shader constants, and each
 * constant register is either unchanged or a column of W*X with X independent of the item (WVP, WV, a
 * projected texture...), i.e. r = W * (W_head^-1 * r_head).
 * The first item of a run ("head") runs the full path with its D3D calls captured, and its pass is left open.
 * A following item with the same inputs except the matrix ([it+0x18], [it+0x1c], [it+0x20], [it+0x14],
 * [it+0x28] equal; single pass; no strip) is learned: it runs the full path too and its calls are compared
 * with the head's - every non-constant call must be identical, and each constant register keeps the
 * hypotheses (constant / W-linear) it satisfied. After 3 observations with a different matrix the model of
 * that effect + vertex shader is trusted: following items only get their W-linear registers set and are
 * drawn inside the open pass. Every 16th item still takes the full path and is checked; any mismatch
 * disables the model for good (logged as [NFSU2 FLUSH] model ... rejected).
 */
#define NREG 256
typedef struct {
    int valid;
    uint32_t sig[5];
    uint32_t fx, vs, ps, otherHash, nOther;
    uint32_t dbg[64][5];             /* method, a[1..3], data hash of the first other calls (rejection log) */
    float W[16];
    uint8_t has[2][NREG];
    float r[2][NREG][4];
    double x[2][NREG][4];
} Head;
typedef struct {
    uint32_t fx, key, vs, ps;
    int state;                       /* 0 learning, 1 trusted, -1 rejected */
    uint32_t nobs, nfast;
    uint8_t hyp[2][NREG];            /* bit0: constant, bit1: W-linear */
} Model;
#define MODELS 1024
static Model models[MODELS];
static uint32_t modelCount;
static Head heads[2];
/* ---- Group replay records (see "Group replay" below) ---- */
#define RC_MAX 64
#define RD_MAX 1024
#define RREG_MAX 96
typedef struct { uint32_t method, na, a[8], doff, dlen, darg; uint32_t textureSerial; } RCall;   /* a[k] = argument k+1 (a[0] of the call is the device); textureSerial: SetTexture's object */
typedef struct { uint16_t s, reg; float v[4]; } RReg;
typedef struct {
    int valid;
    uint32_t mat, slot, sig[5], vs, ps;
    uint32_t texturesCheckedAtChurn;   /* nfsD3dTextureChurn() when its SetTexture objects were last known current */
    int texturesStale;                 /* a recorded texture object is gone: re-record on the next full group */
    uint32_t cleanVerifies;            /* verifications passed in a row (spaces the next ones out) */
    int nset, ncalls; RCall calls[RC_MAX]; uint8_t data[RD_MAX]; uint32_t dused;
    int nregs; RReg regs[RREG_MAX];
    uint32_t uses, changes, flush; int bad;
} MatRec;
typedef struct { int open, commit; uint32_t fx; Head* head; Model* model; uint32_t lastW; float lastWv[16]; MatRec* pending; } RunState;
static int g_merge = -1;
static uint32_t stFlushItems, stFlushFast, stFlushLearn, stFlushVerify, stFlushNative, stFlushLifted, stRejected;
static void closeRun(RunState* rs) {
    if (rs->open) { const uint32_t a[1] = {rs->fx}; fxMethod(rs->fx, 68, 1, a); rs->open = 0; }
    rs->commit = 0;
}
static int invert4(const float* m, double* out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-9) return 0;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = a[p][j]; a[p][j] = a[c][j]; a[c][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; if (f != 0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
    return 1;
}
static inline void mulWx(const float* W, const double* x, float* out) {
    for (int i = 0; i < 4; ++i) out[i] = (float)(W[i * 4 + 0] * x[0] + W[i * 4 + 1] * x[1] + W[i * 4 + 2] * x[2] + W[i * 4 + 3] * x[3]);
}
static inline int close4(const float* a, const float* b) {
    for (int k = 0; k < 4; ++k) if (!(fabsf(a[k] - b[k]) <= 1e-3f + 2e-4f * fabsf(b[k]))) return 0;
    return 1;
}
static void sigOf(uint32_t it, uint32_t* s) { s[0] = M32(it + 0x18); s[1] = M32(it + 0x1c); s[2] = M32(it + 0x20); s[3] = M32(it + 0x14); s[4] = M32(it + 0x28); }
static int runnable(uint32_t it) {
    return !M32(it + 0xC) && !(M32(it + 0x28) && (M8(it + 0x14) & 1)) && validGuest(M32(it + 0x24)) && validGuest(M32(it + 0x10));
}
/* the head's constants and a hash of every other call it made (buffers and the draw excluded) */
static int buildHead(Head* h, uint32_t it, int n, const uint32_t* sig) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    memset(h->has, 0, sizeof h->has);
    h->valid = 0; h->vs = h->ps = 0; h->nOther = 0;
    uint32_t oh = 2166136261u;
#define HMIX(v) (oh = (oh ^ (uint32_t)(v)) * 16777619u)
    for (int i = 0; i < n; ++i) {
        const uint32_t m = r[i].method;
        if (r[i].kind != 2) { HMIX(r[i].kind); HMIX(m); continue; }
        if (m == 100 || m == 104 || m == 82) continue;
        if (m == 94 || m == 109) {
            const int s = m == 109;
            const uint32_t start = r[i].a[1], cnt = r[i].a[3];
            if (start + cnt > NREG || r[i].len[0] != cnt * 16) return 0;
            const float* f = (const float*)(bytes + r[i].off[0]);
            for (uint32_t k = 0; k < cnt; ++k) { memcpy(h->r[s][start + k], f + 4 * k, 16); h->has[s][start + k] = 1; }
            continue;
        }
        if (m == 92) h->vs = r[i].a[1];
        if (m == 107) h->ps = r[i].a[1];
        HMIX(m);
        /* argument count (with the device) of the methods BeginPass emits; the rest of a[] is stale */
        int na = 9;
        switch (m) {
            case 92: case 107: case 87: case 89: na = 2; break;
            case 57: case 65: case 44: case 46: na = 3; break;
            case 67: case 69: case 96: case 98: case 111: case 113: na = 4; break;
            default: break;
        }
        for (int k = 1; k < na; ++k) if (!((r[i].arg[0] == (uint32_t)k && r[i].len[0]) || (r[i].arg[1] == (uint32_t)k && r[i].len[1]))) HMIX(r[i].a[k]);
        uint32_t dh = 0;
        for (int sl = 0; sl < 2; ++sl) for (uint32_t k = 0; k < r[i].len[sl]; ++k) { HMIX(bytes[r[i].off[sl] + k]); dh = dh * 31u + bytes[r[i].off[sl] + k]; }
        if (h->nOther < 64) { uint32_t* d = h->dbg[h->nOther]; d[0] = m; d[1] = r[i].a[1]; d[2] = na > 2 ? r[i].a[2] : 0; d[3] = na > 3 ? r[i].a[3] : 0; d[4] = dh; }
        ++h->nOther;
    }
#undef HMIX
    h->otherHash = oh;
    memcpy(h->W, (const void*)ADDR(M32(it + 0x24)), 64);
    double inv[16];
    if (!invert4(h->W, inv)) return 0;
    for (int s = 0; s < 2; ++s) for (int k = 0; k < NREG; ++k) if (h->has[s][k])
        for (int i = 0; i < 4; ++i) h->x[s][k][i] = inv[i * 4 + 0] * h->r[s][k][0] + inv[i * 4 + 1] * h->r[s][k][1] + inv[i * 4 + 2] * h->r[s][k][2] + inv[i * 4 + 3] * h->r[s][k][3];
    memcpy(h->sig, sig, sizeof h->sig);
    h->fx = M32(sig[0] + 0x20);
    h->valid = 1;
    return 1;
}
static Model* findModel(const Head* h) {
    uint32_t k = (h->fx * 0x9e3779b1u) ^ (h->sig[0] * 0x85ebca6bu) ^ (h->vs * 0xc2b2ae35u) ^ h->ps;
    for (int probe = 0; probe < 16; ++probe) {
        Model* m = &models[(k + (uint32_t)probe) & (MODELS - 1)];
        if (m->fx == h->fx && m->key == h->sig[0] && m->vs == h->vs && m->ps == h->ps) return m;
        if (!m->fx) {
            m->fx = h->fx; m->key = h->sig[0]; m->vs = h->vs; m->ps = h->ps; m->state = 0; m->nobs = 0; m->nfast = 0;
            memset(m->hyp, 3, sizeof m->hyp);
            ++modelCount;
            return m;
        }
    }
    return NULL;
}
/* B ran the full path after A in the same run: check / refine A's model with it */
static void learn(const Head* A, const Head* B, Model* m, int verifying) {
    if (!m || m->state < 0) return;
    const char* why = NULL; int bs = -1, br = -1;
    if (A->vs != B->vs || A->ps != B->ps) why = "shaders";
    else if (A->otherHash != B->otherHash || A->nOther != B->nOther) why = "other calls";
    else if (memcmp(A->has, B->has, sizeof A->has)) why = "register set";
    else {
        for (int s = 0; s < 2 && !why; ++s) for (int k = 0; k < NREG; ++k) {
            if (!A->has[s][k]) continue;
            float p[4]; mulWx(B->W, A->x[s][k], p);
            const uint8_t h = (uint8_t)((close4(A->r[s][k], B->r[s][k]) ? 1 : 0) | (close4(p, B->r[s][k]) ? 2 : 0));
            if (verifying) {
                /* what the fast path would have sent must match */
                const uint8_t hy = m->hyp[s][k];
                const int okv = (hy & 1) ? (h & 1) : (h & 2);
                if (!okv) { why = "verify"; bs = s; br = k; break; }
            } else {
                m->hyp[s][k] &= h;
                if (!m->hyp[s][k]) { why = "register"; bs = s; br = k; break; }
            }
        }
    }
    if (why) {
        m->state = -1; ++stRejected;
        static int shown;
        if (shown++ < 24) {
            printf("[NFSU2 FLUSH] model fx %08x key %08x vs %08x ps %08x rejected (%s", m->fx, m->key, m->vs, m->ps, why);
            if (br >= 0) { float p[4]; mulWx(B->W, A->x[bs][br], p);
                printf(" %c%d: head %.4g %.4g %.4g %.4g, item %.4g %.4g %.4g %.4g, W-linear %.4g %.4g %.4g %.4g", bs ? 'p' : 'c', br,
                       A->r[bs][br][0], A->r[bs][br][1], A->r[bs][br][2], A->r[bs][br][3], B->r[bs][br][0], B->r[bs][br][1], B->r[bs][br][2], B->r[bs][br][3], p[0], p[1], p[2], p[3]); }
            printf(") after %u observations\n", m->nobs);
            if (!strcmp(why, "other calls")) {
                for (uint32_t i = 0; i < A->nOther && i < B->nOther && i < 64; ++i)
                    if (memcmp(A->dbg[i], B->dbg[i], 20)) { printf("   call %u: head m%u %08x %08x %08x %08x, item m%u %08x %08x %08x %08x\n", i,
                        A->dbg[i][0], A->dbg[i][1], A->dbg[i][2], A->dbg[i][3], A->dbg[i][4], B->dbg[i][0], B->dbg[i][1], B->dbg[i][2], B->dbg[i][3], B->dbg[i][4]); }
                printf("   calls: head %u, item %u\n", A->nOther, B->nOther);
            }
        }
        return;
    }
    if (!verifying && memcmp(A->W, B->W, 64) && ++m->nobs >= 3) m->state = 1;
}
/* argument count (with the device) of the device methods a group's setup and BeginPass emit; 0 = unknown */
static int naOf(uint32_t m) {
    switch (m) {
        case 92: case 107: case 87: case 89: return 2;
        case 57: case 65: case 44: case 46: return 3;
        case 67: case 69: case 94: case 109: case 96: case 98: case 111: case 113: return 4;
        default: return 0;
    }
}
/* append captured device calls (constants, buffers and draws excluded) to a record; 0 = not recordable */
static int recCalls(MatRec* rc, int n) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    for (int i = 0; i < n; ++i) {
        const uint32_t m = r[i].method;
        if (r[i].kind != 2) return 0;
        if (m == 100 || m == 104 || m == 82 || m == 94 || m == 109) continue;
        const int na = naOf(m);
        if (!na || rc->ncalls >= RC_MAX || r[i].len[1]) return 0;
        RCall* c = &rc->calls[rc->ncalls++];
        c->method = m; c->na = (uint32_t)na;
        for (int k = 1; k < na; ++k) c->a[k - 1] = r[i].a[k];
        c->textureSerial = m == 65 ? nfsD3dObjectSerial(c->a[1]) : 0;   /* SetTexture(stage, texture) */
        c->dlen = r[i].len[0]; c->darg = r[i].arg[0]; c->doff = rc->dused;
        if (c->dlen) {
            if (c->darg < 1 || c->darg >= (uint32_t)na || rc->dused + c->dlen > RD_MAX) return 0;
            memcpy(rc->data + rc->dused, bytes + r[i].off[0], c->dlen); rc->dused += (c->dlen + 3) & ~3u;
        }
    }
    return 1;
}
static void recHead(MatRec* rc, int n, const Head* h) {
    if (!recCalls(rc, n)) { rc->valid = 0; return; }
    rc->nregs = 0;
    for (int sh = 0; sh < 2; ++sh) for (int k = 0; k < NREG; ++k) if (h->has[sh][k]) {
        if (rc->nregs >= RREG_MAX) { rc->valid = 0; return; }
        RReg* q = &rc->regs[rc->nregs++]; q->s = (uint16_t)sh; q->reg = (uint16_t)k; memcpy(q->v, h->r[sh][k], 16);
    }
    memcpy(rc->sig, h->sig, sizeof rc->sig); rc->vs = h->vs; rc->ps = h->ps;
    rc->valid = 2;  /* complete */
}
static void paramCalls(uint32_t it) {
    const uint32_t view = M32(M32(0x870848) + 4);
    { const uint32_t a[2] = {M32(it + 0x24), view}; ccall(sub_005BDAF0, 2, a); }
    if (M32(M32(M32(it + 0x18) + 0x1c) + 0x59c)) {
        /* push 9; call 48B1B0; push eax; push [it+24]; call 5BD7B0; add esp, 0xc */
        push32(9); push32(RECOMP_RETADDR); sub_0048B1B0(); const uint32_t v9 = g_eax;
        { const uint32_t a[2] = {M32(it + 0x24), v9}; ccall(sub_005BD7B0, 2, a); }
        g_esp += 4;
    }
    if (M32(it + 0x1c)) { const uint32_t a[2] = {M32(it + 0x1c), M32(it + 0x24)}; ccall(sub_005BCE50, 2, a); }
    if (M32(it + 0x20)) { const uint32_t a[2] = {M32(it + 0x20), M32(it + 0x14)}; ccall(sub_005BD1B0, 2, a); }
}
/* device call: queued directly when the render thread can take it, else through the guest COM object */
static void devFast(uint32_t method, int n, const uint32_t* a) {
    uint32_t b[9] = {0};
    for (int i = 0; i < n && i < 9; ++i) b[i] = a[i];
    if (!nfsD3dCall(2, method, b)) devMethod(method, n, a);
}
/* vertex buffer (created on first use), stream source, indices; 0 = nothing to draw */
static int bindBuffers(uint32_t it, int fast) {
    const uint32_t strip = M32(it + 8);
    const uint32_t solid = M32(M32(it + 0x10));
    if (!M32(solid + 0x2c)) {
        /* first use: create the vertex buffer and fill it from the CPU copy (0x5CF2BE) */
        M32(solid + 0x40) = 1;
        const uint32_t dev = M32(0x870974);
        { const uint32_t a[7] = {dev, M32(solid + 0x30), 8, 0, 1, solid + 0x2c, 0}; devMethod(26, 7, a); }
        const uint32_t vb = M32(solid + 0x2c);
        push32(0); const uint32_t ptrSlot = g_esp;  /* the lock pointer, on the guest stack */
        { const uint32_t a[5] = {vb, 0, 0, ptrSlot, 0}; icall(M32(M32(vb) + 11 * 4), 0, 5, a); }
        const uint32_t dst = M32(ptrSlot);
        g_esp += 4;
        if (dst) memmove((void*)ADDR(dst), (const void*)ADDR(M32(solid + 0x28)), M32(solid + 0x30));
        { const uint32_t a[1] = {vb}; icall(M32(M32(vb) + 12 * 4), 0, 1, a); }
    }
    if (M32(solid + 0x2c) != M32(0x87092C)) {
        const uint32_t dev = M32(0x870974);
        const uint32_t a[5] = {dev, 0, M32(solid + 0x2c), 0, M32(M32(0x870910) + 8)};
        if (fast) devFast(100, 5, a); else devMethod(100, 5, a);
        M32(0x87092C) = M32(solid + 0x2c); M32(0x872EE8) += 1;
    }
    if (M32(solid + 0x20) != M32(0x870930)) {
        const uint32_t dev = M32(0x870974);
        const uint32_t a[2] = {dev, M32(solid + 0x20)};
        if (fast) devFast(104, 2, a); else devMethod(104, 2, a);
        M32(0x870930) = M32(solid + 0x20); M32(0x872EEC) += 1;
    }
    return M32(strip + 0xC) && M32(solid + 0x34) < 0xFFFF;
}
static void drawItem(uint32_t it, int fast) {
    const uint32_t strip = M32(it + 8);
    const uint32_t solid = M32(M32(it + 0x10));
    const uint32_t dev = M32(0x870974);
    const uint32_t prims = M32(strip + 0xC) / 3;
    const uint32_t start = (uint32_t)((int32_t)(M32(strip + 0x34) - M32(solid + 0x18)) >> 1);
    const uint32_t a[7] = {dev, 4, 0, 0, M32(solid + 0x34), start, prims};
    if (fast) devFast(82, 7, a); else devMethod(82, 7, a);
}
static void countItem(uint32_t it) {
    const uint32_t solid = M32(M32(it + 0x10));
    M32(solid + 0x40) = 1;
    M32(0x872EE0) += 1;
    M32(0x872EF0) += M32(solid + 0x34) / 3;
}
static int32_t itemPasses(uint32_t it) {
    int32_t npasses = (int32_t)M32(M32(it + 0x18) + 0x14);
    const uint32_t shader = M32(0x870910);
    const uint32_t stype = M32(shader + 4);
    if (!M8(0x8707CD) || M32(M32(0x870848)) != 0) {
        if (M8(shader + 0x18) && (stype == 0 || stype == 1 || stype == 5 || stype == 4)) npasses = 1;
    }
    return npasses;
}
/* the original per-item path; keepOpen: leave a single pass open for a run */
static void nativeItemFull(uint32_t it, RunState* rs, int keepOpen) {
    paramCalls(it);
    const uint32_t strip = M32(it + 8);
    const uint32_t fx = M32(M32(it + 0x18) + 0x20);
    if (M32(it + 0xC)) {
        /* strip path (0x5CF539): z writes off, DrawPrimitiveUP triangle strips, z writes on */
        uint32_t dev = M32(0x870974);
        { const uint32_t a[3] = {dev, 0xE, 0}; devMethod(57, 3, a); }
        M32(0x87092C) = 0;
        const uint32_t npasses = M32(M32(it + 0x18) + 0x14);
        { const uint32_t a[1] = {fx}; fxMethod(fx, 67, 1, a); }
        for (uint32_t p = 0; (int32_t)p < (int32_t)npasses; ++p) {
            { const uint32_t a[2] = {fx, p}; fxMethod(fx, 66, 2, a); }
            dev = M32(0x870974);
            { const uint32_t a[5] = {dev, 5, M32(strip) - 2, strip + 4, 0x18}; devMethod(83, 5, a); }
            { const uint32_t a[1] = {fx}; fxMethod(fx, 68, 1, a); }
        }
        dev = M32(0x870974);
        { const uint32_t a[3] = {dev, 0xE, 1}; devMethod(57, 3, a); }
        M32(0x872EE4) += 1;
        push32(strip); push32(RECOMP_RETADDR); g_ecx = M32(0x870954); sub_0043CE60();  /* thiscall, ret 4 */
        return;
    }
    if (M32(it + 0x28) && (M8(it + 0x14) & 1)) { const uint32_t a[2] = {M32(it + 0x28), strip}; ccall(sub_005BD9E0, 2, a); }
    if (!bindBuffers(it, 1)) return;
    const int32_t npasses = itemPasses(it);
    { const uint32_t a[1] = {fx}; fxMethod(fx, 67, 1, a); }
    for (int32_t p = 0; p < npasses; ++p) {
        { const uint32_t a[2] = {fx, (uint32_t)p}; fxMethod(fx, 66, 2, a); }
        drawItem(it, 1);
        if (npasses == 1 && keepOpen) { rs->open = 1; rs->fx = fx; break; }
        { const uint32_t e[1] = {fx}; fxMethod(fx, 68, 1, e); }
    }
    countItem(it);
}
/* an item of a trusted run: its W-linear constant registers, buffers, draw - inside the open pass */
static void fastItem(uint32_t it, RunState* rs) {
    const Head* h = rs->head;
    const Model* m = rs->model;
    const uint32_t wp = M32(it + 0x24);
    const float* W = (const float*)ADDR(wp);
    if (!(rs->lastW == wp && !memcmp(rs->lastWv, W, 64))) {
        const uint32_t dev = M32(0x870974);
        for (int s = 0; s < 2; ++s) {
            int k = 0;
            while (k < NREG) {
                if (!(h->has[s][k] && m->hyp[s][k] == 2)) { ++k; continue; }
                int e = k; while (e < NREG && h->has[s][e] && m->hyp[s][e] == 2) ++e;
                const uint32_t cnt = (uint32_t)(e - k);
                g_esp -= cnt * 16; const uint32_t buf = g_esp;
                float* out = (float*)ADDR(buf);
                for (int j = k; j < e; ++j) mulWx(W, h->x[s][j], out + 4 * (j - k));
                const uint32_t a[4] = {dev, (uint32_t)k, buf, cnt};
                uint32_t r;
                if (!nfsD3dDeviceCall(s ? 109 : 94, a, &r)) devMethod(s ? 109 : 94, 4, a);
                g_esp += cnt * 16;
                k = e;
            }
        }
        rs->lastW = wp; memcpy(rs->lastWv, W, 64);
    }
    if (bindBuffers(it, 1)) { drawItem(it, 1); countItem(it); }
}
/* Items whose effect info key ([it+0x18]) has a rejected run model and a rejected group model (cars:
 * per-item lighting) gain nothing from the capture and head build (~3 ms per frame on the device). */
static struct { uint32_t key; uint8_t skip; } noCap[512];
static int groupModelRejected(uint32_t vs, uint32_t ps);
/*
 * ---- Commit runs (NFSU2_COMMIT_RUN: 1 default, 0 off, 2 verify) ----
 * Items whose constants fit no learned model (cars: per-car lighting, the eye position in object space)
 * took the whole per-item effect path: parameters, CommitChanges, BeginPass (which re-emits every state
 * and constant of the pass: ~60% of the item), draw, EndPass. Consecutive such items of one group with the
 * same effect and a single pass now stay inside the pass the first one opened: parameters, CommitChanges
 * (D3DX applies only the states and constants of the parameters that changed, as the API prescribes for
 * parameter changes inside a pass), draw. Between them only stream sources and indices change.
 * Verify (2): after each CommitChanges the pass is re-begun and every device call it makes must be a no-op
 * (filtered as equal to the queued state); the rest are counted as missed (and corrected by that BeginPass).
 */
static int g_commitRun = -1;
/* effect info keys ([it+0x18]) whose group model was rejected: their items take commit runs */
static uint32_t commitFxList[16];
static int commitFx(uint32_t fx) { for (int i = 0; i < 16 && commitFxList[i]; ++i) if (commitFxList[i] == fx) return 1; return 0; }
static void addCommitFx(uint32_t fx) { for (int i = 0; i < 16; ++i) { if (commitFxList[i] == fx) return; if (!commitFxList[i]) { commitFxList[i] = fx; printf("[NFSU2 COMMIT] effect key %08x: items drawn in commit runs\n", fx); return; } } }
static uint32_t stCommitItems, stCommitFull, stCommitMissed, stCommitChecked, stCommitCross;
static int g_commitCross = -1;
static uint64_t stCommitNs, stFullNs;
static int g_commitTime = -1;
__attribute__((weak)) uint64_t nfsD3dPushedCount(void) { return 0; }  /* d3d9_render_thread.inc */
static uint64_t nowNs(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void commitRunItem(uint32_t it, RunState* rs) {
    const uint32_t fx = M32(M32(it + 0x18) + 0x20);
    const uint64_t t0 = g_commitTime ? nowNs() : 0;
    if (rs->open && rs->commit && rs->fx == fx && itemPasses(it) == 1) {
        paramCalls(it);
        if (bindBuffers(it, 1)) {
            { const uint32_t a[1] = {fx}; fxMethod(fx, 67, 1, a); }
            if (g_commitRun == 2) {
                { const uint32_t a[1] = {fx}; fxMethod(fx, 68, 1, a); }
                const uint64_t p0 = nfsD3dPushedCount();
                { const uint32_t a[2] = {fx, 0}; fxMethod(fx, 66, 2, a); }
                const uint64_t missed = nfsD3dPushedCount() - p0;
                ++stCommitChecked;
                if (missed) { stCommitMissed += (uint32_t)missed; static int shown; if (shown++ < 10) printf("[NFSU2 COMMIT] item %08x: BeginPass after CommitChanges still changed %u device states\n", it, (unsigned)missed); }
            }
            drawItem(it, 1); countItem(it);
        }
        ++stCommitItems;
        if (g_commitTime) stCommitNs += nowNs() - t0;
        return;
    }
    closeRun(rs);
    rs->head = NULL; rs->model = NULL; rs->lastW = 0;
    nativeItemFull(it, rs, 1);
    rs->commit = rs->open;
    ++stCommitFull;
    if (g_commitTime) stFullNs += nowNs() - t0;
}
static void processItem(uint32_t it, RunState* rs) {
    ++stFlushItems;
    if (!g_merge) { rs->pending = NULL; nativeItemFull(it, rs, 0); return; }
    uint32_t sig[5]; sigOf(it, sig);
    const uint32_t ck = (sig[0] >> 4) & 511;
    const int run = runnable(it) && !(noCap[ck].key == sig[0] && noCap[ck].skip);
    if (g_commitRun && runnable(it) && (!run || commitFx(sig[0]))) { commitRunItem(it, rs); return; }
    const int cont = rs->open && rs->head && run && !memcmp(sig, rs->head->sig, sizeof sig);
    Model* m = cont ? rs->model : NULL;
    int verifying = 0;
    if (m && m->state == 1) {
        if (++m->nfast & 15) { fastItem(it, rs); ++stFlushFast; return; }
        verifying = 1; ++stFlushVerify;
    } else if (m && m->state == 0) ++stFlushLearn;
    closeRun(rs);
    Head* prev = rs->head;
    Head* nh = prev == &heads[0] ? &heads[1] : &heads[0];
    rs->head = NULL; rs->model = NULL; rs->lastW = 0;
    if (run) nfsCapBegin();
    nativeItemFull(it, rs, run);
    const int n = run ? nfsCapEnd() : -1;
    if (run && rs->open && n >= 0 && buildHead(nh, it, n, sig)) {
        if (rs->pending) { recHead(rs->pending, n, nh); rs->pending = NULL; }
        if (m && m->state >= 0) learn(prev, nh, m, verifying);
        rs->head = nh;
        rs->model = findModel(nh);
        if (rs->model && rs->model->state < 0) {
            rs->model = NULL;
            if (groupModelRejected(nh->vs, nh->ps)) { noCap[ck].key = sig[0]; noCap[ck].skip = 1; }
        }
        if (g_commitRun && groupModelRejected(nh->vs, nh->ps)) addCommitFx(sig[0]);
        rs->lastW = M32(it + 0x24); memcpy(rs->lastWv, nh->W, 64);
    }
}
/*
 * ---- Group replay (NFSU2_GROUP_CACHE, default 1) ----
 * Across groups and frames the D3D calls of a world material group are predictable: its setup
 * (sub_005BC940) and the BeginPass of its first item emit the same non-constant calls every frame
 * (render/sampler/texture-stage states, textures, shaders), and each shader constant register of the
 * world effects is one of
 *   L: W * x  with x the same for every group of that effect in this flush (WVP, WV, projected textures),
 *   G: the same value for every group in this flush (fog, light),
 *   M: the same value for the material in every frame (material colours).
 * Per effect (vs, ps) the class of each register is learned from full groups: the first full group of the
 * effect in a flush is the reference (x = W_ref^-1 r_ref), later full groups are compared with it and with
 * their material's record from an earlier flush. The non-constant calls are classified the same way (G: as
 * in the reference group, e.g. the fog colour; M: as in the record). Classes keep being refined after the
 * effect is trusted (24 observations). A group whose material has a record (and whose items all have the
 * record's inputs, no strips) is then replayed: the material setup's device-visible part (replaySetup), then
 * the recorded calls, constants from
 * the classes, buffers and draws - none of the per-item effect work. Every 16th use of a record runs the
 * full group instead and checks the prediction: a value that fits no class retires the record (animated
 * materials), one that fits another class re-learns it. Cars (per-item lighting) fail the learning and are
 * always drawn fully. Groups whose items were all culled skip the material setup.
 */
static void freeTo(uint32_t poolPtrAddr, uint32_t obj);
#define XMODELS 32
typedef struct {
    uint32_t vs, ps;
    int state; uint32_t nobs, nbad;            /* 0 learning, 1 trusted, -1 rejected */
    uint8_t hyp[2][NREG];                      /* bit0 L, bit1 G, bit2 M */
    uint8_t cls[2][NREG];                      /* chosen class when trusted: 1 L, 2 G, 4 M */
    uint32_t refFlush; uint8_t refHas[2][NREG]; float rref[2][NREG][4]; double xref[2][NREG][4];
    uint8_t chyp[RC_MAX], ccls[RC_MAX];        /* non-constant calls: bit1 G (as the reference's), bit2 M (as the record's) */
    uint32_t unclassifiedMats[4];              /* while learning: materials with a register that fit no class at all */
    MatRec refRec;                             /* the reference head's calls in this flush */
} XModel;
static XModel xmodels[XMODELS];
#define MRECS 1024
static MatRec* mrecs;                          /* open addressing on (slot, mat) */
static uint32_t flushSeq;
static int g_groupCache = -1;
static uint32_t stGroupsReplayed, stGroupsFull, stGroupsEmpty, stGroupsVerify, stRecBad, stRelearn;
static uint32_t stRecTexturesStale, stCacheResetsForTextures;   /* totals: records re-recorded for new textures, caches reset after a texture load */
static uint32_t stInstancedDraws, stInstancedItems;
static XModel* xmodelFor(uint32_t vs, uint32_t ps, int create) {
    for (int i = 0; i < XMODELS; ++i) if (xmodels[i].vs == vs && xmodels[i].ps == ps && (vs || ps)) return &xmodels[i];
    if (!create) return NULL;
    for (int i = 0; i < XMODELS; ++i) if (!xmodels[i].vs && !xmodels[i].ps) {
        XModel* x = &xmodels[i]; memset(x, 0, sizeof *x); x->vs = vs; x->ps = ps; memset(x->hyp, 7, sizeof x->hyp); memset(x->chyp, 6, sizeof x->chyp); return x;
    }
    return NULL;
}
static MatRec* recFor(uint32_t slot, uint32_t mat, int create) {
    if (!mrecs) { mrecs = (MatRec*)calloc(MRECS, sizeof(MatRec)); if (!mrecs) return NULL; }
    uint32_t k = (mat * 0x9e3779b1u) ^ (slot * 0x85ebca6bu);
    for (int p = 0; p < 32; ++p) {
        MatRec* r = &mrecs[(k + (uint32_t)p) & (MRECS - 1)];
        if (r->mat == mat && r->slot == slot && r->valid) return r;
        if (!r->valid) { if (!create) return NULL; memset(r, 0, offsetof(MatRec, calls)); r->mat = mat; r->slot = slot; return r; }
    }
    return NULL;
}
static int groupModelRejected(uint32_t vs, uint32_t ps) {
    const XModel* x = xmodelFor(vs, ps, 0);
    return x && x->state < 0;
}
static void resetCaches(void) {
    memset(noCap, 0, sizeof noCap);
    memset(commitFxList, 0, sizeof commitFxList);
    if (mrecs) memset(mrecs, 0, MRECS * sizeof(MatRec));
    memset(xmodels, 0, sizeof xmodels);
    memset(models, 0, sizeof models);
}
static inline void regValue(const XModel* x, const MatRec* rc, const RReg* q, const float* W, float* out) {
    const uint8_t c = x->cls[q->s][q->reg];
    if (c == 1) mulWx(W, x->xref[q->s][q->reg], out);
    else if (c == 2) memcpy(out, x->rref[q->s][q->reg], 16);
    else memcpy(out, q->v, 16);
    (void)rc;
}
static int callEq(const MatRec* a, const RCall* p, const MatRec* b, const RCall* q) {
    if (p->method != q->method || p->dlen != q->dlen) return 0;
    for (uint32_t k = 0; k + 1 < p->na; ++k) if (k + 1 != p->darg || !p->dlen) { if (p->a[k] != q->a[k]) return 0; }
    if (p->dlen && memcmp(a->data + p->doff, b->data + q->doff, p->dlen)) return 0;
    return 1;
}
/* same calls in the same order with the same state / stage / sampler ids (values may differ) */
static int sameShape(const MatRec* a, const MatRec* b) {
    if (a->ncalls != b->ncalls) return 0;
    for (int i = 0; i < a->ncalls; ++i) {
        const RCall* p = &a->calls[i]; const RCall* q = &b->calls[i];
        if (p->method != q->method) return 0;
        if ((p->method == 57 || p->method == 65 || p->method == 67 || p->method == 69) && p->a[0] != q->a[0]) return 0;
        if ((p->method == 67 || p->method == 69) && p->a[1] != q->a[1]) return 0;
    }
    return 1;
}
static int sameCalls(const MatRec* a, const MatRec* b) {
    if (a->ncalls != b->ncalls || a->nset != b->nset) return 0;
    for (int i = 0; i < a->ncalls; ++i) {
        const RCall* p = &a->calls[i]; const RCall* q = &b->calls[i];
        if (p->method != q->method || p->dlen != q->dlen) return 0;
        for (uint32_t k = 0; k + 1 < p->na; ++k) if (k + 1 != p->darg || !p->dlen) { if (p->a[k] != q->a[k]) return 0; }
        if (p->dlen && memcmp(a->data + p->doff, b->data + q->doff, p->dlen)) return 0;
    }
    return 1;
}
/* While an effect is learning, a register that fits no class at all (not W-linear, not as in the reference, not as
 * in the material's own record) is either one animated material or per-object values (cars: lighting per car).
 * Up to 3 such materials only retire their records; the 4th rejects the effect (its items then take commit runs).
 * Returns 1 = only this material. */
static int noteUnclassifiedMaterial(XModel* effect, uint32_t mat) {
    for (int i = 0; i < 4; ++i) {
        if (effect->unclassifiedMats[i] == mat) return 1;
        if (!effect->unclassifiedMats[i]) { if (i == 3) return 0; effect->unclassifiedMats[i] = mat; return 1; }
    }
    return 0;
}
/* a full group's first item was recorded into pend (W = its matrix): learn / verify, then store */
static void groupLearn(MatRec* pend, const float* W, int verifying, uint32_t g) {
    (void)g;
    XModel* x = xmodelFor(pend->vs, pend->ps, 1);
    MatRec* old = recFor(pend->slot, pend->mat, 0);
    int verificationPassed = 0;
    if (!x) return;
    if (x->refFlush != flushSeq) {
        /* first full group of this effect in this flush: the reference */
        double inv[16];
        if (!invert4(W, inv)) return;
        x->refFlush = flushSeq; memset(x->refHas, 0, sizeof x->refHas);
        memcpy(&x->refRec, pend, sizeof *pend);
        for (int i = 0; i < pend->nregs; ++i) {
            const RReg* q = &pend->regs[i];
            x->refHas[q->s][q->reg] = 1; memcpy(x->rref[q->s][q->reg], q->v, 16);
            for (int r = 0; r < 4; ++r) x->xref[q->s][q->reg][r] = inv[r * 4 + 0] * q->v[0] + inv[r * 4 + 1] * q->v[1] + inv[r * 4 + 2] * q->v[2] + inv[r * 4 + 3] * q->v[3];
        }
    } else if (x->state >= 0 && old && old->valid == 2 && !old->bad && !old->texturesStale && old->flush != flushSeq) {
        /* compare with the reference group (G, L) and with the material's earlier record (M). Hypotheses keep
         * being refined after the effect is trusted; only a value that fits none of them counts against it. */
        const int trusted = x->state == 1;
        int matChanged = !sameShape(old, pend), predFail = 0, relearn = 0, regReject = -1, registerUnclassified = 0;
        const int refShape = sameShape(&x->refRec, pend);
        for (int i = 0; i < pend->ncalls && !matChanged; ++i) {
            const RCall* q = &pend->calls[i];
            uint8_t h = (uint8_t)((refShape && callEq(&x->refRec, &x->refRec.calls[i], pend, q) ? 2 : 0) | (callEq(old, &old->calls[i], pend, q) ? 4 : 0));
            if (!refShape) h |= 2;   /* G not testable here */
            if (verifying && trusted && !(h & x->ccls[i])) {
                predFail = 1;
                static int vshown;
                if (vshown++ < 40) {
                    const RCall* o = &old->calls[i]; const RCall* rf = &x->refRec.calls[i < x->refRec.ncalls ? i : 0];
                    printf("[NFSU2 GROUPS] verify call %d (class %u) material %08x: now m%u %08x %08x %08x, record m%u %08x %08x %08x, reference m%u %08x %08x %08x\n",
                           i, x->ccls[i], pend->mat, q->method, q->a[0], q->a[1], q->a[2], o->method, o->a[0], o->a[1], o->a[2], rf->method, rf->a[0], rf->a[1], rf->a[2]);
                }
            }
            if (x->chyp[i] & h) { if ((x->chyp[i] & h) != x->chyp[i]) relearn = 1; x->chyp[i] &= h; }
            else matChanged = 1;
        }
        for (int i = 0; i < pend->nregs && !matChanged && regReject < 0; ++i) {
            const RReg* q = &pend->regs[i];
            const RReg* oq = NULL;
            for (int j = 0; j < old->nregs; ++j) if (old->regs[j].s == q->s && old->regs[j].reg == q->reg) { oq = &old->regs[j]; break; }
            if (!oq || !x->refHas[q->s][q->reg]) { matChanged = 1; break; }
            float l[4]; mulWx(W, x->xref[q->s][q->reg], l);
            const uint8_t h = (uint8_t)((close4(l, q->v) ? 1 : 0) | (close4(x->rref[q->s][q->reg], q->v) ? 2 : 0) | (close4(oq->v, q->v) ? 4 : 0));
            uint8_t* hy = &x->hyp[q->s][q->reg];
            if (verifying && trusted && !(h & x->cls[q->s][q->reg])) {
                predFail = 1;
                static int rshown;
                if (rshown++ < 40) printf("[NFSU2 GROUPS] verify %c%d (class %u, hits %u) material %08x: now %.4g %.4g %.4g %.4g, record %.4g %.4g %.4g %.4g, reference %.4g %.4g %.4g %.4g, L %.4g %.4g %.4g %.4g\n",
                    q->s ? 'p' : 'c', q->reg, x->cls[q->s][q->reg], h, pend->mat, q->v[0], q->v[1], q->v[2], q->v[3], oq->v[0], oq->v[1], oq->v[2], oq->v[3],
                    x->rref[q->s][q->reg][0], x->rref[q->s][q->reg][1], x->rref[q->s][q->reg][2], x->rref[q->s][q->reg][3], l[0], l[1], l[2], l[3]);
            }
            if (*hy & h) { if ((*hy & h) != *hy) relearn = 1; *hy &= h; }
            else if (!trusted && (h || !noteUnclassifiedMaterial(x, pend->mat))) regReject = q->s * 1000 + q->reg;
            else { matChanged = 1; registerUnclassified = !h; }   /* this material's own value moved (animated, e.g. a scrolling UV offset): retire its record */
        }
        if (regReject >= 0) {
            x->state = -1;
            printf("[NFSU2 GROUPS] effect vs %08x ps %08x rejected: register %c%d fits no class (material %08x)\n", x->vs, x->ps, regReject >= 1000 ? 'p' : 'c', regReject % 1000, pend->mat);
        } else {
            verificationPassed = verifying && !matChanged && !predFail;
            if (matChanged) {
                /* this material's calls or constants moved on their own (animated texture...): retire its record */
                if ((++old->changes >= 3 || verifying) && !old->bad) { old->bad = 1; ++stRecBad; }
                /* trusted while its objects stood still (cars on the grid), then per-object values: the 4th material
                 * whose register fits no class rejects the effect, so its items go back to commit runs */
                if (trusted && ((verifying && ++x->nbad >= 32) || (registerUnclassified && !noteUnclassifiedMaterial(x, pend->mat))))
                    { x->state = -1; printf("[NFSU2 GROUPS] effect vs %08x ps %08x rejected (%u failed verifications)\n", x->vs, x->ps, x->nbad); }
            } else if (predFail) ++stRelearn;
            if (x->state == 0 && !matChanged && ++x->nobs >= 24) { x->state = 1; relearn = 1; printf("[NFSU2 GROUPS] effect vs %08x ps %08x trusted after %u observations\n", x->vs, x->ps, x->nobs); }
            if (x->state == 1 && relearn) {   /* which constant registers follow the world matrix (instancing input) */
                printf("[NFSU2 GROUPS] effect vs %08x: world-matrix registers", x->vs);
                for (int k = 0; k < NREG; ++k) if (x->refHas[0][k] && (x->hyp[0][k] & 1)) printf(" c%d", k);
                printf("; pixel");
                for (int k = 0; k < NREG; ++k) if (x->refHas[1][k] && (x->hyp[1][k] & 1)) printf(" c%d", k);
                printf("\n");
            }
            if (x->state == 1 && relearn) {
                for (int s2 = 0; s2 < 2; ++s2) for (int k = 0; k < NREG; ++k) {
                    const uint8_t h = x->hyp[s2][k];
                    x->cls[s2][k] = (h & 1) ? 1 : (h & 2) ? 2 : 4;
                }
                for (int i = 0; i < RC_MAX; ++i) x->ccls[i] = (x->chyp[i] & 4) ? 4 : 2;
            }
        }
    }
    /* store / refresh the material record */
    MatRec* r = old ? old : recFor(pend->slot, pend->mat, 1);
    if (!r) return;
    const uint32_t uses = old ? old->uses : 0, changes = old ? old->changes : 0; const int bad = old ? old->bad : 0;
    const uint32_t cleanVerifies = !old ? 0 : verificationPassed ? old->cleanVerifies + 1 : verifying ? 0 : old->cleanVerifies;
    memcpy(r, pend, sizeof *r);
    r->uses = uses; r->changes = changes; r->bad = bad; r->flush = flushSeq; r->valid = 2;
    r->cleanVerifies = cleanVerifies;
    r->texturesCheckedAtChurn = currentTextureChurn();   /* just recorded: its texture objects are the current ones */
}
/* send constant registers of a record: all of them (first item) or the L ones only */
static void sendRegs(const XModel* x, const MatRec* rc, const float* W, int lOnly) {
    const uint32_t dev = M32(0x870974);
    int i = 0;
    while (i < rc->nregs) {
        const RReg* q = &rc->regs[i];
        if (lOnly && x->cls[q->s][q->reg] != 1) { ++i; continue; }
        int e = i + 1;
        while (e < rc->nregs && rc->regs[e].s == q->s && rc->regs[e].reg == rc->regs[e - 1].reg + 1 && (!lOnly || x->cls[rc->regs[e].s][rc->regs[e].reg] == 1)) ++e;
        const uint32_t cnt = (uint32_t)(e - i);
        g_esp -= cnt * 16; const uint32_t buf = g_esp;
        float* out = (float*)ADDR(buf);
        for (int j = i; j < e; ++j) regValue(x, rc, &rc->regs[j], W, out + 4 * (j - i));
        const uint32_t a[4] = {dev, q->reg, buf, cnt};
        uint32_t res;
        if (!nfsD3dDeviceCall(q->s ? 109 : 94, a, &res)) devMethod(q->s ? 109 : 94, 4, a);
        g_esp += cnt * 16;
        i = e;
    }
}
/* Texture objects created or retired so far. Test switch NFSU2_TEST_TEXTURE_CHURN=<frame>[:<count>|:stale]: from
 * that frame on, <count> (default 1) extra changes are reported (1: every record re-checks its textures; >64: the
 * caches reset), or (stale) every record's textures count as replaced once. */
static int testStaleFrame = -1;
static uint32_t currentTextureChurn(void) {
    static int testFrame = -1; static uint32_t testCount;
    if (testFrame == -1) {
        const char* e = getenv("NFSU2_TEST_TEXTURE_CHURN");
        const char* colon = e ? strchr(e, ':') : NULL;
        testFrame = e ? atoi(e) : -2;
        testCount = colon && strcmp(colon + 1, "stale") ? (uint32_t)atoi(colon + 1) : 1;
        if (colon && !strcmp(colon + 1, "stale")) testStaleFrame = testFrame;
    }
    uint32_t churn = nfsD3dTextureChurn();
    if (testFrame >= 0 && getD3D9PresentCount() >= (uint32_t)testFrame) churn += testCount;
    return churn;
}
/* Every SetTexture of the record still binds the texture object that was recorded (not a new object at a
 * reused handle, nor a released one). Called only when textures were created or retired since the last check. */
static int recordTexturesCurrent(const MatRec* record) {
    if (testStaleFrame >= 0 && record->texturesCheckedAtChurn == nfsD3dTextureChurn()) return 0;   /* test: first check after the frame */
    for (int i = 0; i < record->ncalls; ++i) {
        const RCall* call = &record->calls[i];
        if (call->method == 65 && call->a[1] && nfsD3dObjectSerial(call->a[1]) != call->textureSerial) return 0;
    }
    return 1;
}
/* Verify every 16th use of a record; one that keeps passing is verified every 32nd, then every 64th use. */
static uint32_t verifyInterval(const MatRec* record) {
    return record->cleanVerifies >= 8 ? 64u : record->cleanVerifies >= 4 ? 32u : 16u;
}
/* 1 = replay group g from its material's record (*effectOut = the trusted effect model). 0 = draw it fully;
 * *verify = 1 when that full draw is this record's periodic check. */
static int canReplay(MatRec* record, uint32_t group, XModel** effectOut, int* verify) {
    *verify = 0;
    if (!record || record->valid != 2 || record->bad || record->texturesStale) return 0;
    XModel* effect = xmodelFor(record->vs, record->ps, 0);
    if (!effect || effect->state != 1 || effect->refFlush != flushSeq) return 0;
    if (!sameShape(&effect->refRec, record)) {
        /* calls taken from this flush's reference group need the same call layout */
        for (int i = 0; i < record->ncalls; ++i) if (effect->ccls[i] == 2) return 0;
    }
    const uint32_t itemsHead = group + 0x18;
    for (uint32_t item = M32(itemsHead); item != itemsHead; item = M32(item)) {
        if (!validGuest(item) || !runnable(item)) return 0;
        uint32_t itemSig[5]; sigOf(item, itemSig);
        if (memcmp(itemSig, record->sig, sizeof itemSig)) return 0;
        const uint32_t solid = M32(M32(item + 0x10));
        if (!validGuest(solid) || !M32(solid + 0x2c)) return 0;   /* vertex buffer not created yet: full path */
    }
    const uint32_t textureChurn = currentTextureChurn();
    if (record->texturesCheckedAtChurn != textureChurn) {
        if (!recordTexturesCurrent(record)) { record->texturesStale = 1; ++stRecTexturesStale; return 0; }
        record->texturesCheckedAtChurn = textureChurn;
    }
    if (++record->uses % verifyInterval(record) == 0) { *verify = 1; return 0; }
    *effectOut = effect;
    return 1;
}
/*
 * Material setup for a replayed group (sub_005BC940 with a shader slot set). Its only device calls are the
 * cull / z-write render states; everything else feeds the effect (texture, the alpha/blend values at
 * 0x86EAD8.., the UV transform), which a replayed group does not use: the effect's pass output is replayed.
 * Every material sets those parameters again before its own BeginPass, so skipping them here leaves later
 * full groups unchanged. On the device the effect SetTexture alone (AddRef/Release of the texture for every
 * effect sharing the parameter) was ~60% of sub_005BC940, ~3 ms per frame. Returns 0 = run the original.
 */
static int replaySetup(uint32_t mat) {
    const uint32_t shader = M32(0x870910);
    if (!validGuest(shader) || !validGuest(mat)) return 0;
    if (mat == M32(0x870940) && M32(0x870944) == shader) return 1;
    /* The effect parameters are not set here, so the game's "current material" must not claim this one:
     * a following full group with the same material would skip its setup and draw with the previous full
     * group's texture and blend values. 0 = no current material. */
    M32(0x872F10) += 1; M32(0x870940) = 0; M32(0x870944) = shader;
    if (!M32(M32(mat) + 0x18)) return 1;
    /* sub_005BC880 / sub_005BC8D0 with a shader: the alpha test and blend values for the effect */
    const int at = M8(mat + 0x55) == 1;
    M32(0x86EAD8) = at ? 1 : 0; M32(0x86EADC) = at ? 0x5F : 0;
    const int8_t bm = (int8_t)M8(mat + 0x56);
    uint32_t be = 1, bs = 5, bd = 6;
    switch (bm) {
        case 0: be = 0; bs = 5; bd = 6; break;
        case 2: bd = 2; break;
        case 5: bs = 7; bd = 8; break;
        case 6: bs = 7; bd = 2; break;
        default: break;   /* 1, 3, 4 and out of range: 5, 6 */
    }
    M32(0x86EAE0) = be; M32(0x86EAE4) = bs; M32(0x86EAE8) = bd;
    /* cull and z-write, straight to the device */
    const uint32_t dev = M32(0x870974);
    const uint32_t stype = M32(shader + 4);
    if (!bm) {
        if (stype == 5) { const uint32_t a[3] = {dev, 0x16, 1}; devFast(57, 3, a); }
        const uint32_t a[3] = {dev, 0xE, 1}; devFast(57, 3, a);
    } else if (stype == 5) {
        { const uint32_t a[3] = {dev, 0x16, 2}; devFast(57, 3, a); }
        const uint32_t a[3] = {dev, 0xE, 0}; devFast(57, 3, a);
    } else { const uint32_t a[3] = {dev, 0xE, 0}; devFast(57, 3, a); }
    if (M32(M32(shader + 0x1c) + 0x8bc)) M32(0x870948) = 1;
    if (!M8(mat + 0x52)) M32(0x870948) = 0;
    return 1;
}
/* ---- Instanced replay (NFSU2_INSTANCING, default 1) ----
 * Consecutive items of a replayed group that draw the same mesh range (lamp posts, barriers, trees) differ only
 * in their world matrix, i.e. in the effect's L registers. They go to the bridge as one instanced draw with
 * W * x of each item (d3d9_gles: an instanced shader variant reads those registers per instance). Effects
 * with a pixel-shader L register are not instanced. */
#define INSTANCE_MAX 64
#define INSTANCE_REGS 16
static int instancingEnabled(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("NFSU2_INSTANCING"); on = !(e && e[0] == '0'); }
    return on;
}
static inline void unlinkItem(uint32_t it) { const uint32_t n = M32(it), pv = M32(it + 4); M32(pv) = n; M32(n + 4) = pv; }
/* the vertex-shader registers of a record that follow the world matrix; 0: none, too many, or a pixel-shader one */
static int worldMatrixRegisters(const XModel* x, const MatRec* rc, uint16_t* regs) {
    int n = 0;
    for (int i = 0; i < rc->nregs; ++i) {
        const RReg* q = &rc->regs[i];
        if (x->cls[q->s][q->reg] != 1) continue;
        if (q->s) return 0;
        int dup = 0; for (int j = 0; j < n; ++j) dup |= regs[j] == q->reg;
        if (dup) continue;
        if (n == INSTANCE_REGS) return 0;
        regs[n++] = q->reg;
    }
    return n;
}
/* `it` (unlinked, buffers bound) and the following items of group g with its mesh range as one instanced draw.
 * Returns the last item drawn (its W is now in the registers), 0 = no run: draw `it` alone (*refused: the bridge
 * did not take it, e.g. render thread off or a recording running). */
static uint32_t drawInstancedRun(const XModel* x, uint32_t g, uint32_t it, const uint16_t* regs, int nregs, int* refused) {
    const uint32_t solid = M32(M32(it + 0x10)), strip = M32(it + 8);
    uint32_t items[INSTANCE_MAX]; uint32_t n = 1; items[0] = it;
    for (uint32_t next = M32(g + 0x18); next != g + 0x18 && n < INSTANCE_MAX; next = M32(next)) {
        const uint32_t s2 = M32(next + 8);
        if (M32(M32(next + 0x10)) != solid || M32(s2 + 0x34) != M32(strip + 0x34) || M32(s2 + 0xC) != M32(strip + 0xC)) break;
        items[n++] = next;
    }
    if (n < 2) return 0;
    float data[INSTANCE_MAX * INSTANCE_REGS * 4];
    for (uint32_t i = 0; i < n; ++i) {
        const float* W = (const float*)ADDR(M32(items[i] + 0x24));
        for (int j = 0; j < nregs; ++j) mulWx(W, x->xref[0][regs[j]], data + (i * (uint32_t)nregs + (uint32_t)j) * 4);
    }
    /* drawItem's DrawIndexedPrimitive: triangle list, base 0, min 0, vertex count, start index, primitives */
    const uint32_t dip[6] = {4, 0, 0, M32(solid + 0x34), (uint32_t)((int32_t)(M32(strip + 0x34) - M32(solid + 0x18)) >> 1), M32(strip + 0xC) / 3};
    if (!nfsD3dDrawInstanced(dip, (uint32_t)nregs, regs, n, data)) { *refused = 1; return 0; }
    for (uint32_t i = 1; i < n; ++i) {
        unlinkItem(items[i]); countItem(items[i]);
        ++stFlushItems; ++stFlushFast;
        freeTo(0x87091C, items[i]);
    }
    ++stInstancedDraws; stInstancedItems += n;
    return items[n - 1];
}
static void replayGroup(MatRec* rc, XModel* x, uint32_t g) {
    const uint32_t dev = M32(0x870974);
    for (int i = 0; i < rc->ncalls; ++i) {
        const MatRec* src = x->ccls[i] == 2 ? &x->refRec : rc;   /* shape checked in canReplay */
        const RCall* c = &src->calls[i];
        uint32_t a[9] = {0};
        a[0] = dev;
        for (uint32_t k = 1; k < c->na; ++k) a[k] = c->a[k - 1];
        uint32_t room = 0;
        if (c->dlen) { room = (c->dlen + 15) & ~15u; g_esp -= room; memcpy((void*)ADDR(g_esp), src->data + c->doff, c->dlen); a[c->darg] = g_esp; }
        if (!nfsD3dCall(2, c->method, a)) devMethod(c->method, (int)c->na, a);
        g_esp += room;
    }
    int first = 1, instancing = instancingEnabled(); uint32_t lastW = 0;
    uint16_t instanceRegs[INSTANCE_REGS]; int nInstanceRegs = -1;   /* the record's world-matrix registers, on first use */
    while (M32(g + 0x18) != g + 0x18) {
        const uint32_t it = M32(g + 0x18);
        unlinkItem(it);
        const uint32_t wp = M32(it + 0x24);
        if (first || wp != lastW) sendRegs(x, rc, (const float*)ADDR(wp), !first);
        first = 0; lastW = wp;
        if (bindBuffers(it, 1)) {
            if (instancing && nInstanceRegs < 0) nInstanceRegs = worldMatrixRegisters(x, rc, instanceRegs);
            int refused = 0;
            const uint32_t lastOfRun = instancing && nInstanceRegs > 0 ? drawInstancedRun(x, g, it, instanceRegs, nInstanceRegs, &refused) : 0;
            if (refused) instancing = 0;   /* not for the rest of the group either */
            if (lastOfRun) lastW = M32(lastOfRun + 0x24);
            else drawItem(it, 1);
            countItem(it);
        }
        ++stFlushItems; ++stFlushFast;
        freeTo(0x87091C, it);
    }
}
static int traceFrames[8], nTraceFrames = -1;
static int isTraceFrame(void) {
    if (nTraceFrames < 0) {
        nTraceFrames = 0;
        const char* e = getenv("NFSU2_ITEM_TRACE");
        while (e && *e && nTraceFrames < 8) { traceFrames[nTraceFrames++] = atoi(e); e = strchr(e, ','); if (e) ++e; }
    }
    const int now = (int)getD3D9PresentCount();
    for (int i = 0; i < nTraceFrames; ++i) if (traceFrames[i] == now) return 1;
    return 0;
}
static void printCalls(int n) {
    const CapRec* r = (const CapRec*)nfsCapRecs();
    const uint8_t* bytes = nfsCapBytes();
    for (int i = 0; i < n && i < 120; ++i) {
        printf("   m%u %08x %08x %08x %08x", r[i].method, r[i].a[1], r[i].a[2], r[i].a[3], r[i].a[4]);
        if (r[i].len[0]) {
            uint32_t h = 2166136261u; for (uint32_t k = 0; k < r[i].len[0]; ++k) h = (h ^ bytes[r[i].off[0] + k]) * 16777619u;
            printf(" data %u bytes hash %08x", r[i].len[0], h);
            if (r[i].method == 94 || r[i].method == 109) { const float* f = (const float*)(bytes + r[i].off[0]); printf(" ["); for (uint32_t k = 0; k < r[i].len[0] / 4; ++k) printf(" %.6g", f[k]); printf("]"); }
        }
        printf("\n");
    }
}
static void traceItem(uint32_t it, uint32_t g, int n) {
    printf("[ITEM] g=%08x it=%08x mat=%08x key=%08x fx=%08x npass=%u solid=%08x calls=%d slot=%u\n", g, it, M32(it + 0x24), M32(it + 0x18),
           M32(M32(it + 0x18) + 0x20), M32(M32(it + 0x18) + 0x14), validGuest(M32(it + 0x10)) ? M32(M32(it + 0x10)) : 0u, n, (M32(0x870910) ? 0u : 0u));
    if (validGuest(M32(it + 0x24))) { const float* W = (const float*)ADDR(M32(it + 0x24)); printf("   W"); for (int k = 0; k < 16; ++k) printf(" %.6g", W[k]); printf("\n"); }
    if (M32(M32(0x870848) + 4) < 64 && validGuest(M32(0x832DE0 + M32(M32(0x870848) + 4) * 0x70))) { const uint32_t view = M32(M32(0x870848) + 4); const float* V = (const float*)ADDR(M32(0x832DE0 + view * 0x70)); const float* P = (const float*)ADDR(M32(0x832DE0 + view * 0x70) + 0x40);
      printf("   V"); for (int k = 0; k < 16; ++k) printf(" %.6g", V[k]); printf("\n   P"); for (int k = 0; k < 16; ++k) printf(" %.6g", P[k]); printf("\n");
      printf("   viewrec"); for (int k = 0; k < 28; ++k) printf(" %08x", M32(0x832DE0 + view * 0x70 + 4 * k)); printf("\n"); }
    printCalls(n);
    { uint32_t sg[5]; sigOf(it, sg); printf("   sig %08x %08x %08x %08x %08x strip %08x\n", sg[0], sg[1], sg[2], sg[3], sg[4], M32(it + 8)); }
}
static void freeTo(uint32_t poolPtrAddr, uint32_t obj) {  /* sub_0043F970(pool, obj) */
    const uint32_t pool = M32(poolPtrAddr);
    M32(obj) = M32(pool + 0x10); M32(pool + 0x18) -= 1; M32(pool + 0x10) = obj;
}
static void nativeFlush(void) {
    M32(0x872EDC) += 1;
    for (uint32_t b = 0; b < 368; ++b) {
        const uint32_t head = 0x870D30u + 8 * b;
        if (M32(head) == head) continue;
        const uint32_t shader = M32(0x86EB28u + 4 * (b % 23));
        M32(0x870910) = shader;
        icall(M32(M32(shader) + 4), shader, 0, 0);
        { const uint32_t dev = M32(0x870974); const uint32_t a[2] = {dev, M32(shader + 0x24)}; devMethod(87, 2, a); }
        {
            const uint32_t fx = M32(shader + 0x20);
            push32(0); const uint32_t passesSlot = g_esp;
            const uint32_t a[3] = {fx, passesSlot, 0};
            fxMethod(fx, 65, 3, a);
            g_esp += 4;
        }
        M32(0x872F04) += 1;
        /* a commit run can continue into the next group of the same effect (NFSU2_COMMIT_CROSS) */
        RunState carry = {0};
        while (M32(head) != head) {
            RunState rs = {0};
            const uint32_t g = M32(head);
            { const uint32_t n = M32(g), pv = M32(g + 4); M32(pv) = n; M32(n + 4) = pv; }
            M32(0x872ED8) += 1;
            const uint32_t slot = b % 23, mat = M32(g + 0x10);
            if (M32(g + 0x18) == g + 0x18) {
                /* every item was culled by distance: no material setup for nothing */
                ++stGroupsEmpty;
                M32(M32(mat) + 0x1c) = 0;
                push32(RECOMP_RETADDR); g_ecx = g; sub_005CE750();
                freeTo(0x870918, g);
                continue;
            }
            const int tracing = isTraceFrame();
            if (carry.open) {
                const uint32_t fi = M32(g + 0x18);
                if (g_commitCross && !tracing && validGuest(fi) && runnable(fi) && commitFx(M32(fi + 0x18))
                    && M32(M32(fi + 0x18) + 0x20) == carry.fx && itemPasses(fi) == 1) { rs = carry; carry.open = 0; carry.commit = 0; ++stCommitCross; }
                else closeRun(&carry);
            }
            XModel* xm = NULL; int verify = 0;
            MatRec* rc = (g_groupCache && g_merge && !tracing) ? recFor(slot, mat, 0) : NULL;
            if (rc && canReplay(rc, g, &xm, &verify)) {
                closeRun(&rs);
                /* the material setup: device-visible part only (NFSU2_REPLAY_SETUP=0: the game's) */
                static int nativeSetup = -1;
                if (nativeSetup < 0) { const char* e = getenv("NFSU2_REPLAY_SETUP"); nativeSetup = e ? atoi(e) : 1; }
                if (!nativeSetup || !replaySetup(mat)) { const uint32_t a[1] = {mat}; ccall(sub_005BC940, 1, a); }
                replayGroup(rc, xm, g);
                ++stGroupsReplayed;
            } else {
                static MatRec pend;
                int rec = g_groupCache && g_merge && !tracing && validGuest(M32(g + 0x18)) && runnable(M32(g + 0x18));
                if (verify) ++stGroupsVerify;
                ++stGroupsFull;
                if (tracing) nfsCapBegin();
                { const uint32_t a[1] = {mat}; ccall(sub_005BC940, 1, a); }
                if (tracing) { const int n = nfsCapEnd(); printf("[GROUP] frame %u slot %u g=%08x mat=%08x calls=%d\n", getD3D9PresentCount(), slot, g, mat, n); printCalls(n); }
                if (rec) {
                    memset(&pend, 0, offsetof(MatRec, calls));
                    pend.mat = mat; pend.slot = slot; pend.valid = 1; pend.nset = 0;
                    rs.pending = &pend;
                }
                const uint32_t firstIt = M32(g + 0x18);
                float W0[16]; if (rec) memcpy(W0, (const void*)ADDR(M32(firstIt + 0x24)), 64);
                int firstDone = 0;
                while (M32(g + 0x18) != g + 0x18) {
                    const uint32_t it = M32(g + 0x18);
                    { const uint32_t n = M32(it), pv = M32(it + 4); M32(pv) = n; M32(n + 4) = pv; }
                    if (tracing) { closeRun(&rs); rs.head = NULL; nfsCapBegin(); nativeItemFull(it, &rs, 0); const int n = nfsCapEnd(); traceItem(it, g, n); }
                    else processItem(it, &rs);
                    if (!firstDone) {
                        firstDone = 1;
                        if (rec && pend.valid == 2) groupLearn(&pend, W0, verify, g);
                        rs.pending = NULL;
                    }
                    freeTo(0x87091C, it);
                }
            }
            if (rs.open && rs.commit && g_commitCross) { carry = rs; rs.open = 0; } else closeRun(&rs);
            M32(M32(M32(g + 0x10)) + 0x1c) = 0;
            push32(RECOMP_RETADDR); g_ecx = g; sub_005CE750();  /* thiscall, no stack args */
            freeTo(0x870918, g);
        }
        closeRun(&carry);
        icall(M32(M32(shader) + 8), shader, 0, 0);
        { const uint32_t fx = M32(shader + 0x20); const uint32_t a[1] = {fx}; fxMethod(fx, 69, 1, a); }
        M32(0x870910) = 0;
    }
    M32(0x87094C) = 0;
}
static int g_nativeFlush = -1;
void sub_005CF150(void) {
    if (drawDist < 0) { const char* e = getenv("NFSU2_DRAW_DIST"); drawDist = e ? (float)atof(e) : 0; }
    const uint32_t viewRec = M32(0x870848);
    const uint32_t viewId = validGuest(viewRec) ? M32(viewRec + 4) : 0xffffffffu;
    const uint32_t vm = viewId < 64 ? M32(0x832DE0 + viewId * 0x70) : 0;
    if (validGuest(vm)) {
        const float* V = (const float*)ADDR(vm);
        /* camera = -t * R^T for a row-vector view matrix with rotation R (rows 0-2) and translation t (row 3) */
        float cam[3];
        for (int j = 0; j < 3; ++j) cam[j] = -(V[12] * V[j * 4 + 0] + V[13] * V[j * 4 + 1] + V[14] * V[j * 4 + 2]);
        const uint32_t itemPool = M32(0x87091C);
        /* how adjacent items differ: diagnostics only, sampled on every 8th frame (scaled when printed) */
        const int sampleAdjacentStats = (getD3D9PresentCount() & 7u) == 0;
        for (uint32_t b = 0; b < 368; ++b) {  /* 16 layers x 23 shader slots */
            const uint32_t s = b % 23, head = 0x870D30u + b * 8;
            for (uint32_t g = M32(head); g != head && validGuest(g); g = M32(g)) {
                ++slotGroups[s];
                if (sampleAdjacentStats) {
                    uint32_t prev = 0;
                    for (uint32_t it = M32(g + 0x18); it != g + 0x18 && validGuest(it); it = M32(it)) {
                        ++slotItems[s];
                        if (M32(it + 0x24) == 0x872E90u) ++stDiffIdent;
                        if (prev) {
                            static const int offs[7] = {0x24, 0x18, 0x1c, 0x20, 0x14, 0x28, 0xC};
                            int nd = 0;
                            for (int k = 0; k < 7; ++k) if (M32(it + offs[k]) != M32(prev + offs[k])) { ++stDiff[k]; ++nd; }
                            if (!nd) ++stDiff[7];
                            if (validGuest(M32(it + 0x10)) && M32(it + 0x10) == M32(prev + 0x10)) ++stDiff[8];
                            /* same matrix contents although a different pointer */
                            if (M32(it + 0x24) != M32(prev + 0x24) && validGuest(M32(it + 0x24)) && validGuest(M32(prev + 0x24)) &&
                                !memcmp((const void*)ADDR(M32(it + 0x24)), (const void*)ADDR(M32(prev + 0x24)), 64)) ++stDiff[9];
                        }
                        prev = it;
                    }
                }
                const uint32_t ihead = g + 0x18;
                for (uint32_t it = M32(ihead), next; it != ihead && validGuest(it); it = next) {
                    next = M32(it);
                    ++stItems;
                    const uint32_t mesh = M32(it + 0x10), mat = M32(it + 0x24);
                    const uint32_t solid = validGuest(mesh) ? M32(mesh) : 0;
                    if (!validGuest(solid) || !validGuest(mat)) { ++stNoBound; continue; }
                    const Bound* b = solidBound(solid);
                    if (!b->ok) { ++stNoBound; continue; }
                    const float* W = (const float*)ADDR(mat);
                    float c[3], sc = 0;
                    for (int j = 0; j < 3; ++j) c[j] = b->c[0] * W[0 * 4 + j] + b->c[1] * W[1 * 4 + j] + b->c[2] * W[2 * 4 + j] + W[12 + j];
                    for (int i = 0; i < 3; ++i) { const float l = W[i * 4] * W[i * 4] + W[i * 4 + 1] * W[i * 4 + 1] + W[i * 4 + 2] * W[i * 4 + 2]; if (l > sc) sc = l; }
                    const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
                    const float near = sqrtf(dx * dx + dy * dy + dz * dz) - b->r * sqrtf(sc);
                    const int bin = near < 100 ? 0 : near < 150 ? 1 : near < 200 ? 2 : near < 250 ? 3 : near < 300 ? 4 : near < 400 ? 5 : near < 600 ? 6 : 7;
                    ++stHist[bin];
                    if (bin == 7) { static int shown; if (shown++ < 12) printf("[NFSU2 CULL] far item: slot %u near %.0f centre %.0f radius %.0f scale %.2f identity %d solid %08x verts %u fx %08x view %u\n",
                        s, near, sqrtf(dx * dx + dy * dy + dz * dz), b->r, sqrtf(sc), mat == 0x872E90u, solid, M32(solid + 0x34), M32(M32(it + 0x18) + 0x20), viewId); }
                    if (drawDist > 0 && near > drawDist && validGuest(itemPool)) {
                        /* unlink and give back to the item pool (sub_0043F970) */
                        const uint32_t prev = M32(it + 4);
                        M32(prev) = next; M32(next + 4) = prev;
                        M32(it) = M32(itemPool + 0x10); M32(itemPool + 0x10) = it; M32(itemPool + 0x18) -= 1;
                        ++stCulled;
                    }
                }
            }
        }
    }
    const uint32_t now = getD3D9PresentCount();
    if (now != stFrame) {
        stFrame = now;
        if (++stFrames % 240 == 0 && stItems) {
            printf("[NFSU2 CULL] items/frame %u (no bound %u), nearest surface distance: <100m %u, <150 %u, <200 %u, <250 %u, <300 %u, <400 %u, <600 %u, more %u; culled %u (draw distance %.0f m)\n",
                   stItems / 240, stNoBound / 240, stHist[0] / 240, stHist[1] / 240, stHist[2] / 240, stHist[3] / 240, stHist[4] / 240, stHist[5] / 240, stHist[6] / 240, stHist[7] / 240, stCulled / 240, drawDist);
            printf("[NFSU2 CULL] per slot items/groups:");
            for (int k = 0; k < 23; ++k) if (slotItems[k]) printf(" s%d=%u/%u", k, slotItems[k] / 240, slotGroups[k] / 240);
            printf("\n");
            { uint32_t tr = 0, le = 0, rj = 0; for (int k = 0; k < MODELS; ++k) if (models[k].fx) { if (models[k].state > 0) ++tr; else if (models[k].state < 0) ++rj; else ++le; }
            printf("[NFSU2 FLUSH] native flushes %u, lifted %u; items/frame %u: fast %u, learning %u, verified %u (merge %d); models trusted %u, learning %u, rejected %u\n",
                   stFlushNative, stFlushLifted, stFlushItems / 240, stFlushFast / 240, stFlushLearn / 240, stFlushVerify / 240, g_merge, tr, le, rj); }
            printf("[NFSU2 FLUSH] adjacent items differ in: matrix %u, fxinfo %u, +1c %u, +20 %u, flags %u, +28 %u, strip %u; identical %u; same solid %u; equal matrix contents %u; identity matrix %u\n",
                   stDiff[0] / 30, stDiff[1] / 30, stDiff[2] / 30, stDiff[3] / 30, stDiff[4] / 30, stDiff[5] / 30, stDiff[6] / 30, stDiff[7] / 30, stDiff[8] / 30, stDiff[9] / 30, stDiffIdent / 30);   /* sampled 1 frame in 8: 240/8 */
            memset(stDiff, 0, sizeof stDiff); stDiffIdent = 0;
            { uint32_t tr = 0, le = 0, rj = 0; for (int k = 0; k < XMODELS; ++k) if (xmodels[k].vs || xmodels[k].ps) { if (xmodels[k].state > 0) ++tr; else if (xmodels[k].state < 0) ++rj; else ++le; }
              printf("[NFSU2 GROUPS] groups/frame: replayed %u, full %u (verifying %u), empty skipped %u; records retired %u, relearned %u, re-recorded for new textures %u; caches reset for texture loads %u; effects trusted %u, learning %u, rejected %u (cache %d)\n",
                     stGroupsReplayed / 240, stGroupsFull / 240, stGroupsVerify / 240, stGroupsEmpty / 240, stRecBad, stRelearn, stRecTexturesStale, stCacheResetsForTextures, tr, le, rj, g_groupCache); }
            printf("[NFSU2 GROUPS] instanced draws/frame %u covering %u replayed items (NFSU2_INSTANCING %d)\n", stInstancedDraws / 240, stInstancedItems / 240, instancingEnabled());
            stInstancedDraws = stInstancedItems = 0;
            stGroupsReplayed = stGroupsFull = stGroupsVerify = stGroupsEmpty = 0;
            printf("[NFSU2 COMMIT] items/frame: in an open pass %u, opening %u, groups continuing a run %u (mode %d, cross %d)", stCommitItems / 240, stCommitFull / 240, stCommitCross / 240, g_commitRun, g_commitCross);
            if (g_commitTime && stCommitItems && stCommitFull) printf("; %.1f us per committed item, %.1f us per full item", stCommitNs / 1000.0 / stCommitItems, stFullNs / 1000.0 / stCommitFull);
            if (g_commitRun == 2) printf("; verified %u, missed device states %u", stCommitChecked, stCommitMissed);
            printf("\n");
            stCommitItems = stCommitFull = stCommitCross = 0; stCommitNs = stFullNs = 0;
            stFlushNative = stFlushLifted = stFlushItems = stFlushFast = stFlushLearn = stFlushVerify = 0;
            memset(slotItems, 0, sizeof slotItems); memset(slotGroups, 0, sizeof slotGroups);
            stItems = stCulled = stNoBound = 0; memset(stHist, 0, sizeof stHist);
        }
    }
    if (g_nativeFlush < 0) { const char* e = getenv("NFSU2_NATIVE_FLUSH"); g_nativeFlush = e ? atoi(e) : 1; }
    if (g_merge < 0) { const char* e = getenv("NFSU2_FLUSH_MERGE"); g_merge = e ? atoi(e) : 1; }
    if (g_commitRun < 0) { const char* e = getenv("NFSU2_COMMIT_RUN"); g_commitRun = e ? atoi(e) : 1; }
    if (g_commitCross < 0) { const char* e = getenv("NFSU2_COMMIT_CROSS"); g_commitCross = g_commitRun && !(e && e[0] == '0'); }
    if (g_commitTime < 0) { const char* e = getenv("NFSU2_COMMIT_TIME"); g_commitTime = e && e[0] == '1'; }
    if (g_nativeFlush && !flushNeedsLifted()) {
        const uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
        ++stFlushNative;
        {
            static uint32_t lastFlushFrame;
            const uint32_t nowF = getD3D9PresentCount();
            if (g_groupCache < 0) { const char* e = getenv("NFSU2_GROUP_CACHE"); g_groupCache = e ? atoi(e) : 1; }
            if (nowF > lastFlushFrame + 10 || nowF < lastFlushFrame) resetCaches();   /* loading screens: objects may be gone */
            lastFlushFrame = nowF;
            /* A world section (re)loaded its textures during play (~500 at once on the device): the learned records
             * and effect models describe the old objects. Relearning (~1 s) beats checking each record, and frees
             * the record and model tables for the new materials. A few textures (streaming) are caught per record. */
            {
                static uint32_t textureChurnAtLastFlush;
                const uint32_t textureChurn = currentTextureChurn();
                if (textureChurn - textureChurnAtLastFlush > 64u) {
                    resetCaches(); ++stCacheResetsForTextures;
                    printf("[NFSU2 GROUPS] frame %u: %u textures created or released since the last flush, learned records reset\n", nowF, textureChurn - textureChurnAtLastFlush);
                }
                textureChurnAtLastFlush = textureChurn;
            }
            /* Flushes of the same view (view + projection matrices) in the same frame share the group-replay
             * reference (NFSU2_REF_PER_VIEW, default 1): the game flushes ~11 times per frame. */
            static int perView = -1;
            if (perView < 0) { const char* e = getenv("NFSU2_REF_PER_VIEW"); perView = e ? atoi(e) : 1; }
            uint32_t vk = 2166136261u;
            if (validGuest(vm)) { const uint32_t* w = (const uint32_t*)ADDR(vm); for (int k = 0; k < 32; ++k) vk = (vk ^ w[k]) * 16777619u; }
            vk = (vk ^ viewId) * 16777619u; vk = (vk ^ nowF) * 16777619u;
            if (perView && validGuest(vm)) flushSeq = vk | 1u; else flushSeq = (flushSeq + 2) & ~1u;
            if (isTraceFrame() || (nowF % 600) == 0) {
                uint32_t ng = 0, ni = 0;
                for (uint32_t b2 = 0; b2 < 368; ++b2) { const uint32_t h2 = 0x870D30u + 8 * b2;
                    for (uint32_t g2 = M32(h2); g2 != h2 && validGuest(g2); g2 = M32(g2)) { ++ng; for (uint32_t i2 = M32(g2 + 0x18); i2 != g2 + 0x18 && validGuest(i2); i2 = M32(i2)) ++ni; } }
                printf("[NFSU2 FLUSHES] frame %u view %u key %08x groups %u items %u\n", nowF, viewId, vk, ng, ni);
            }
        }
        nativeFlush();
        g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
        g_esp += 4;  /* ret */
        return;
    }
    ++stFlushLifted;
    sub_005CF150_orig();
}

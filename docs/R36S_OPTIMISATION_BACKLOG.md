# R36S optimisation backlog (2026-10-22, after v35)

The CPU is still the limit. Device (10-21 log): game thread 45–50 ms/frame by /proc (~25 ms/frame in the short
v32 session), render thread 25–29 ms/frame, of which 16–20 ms are the Mali driver. v34 cut world draws by a
third (VM: 704 → 466 per frame), so the game thread is now the clearer bottleneck. Budget at 30 fps: 33 ms per
thread.

Scores are estimates (0–10) from the VM profile (busy game-thread samples) and the 10-21 device log, not
measurements. Percentages are shares of the game thread's busy time unless noted. `#` is the item's original
number (kept so older notes still match).

## Remaining, ordered by expected performance gain

Ties are broken by the expected saving, then by the thread: game-thread items first, since that thread is the
bottleneck.

| Rank | # | Optimisation | Thread | Expected saving | Gain (0–10) | Complexity (0–10) | Notes |
|---|---|---|---|---|---|---|---|
| 1 | 13 | Car reflection map updated every other frame | Game + Render | 5–10% | 6 | 3 | ⚠️ Visual trade-off: needs the user's approval |
| 2 | 14 | Lower physics or AI rate for distant cars | Game | 5–10% | 6 | 5 | ⚠️ Gameplay trade-off: needs the user's approval |
| 3 | 11 | Fewer GL calls per draw (VAOs, batched constant uploads; ~22 µs driver time per draw) | Render | 10–20% of driver time | 6 | 5 | The ES 3.2 context v34 uses for instancing also has VAOs; worth less now that there are a third fewer draws |
| 4 | 3 | Replay or commit runs for the second render path (`sub_005B9940`, 7%) | Game | up to ~5% | 5 | 6 | Now known: it walks the scenery tree (`sub_005E9730` → `sub_005D6700`, native since v35) and sets up each object (`sub_005D62C0`) |
| 5 | 17 | ThinLTO across the lifted files (clang): small lifted functions inlined into callers in other files | All lifted code | 0–8% | 4 | 6 | The profile is flat (hundreds of small functions), which is what cross-file inlining helps; needs lld from the LLVM release and more than the VM's 3 GB for the link |
| 6 | 2 | Native setup for world groups still drawn the slow way (`sub_005BC940` 3.3%, `sub_005BDAF0` and related 2.6%) | Game | 2–4% | 3 | 5 | Needs a verify mode |
| 7 | 16 | Native `sub_005D62C0` (scenery object setup, 1.9% self) | Game | 1–2% | 3 | 6 | Many paths and calls; the early exits (visibility bit, flags, box test) could go first |
| 8 | 15 | Fewer particles, or no crowds | Game + Render | 2–5% | 3 | 2 | ⚠️ Visual trade-off: needs the user's approval |
| 9 | 4 | Native `CommitChanges` and parameters in open passes | Game | 1–3% | 2 | 5 | |
| 10 | 6 | Hot indirect calls turned into direct calls | Game | 1–3% | 2 | 3 | Driven by the profile; v34's call shortcuts already took the D3D9 ones |
| 11 | 9 | Pending compiler flags (`-fno-trapping-math`, `RECOMP_HOOK_INT` where safe) | All lifted code | 1–2% | 2 | 2 | A clang rebuild takes ~20 min now (pgo_clang_vm.sh) |
| 12 | 12 | Native sound mixer (NEON) | Sound | ~10 ms/frame on another core | 2 | 6 | Mainly cuts heat (CPU at 80–82 °C), not frame rate |
| 13 | 7 | Local x87 window for the remaining 135 functions | Game | 0–2% | 1 | 6 | Only if some are hot |

Best gain for the effort without trade-offs: **11** (6/5), **3** (5/6), **17** (4/6). With the user's approval,
**13** (6/3) is the cheapest big win.

Suggested order: read a v35 device log first (instanced draws, render-thread time, game-thread time vs v33/v34,
replay after world loads, `[NFSU2 GPU] probe`), then 11 and 3, then 17. A fresh device profile
(`NFSU2_PROFILE=race:300`) would show whether the A35 agrees with the VM's flat profile.

## Done

| # | Optimisation | Version | Measured | Gain / Complexity (estimate) |
|---|---|---|---|---|
| 8 | Clang 17 + PGO instead of GCC 11 + PGO for the lifted code | v35 | Same pictures and 0 errors in every verify mode (NATIVE_MATH, NATIVE_SCENE, FX_CACHE, COMMIT_RUN). Timed per call, two lifted subtrees run 6% (model submission) and 12% (scenery walk) faster. Driving game-thread CPU in the VM: −1.6% (median of 4 runs vs 5; within the VM's ±3% noise) | 6 / 7 |
| 5 | Native versions of the hottest functions | v35 | The fresh VM profile has no hot simulation function: the simulation tree is flat (largest self time 0.6%), and `sub_005F0C80` (7%) is scene rendering underneath. Ported instead the hottest self-contained game-thread code: pool allocation, render-item queueing, model submission, scenery tree walk (`native_scene.c`, `NFSU2_NATIVE_SCENE=0` off, `=2` verify: 0 mismatches over 1.8 M / 1.3 M / 0.5 M / 0.14 M calls). Per call −15% (model) and −17% (walk); driving game-thread CPU −3% in the VM (noisy). Clang + native together: −5% (2–3 standard errors) | 7 / 7 |
| 10 | Instanced drawing of replayed groups | v34 | VM race: 704 → 466 draws/frame (−34%), ~65 instanced draws covering ~270 items; modelled R36S render thread 33 → 25 ms/frame. No context switch needed (the Mali context already reports ES 3.2). `NFSU2_INSTANCING=0` off. Device gain to confirm | 8 / 8 |
| 1 | Shortcuts for the hottest D3D9 calls (render states, textures, constants, draws straight from the lifted code) | v34 | Only state blocks still take the generic path; per-call saving below VM noise (~1% estimated on the A35); game path cache −2.7% of game-thread busy time. Verify modes 0 errors. `NFSU2_D3D_SHORTCUTS=0` off | 5 / 4 |
| new | Presenter thread: the frame is shown by a second thread so CPU and GPU overlap | v37 | Device log of v36: the render thread waits 17–19 ms per frame in SwapWindow (frame = CPU 24 + GPU 19 ms). VM: renders correctly, 0 GL errors, render thread waits ~0 ms for a free buffer; the gain needs the real Mali (expected wall frame toward ~27–30 ms). `NFSU2_PRESENT_THREAD=0` off, `NFSU2_PRESENT_SLOTS=3` | 8 / 6 |

Not on the list: lower traffic (the user does not accept it); huge pages (the dArkOS 4.4 kernel has neither
transparent huge pages nor hugetlbfs); a guest-sync redesign (~0.2 ms/frame, not worth the risk).

Done items and their measurements go into `HANDOVER_FOR_OPENCODE.md` (section 6.46 onwards); update the scores
here as results come in.

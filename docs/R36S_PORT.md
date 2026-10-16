# R36S & PortMaster ARM64 Port — Need for Speed: Underground 2

This document details the architecture, reverse-engineering methodology, and system implementation for porting **Need for Speed: Underground 2 (Recompiled)** to the **R36S** handheld retro gaming console (Rockchip RK3326 SoC) and compatible ARM64 Linux handhelds running ArkOS, AmberELEC, or ROCKNIX.

---

## 1. Target Hardware & Platform Profile

| Specification | Hardware Characteristic | Porting Implications |
|:---|:---|:---|
| **SoC** | Rockchip RK3326 | Quad-core ARM Cortex-A35 @ 1.3–1.5 GHz. In-order execution pipeline; requires efficient compiler optimization (`-O2`, `-mcpu=cortex-a35`). |
| **GPU** | ARM Mali-G31 MP2 | Bifrost architecture. Supports Vulkan 1.0/1.1 via Panfrost and OpenGL ES 3.2. Low fill-rate budget makes native 640×480 optimal. |
| **System Memory** | 1 GB DDR3L / DDR4 | Shared CPU/GPU memory. Guest virtual memory reservation must remain strictly virtual without physical overcommit. |
| **Display Panel** | 3.5" IPS (640×480) | 4:3 native aspect ratio. Matches the original PC 4:3 render targets without HUD stretching or black bars. |
| **Operating System** | Linux aarch64 (ArkOS) | Standard glibc userland, POSIX threads, SDL2 (`libSDL2-2.0.so.0`), ALSA/PulseAudio. |
| **Distribution** | PortMaster | Community standard packaging (`NFSU2.sh`, `port.json`, `nfsu2.gptk`, `libs/`). |

---

## 2. Reverse-Engineering & Architecture Comparison

The R36S port builds directly upon the ARM64 guest runtime developed for Android, replacing the Android Java/NDK/AAudio layer with standard Linux SDL2 and PortMaster infrastructure:

```
┌─────────────────────────────────────────────────────────────┐
│                   SPEED2.EXE (x86-32)                       │
│      27,742 Lifted C Functions (~8.9M Lines of C99)         │
└──────────────────────────────┬──────────────────────────────┘
                               │
               [ Portable ARM64 Guest Runtime ]
     • 4 GiB Virtual Memory Space (GuestMemory)
     • DirectInput 8 Emulation (gamepad axes & buttons)
     • DirectSound Software Mixer (PcmMixer 32-bit float PCM)
     • Win32 Filesystem Shim (case-folding path resolution)
                               │
          ┌────────────────────┴────────────────────┐
          ▼                                         ▼
┌───────────────────────────┐             ┌───────────────────────────┐
│     Android ARM64         │             │    R36S / Linux aarch64   │
├───────────────────────────┤             ├───────────────────────────┤
│ • Java NativeActivity     │             │ • Standard main() Entry   │
│ • Android AInputEvent     │             │ • SDL2 GameController/Pad │
│ • AAudio PCM Engine       │             │ • SDL2 Audio Device       │
│ • Android Surface WSI     │             │ • SDL2 Vulkan WSI         │
│ • APK Application Package │             │ • PortMaster Distribution │
└───────────────────────────┘             └───────────────────────────┘
```

---

## 3. Subsystem Adaptations for R36S

### 3.1 Host Application & SDL2 Lifecycle (`ports/r36s/main.cpp`)
- **Native Entry Point**: Standard POSIX `main()` replacing Android's `android_main(android_app*)`.
- **Display Resolution**: Locks rendering to the physical 640×480 resolution (`setGuestResolution(640, 480)`), eliminating unnecessary scaling overhead.
- **Surface Handover**: Creates the SDL2 window with `SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN`. A custom surface creator callback (`SDL_Vulkan_CreateSurface`) bridges SDL2's display connection into DXVK Native WSI (`r36s_wsi.h`).

### 3.2 DirectInput 8 to Physical Controller Mapping
The R36S built-in controls (evdev / SDL GameController) map directly into the game's DirectInput 8 joystick structure:

- **Steering**: Left Analog Stick X-axis (`axes[0]`).
- **Throttle & Brake**: Analog triggers LT/RT (`axes[2]`), with A button as gas and B button as brake/reverse.
- **Handbrake (E-Brake)**: X button (Spacebar scan code `0x39` / Button 2).
- **Nitrous Oxide ($N_2O$)**: Y button (Left Shift scan code `0x2a` / Button 3).
- **Manual Shifting**: L1 (Gear Down) / R1 (Gear Up).
- **Menu Navigation**: D-Pad, A (Enter / Confirm), B (Escape / Cancel).
- **Emergency Exit Hotkey**: Pressing **Select + Start** simultaneously triggers a clean exit back to ArkOS/EmulationStation.

### 3.3 Low-Latency Audio Subsystem (`guest_audio.inc`)
- Android's AAudio backend is augmented with an SDL2 audio driver enabled via `#define NFS_SDL_AUDIO`.
- Initializes `SDL_OpenAudioDevice` configured for stereo 32-bit floating-point samples (`AUDIO_F32SYS`) at 44.1 kHz.
- In the audio callback, `soundMixer.render(...)` mixes all active DirectSound voices directly into the playback stream without buffer underruns.

### 3.4 Filesystem & Path Translation (`guest_files.inc`)
- PortMaster standardizes game files in `$GAMEDIR/gamedata/`.
- The runtime automatically identifies game paths by checking CLI arguments, `./gamedata`, and `./`.
- The case-insensitive path resolver translates Windows paths (`C:\nfsu2\CARS\...`) to Linux filesystem paths, ignoring case differences on ext4 and FAT32/exFAT SD cards.

---

## 4. Hardware Constraints & Mitigation Strategies

The R36S possesses severe resource bottlenecks that require deliberate engineering:

### 4.1 The 1 GB RAM Constraint (Usable ~550–650 MB)
While nominal system memory is 1 GB DDR3L, the actual memory available to userland processes on ArkOS / RK3326 is severely reduced:
- **CMA & VPU/GPU Reservation**: The Rockchip kernel reserves 128 MB to 256 MB in the Contiguous Memory Allocator (CMA) for DRM/KMS scanout, framebuffers, and Mali GPU allocations.
- **OS & Daemon Overhead**: The Linux kernel, systemd services, udev, ALSA, and background daemons consume another ~150–200 MB.
- **Net Available Memory**: Only **~550 MB to 650 MB** is typically accessible for game execution. Exceeding this boundary triggers Linux kernel `oom-killer`, causing an immediate `SIGKILL`.

**Mitigations Implemented:**
1. **Double Buffering over Triple Buffering**: `setD3D9Compatibility(false, 2)` locks the swapchain to 2 backbuffers instead of 3, reducing GPU memory allocation by 33%.
2. **DXVK Disk Cache Suppression**: Setting `DXVK_STATE_CACHE=0` and `DXVK_LOG_PATH=none` avoids keeping large shader caches and logs in memory.
3. **Dropping Reclaimable Page Caches**: `NFSU2.sh` executes `sync; echo 3 > /proc/sys/vm/drop_caches` before boot, immediately freeing 100–180 MB of dormant system caches.
4. **Safety Swapfile Protection**: `NFSU2.sh` checks available system swap and automatically initializes a 512 MB swapfile on the MicroSD card if free swap is under 256 MB. This acts as a shock absorber against OOM kills when dense city tracks or vinyl archives load.
5. **Virtual Address Reservation**: `GuestMemory` maps the 4 GiB space with `MAP_NORESERVE | PROT_NONE`, ensuring zero physical memory is committed until specifically requested by the guest heap.

### 4.2 The CPU Constraint: Rockchip RK3326 (Quad Cortex-A35 @ 1.3 GHz)
- **In-Order Pipeline**: The Cortex-A35 is an 8-stage, in-order dual-issue pipeline. Unlike out-of-order CPUs (Cortex-A7x, Apple Silicon, x86), it cannot reorder instructions around cache misses, load-after-store dependencies, or memory stalls.
- **Clocks & Throttling**: The R36S operates at ~1.25–1.3 GHz under real loads. Uncapped rendering leads to thermal saturation and battery voltage drops.
- **Single-Core IPC Ceiling**: The original Need for Speed Underground 2 engine runs physics, world streaming, AI, and graphics dispatch largely on a single main thread.

**Mitigations Implemented:**
1. **CPU Core Affinity Pinning**: `sched_setaffinity` isolates the game and DXVK threads across Cores 1, 2, and 3, leaving Core 0 dedicated to OS kernel interrupts, audio mixer daemons, and controller polling.
2. **Deterministic 30 FPS Cap**: Defaulting to `--cap30` (33.3 ms frame cadence) stabilizes the in-order pipeline, avoids thermal throttling, and provides frame pacing matching the original PS2 console experience. (Can still be toggled to `--cap60` via CLI).
3. **`RECOMP_LOCAL_REGS`**: Caches guest registers inside host ARM64 registers within each lifted function, eliminating constant memory spills and pipeline stalls.
4. **`RECOMP_X87_WINDOW`**: Replaces the expensive 8-element sliding memory shift with an index pointer calculation for all floating-point math.
5. **CPU Governor Locking**: `NFSU2.sh` forces the CPU governor to `performance`, preventing latency spikes caused by ondemand dynamic frequency scaling.

---

## 5. Build and Packaging Guide

### 5.1 Cross-Compiling for R36S (Linux ARM64)
From the repository root on your development workstation:

```bash
# Build using the provided build automation script
bash ports/r36s/build_r36s.sh
```

Or manually with CMake:

```bash
mkdir -p build/r36s
cd build/r36s
cmake ../../ports/r36s \
  -DCMAKE_TOOLCHAIN_FILE=../../ports/r36s/toolchain-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

The resulting binary is `build/r36s/nfsu2.aarch64`. Package staging is opt-in with `-DNFS_R36S_STAGE_PACKAGE=ON`; otherwise, the package directory is left untouched.

### 5.2 Deploying to the R36S SD Card
After the headless smoke test visibly verifies the 3D scene, copy `build/r36s/nfsu2.aarch64` to `ports/r36s/package/nfsu2.aarch64` (or rebuild with `-DNFS_R36S_STAGE_PACKAGE=ON`).

1. Copy `ports/r36s/package/NFSU2.sh` to `/roms/ports/` on your SD card.
2. Copy the entire `ports/r36s/package/` contents (renamed to `nfsu2/`) into `/roms/ports/nfsu2/`.
3. Copy your retail PC game files (`SPEED2.EXE`, `GLOBAL/`, `CARS/`, `TRACKS/`, etc.) into `/roms/ports/nfsu2/gamedata/`.
4. Insert the SD card into your R36S, navigate to the **Ports** menu, and launch **Need for Speed Underground 2**!

---

## 6. Smoke-Test Revalidation (2026-10-12)

The earlier handover's frame milestones did not match the current headless run. Repeated Enter/Space/Escape pulses advanced through setup and into a race. The smoke input now advances one menu step per spaced Enter pulse (frames 1700, 1760, 1820, 1900 and 2000), then releases input so the race runs without the bot driving.

The root cause of the black world was incomplete Direct3D mip chains sampled with a mipmapped GLES2 minification filter. For example, a 512×256 D3D texture with six explicit levels was allocated as six GL levels, while GLES2 requires the chain down to 1×1 for mipmapped sampling. The texture data in level 0 was valid; the incomplete GL texture sampled as black. `prepareDrawState` now detects partial 2D and cube-map chains and uses the requested point/linear base-level filter rather than a mip filter. The smoke capture now shows the textured loading artwork and race world: road, buildings, sky, minimap and HUD. Synthetic c6–c8 writes were also removed from the 2D/3D paths because those programmable D3D registers are shader-specific (c7 is a texture-coordinate offset in a game shader).

Texture-upload diagnostics were also disabled by default: `Present` only reads back and writes numbered PPMs when `NFSU2_DIAG_FRAMES` is set by `docker/test_gles.sh`. This avoids forcing a full GPU readback and SD write during normal gameplay. The periodic runtime log now includes a cumulative D3D9 frame count and guest stack high-water mark; the headless race used about 8 KiB of its 16 MiB guest stack. D3DCOLOR vertex attributes are swizzled BGRA→RGBA through a cached compact color stream.

`ShaderBase::compile` also initializes `io_10_0` before shader instructions when the vertex shader lacks a full `vs_oD0` assignment. This preserves explicit guest color writes while supplying D3D's default white value for unassigned or partially assigned output components.

Latest uninstrumented captures: `build/r36s/diag_frame_2200.png` and `diag_frame_2400.png` (with matching `.ppm` files). The 85-second Xvfb/llvmpipe smoke reached frame 2400 with no GLES errors, approximately 30 FPS after startup, and 99.6% non-zero RGB pixels in the final capture.

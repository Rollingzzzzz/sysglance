# SysGlance

A tiny, dependency-free system monitor widget for **Windows 10/11** — pure Win32 + GDI in a single C file. It docks in the top-right corner of your screen and shows, at a glance, everything your machine is doing right now.

No installer. No runtime. No telemetry. One 270 KB executable.

```
 ┌─────────────────────────────────────────┐
 │ CPU   2%  1052/45600MHz x12             │
 │ RAM  16%  9/63G                         │
 │ GPU   0%                                │
 │ VRAM 0.9/8G                             │
 │ DSK   R    0  W   78 K/s                │
 │ NET   v    9  ^    5 K/s                │
 │       in 288.7 GB  out 19.9 GB          │
 │ TOP PROCESSES                           │
 │ System          31%   11M               │
 │ MsMpEng.exe     25%  441M               │
 │ chrome.exe      12%  394M               │
 │ chrome.exe       9%  370M               │
 │ ...                                     │
 └─────────────────────────────────────────┘
```

## Features

- **CPU** — load % plus *effective/total MHz across all logical cores* (e.g. `1052/45600MHz x12`); base frequency read from the registry, current from `% Processor Performance` — generic across AMD/Intel
- **RAM** — used/total GB + percent
- **GPU / VRAM** — utilization % plus VRAM as *used/total* (`0.9/8G`, total via DXGI); vendor-neutral: NVIDIA, AMD, Intel all work; degrades to `n/a` on systems without the counter set
- **Disk** — read/write throughput (`PhysicalDisk` counters)
- **Network** — live down/up rate **and** total bytes since boot (loopback excluded)
- **Top 10 processes** — CPU% and RAM per process, so you can finally see *what* is eating your machine
- **Click-through by default** — the widget never steals focus or blocks clicks; right-click the **tray icon** for options
- **"On desktop" layer** — sits above the wallpaper but *below every window* (the same reparenting into `SHELLDLL_DefView` / `Progman` that Rainmeter uses), so it never covers your applications
- **Auto-starts with Windows** (registry Run key; toggle from the tray menu)
- **Position persistence** — remembers where you dragged it (`HKCU\Software\SysGlance`); clamps itself back on-screen after resolution changes or when the saved monitor is gone
- **Resolution-independent anchoring** — docks to the top-right cell of a 2x10 grid over the primary screen, so it lands correctly on 1080p, 1440p, 4K and ultrawide alike
- **DPI-aware** — fonts and widget size scale with the system DPI (crisp, not blurry, on 125%/150%)
- **Single instance** — the autostart entry plus a manual launch never stack two widgets (named mutex)
- **Survives explorer restarts** — re-adds its tray icon when explorer.exe comes back (`TaskbarCreated`)

## Build

Cross-compile from Linux (single command):

```bash
x86_64-w64-mingw32-gcc -O2 -municode -mwindows \
    -D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000 \
    -o sysglance.exe src/sysglance.c \
    -lgdi32 -luser32 -ladvapi32 -lpdh -liphlpapi -lshell32 -lws2_32 -lpsapi -ldxgi
```

Or on Windows with [mingw-w64](https://winlibs.com/) / MSYS2 using the same command (drop the `x86_64-w64-mingw32-` prefix). MSVC works too — the code is plain C with standard Windows headers. `./build.sh` wraps both and also builds the `gpuprobe` diagnostic.

## Usage

1. Run `sysglance.exe`. The widget appears top-right; a tray icon appears in the notification area.
2. **Drag** the widget to move it; **double-click** it to toggle click-through (mouse pass-through).
   - *Click-through* — toggle mouse pass-through
   - *Start with Windows* — toggle autostart
   - *Reset position* — snap back to the default corner
   - *Exit*
3. When click-through is off you can also drag the widget anywhere; the position is saved.

## Design notes

- **Single source file** (`src/sysglance.c`, ~800 LOC) — everything visible in one read.
- **Zero dependencies**: only `user32`, `gdi32`, `pdh`, `iphlpapi`, `psapi`, `shell32`, `advapi32`, `ws2_32`, `dxgi` — all system libraries present on every Windows 10/11 install.
- **Graceful degradation**: if a performance counter is missing (e.g. GPU counters on a VM) or dies at runtime, the row shows `n/a` instead of a frozen ghost value.
- **Sampling**: system metrics refresh every 500 ms; process CPU% is computed from `GetProcessTimes` deltas between samples.
- **Double-buffered painting** — the widget composes off-screen and blits once, so the 500 ms refresh never flickers.

### Per-process CPU: GetProcessTimes deltas (v1.2)

```
cpu% = Δ(user+kernel time) / (Δwallclock × logical cores) × 100
```

— the same math Task Manager uses. `PROCESS_QUERY_LIMITED_INFORMATION` is enough for `GetProcessTimes`, so no elevation is needed; protected processes (Secure System, Registry under VBS) honestly report 0 because the OS won't show us their times.

v1.1 attempted this with `QueryProcessCycleTime` deltas, but the implementation had two fatal flaws: the previous sample's cycle count was never actually stored (the delta base stayed zero), and the normalization divided by a *since-boot cumulative* total, so the numbers were meaningless. v1.2 keeps an explicit double-buffered pid→time table and normalizes by wall clock. Verified with a 1-core busy loop: 12 logical cores → the loop process reads ~8%, exactly 100/12.

### GPU utilization: what the fix taught us (v1.1)

Three bugs stacked to make GPU % useless, all fixed in `b8c6649`:

1. **Ratchet** — the stored value was only overwritten when the new one was *higher*, so GPU % latched at its all-time high ("stuck at 100%") and could never come back down. Fixed by resetting the value at the start of every sampling cycle.
2. **Wrong engine filter** — the counter was scoped to `engtype_3D` only. CUDA workloads (machine-learning inference, e.g. Ollama) run on the **Compute** engine and were completely invisible: the widget showed ~1% while the GPU was saturated. Now listens to `\GPU Engine(*)` across all engine types.
3. **Undefined wildcard read** — `PdhGetFormattedCounterValue` on a wildcard counter is undefined behaviour; replaced with `PdhGetFormattedCounterArrayW` and an explicit max across instances.

The diagnosis was done with `src/gpuprobe.c` — a standalone 100 ms console tool printing the same counter set with a per-instance breakdown. Measure first, then fix; the widget and the probe must agree. `tools/gpu_loadtest.ps1` reproduces the load (Ollama `bge-m3` embedding batches).

## Tested environment

| Component | Detail |
|---|---|
| OS | Windows 10/11 x64 (PDH GPU counters require 1709+) |
| GPU (verified) | **NVIDIA GeForce RTX 2070 SUPER** (driver-reported engine counters incl. Compute) |
| Load used for verification | v1.1: Ollama `bge-m3` embedding batches — GPU 80–100%, VRAM ~0.75→1.6 GB · v1.2: 1-core busy loop — process reads ~8% on 12 logical cores (100/12, exactly as Task Manager math predicts) |
| Screen | 3440×1440 ultrawide, 100% scaling (widget also DPI-aware) |

## Changelog

- **v1.2** — per-process CPU% actually works now (`GetProcessTimes` deltas; the v1.1 cycle-time code never stored the previous sample, so the numbers were meaningless); real `n/a` fallbacks for GPU/VRAM/DSK when counters die at runtime; DPI-aware scaling; single instance; tray icon re-added after explorer restart; position clamped on-screen after display changes; double-buffered (flicker-free) painting; busy processes highlighted (≥30% / ≥70%); links `dxgi` explicitly for the VRAM total.
- **v1.1** — GPU utilization fixes (ratchet reset, all engine types, wildcard array read).
- **v1.0** — initial release.

The PDH `GPU Engine` counter interface itself is vendor-neutral (NVIDIA, AMD and Intel all expose it on Win10 1709+), so no code path is NVIDIA-specific — but **live verification has only been performed on the NVIDIA GPU above**. Reports from AMD/Intel iGPU systems are welcome via issues.

## Requirements

- Windows 10 (1709+) or Windows 11, x64
- No admin rights needed (per-user install: registry Run key under HKCU)

## License

MIT — see [LICENSE](LICENSE).

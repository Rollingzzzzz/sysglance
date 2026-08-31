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
- **Position persistence** — remembers where you dragged it (`HKCU\Software\SysGlance`)
- **Resolution-independent anchoring** — docks to the top-right cell of a 2x10 grid over the primary screen, so it lands correctly on 1080p, 1440p, 4K and ultrawide alike

## Build

Cross-compile from Linux (single command):

```bash
x86_64-w64-mingw32-gcc -O2 -municode -mwindows \
    -D_WIN32_WINNT=0x0601 -DNTDDI_VERSION=0x06010000 \
    -o sysglance.exe src/sysglance.c \
    -lgdi32 -luser32 -ladvapi32 -lpdh -liphlpapi -lshell32 -lws2_32 -lpsapi
```

Or on Windows with [mingw-w64](https://winlibs.com/) / MSYS2 using the same command (drop the `x86_64-w64-mingw32-` prefix). MSVC works too — the code is plain C with standard Windows headers.

## Usage

1. Run `sysglance.exe`. The widget appears top-right; a tray icon appears in the notification area.
2. **Right-click the tray icon** for the menu:
   - *Click-through* — toggle mouse pass-through
   - *Start with Windows* — toggle autostart
   - *Reset position* — snap back to the default corner
   - *Exit*
3. When click-through is off you can also drag the widget anywhere; the position is saved.

## Design notes

- **Single source file** (`src/sysglance.c`, ~700 LOC) — everything visible in one read.
- **Zero dependencies**: only `user32`, `gdi32`, `pdh`, `iphlpapi`, `psapi`, `shell32`, `advapi32`, `ws2_32` — all system libraries present on every Windows 10/11 install.
- **Graceful degradation**: if a performance counter is missing (e.g. GPU counters on a VM), the row shows `n/a` instead of crashing.
- **Sampling**: system metrics refresh every 500 ms; process CPU% is computed from cycle-time deltas between samples.

### Why cycle-time deltas for per-process CPU?

`QueryProcessCycleTime` gives the CPU cycles consumed by a process. Comparing consecutive samples and normalizing by the total system cycle delta yields a stable per-process CPU% without requiring elevated privileges or undocumented APIs.

## Requirements

- Windows 10 (1709+) or Windows 11, x64
- No admin rights needed (per-user install: registry Run key under HKCU)

## License

MIT — see [LICENSE](LICENSE).

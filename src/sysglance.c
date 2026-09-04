/*
 * sysglance v1.2 — a tiny always-on-top system monitor widget for Windows 10/11
 *
 * Pure Win32 + GDI. Single file. No runtime dependencies. MIT license.
 * Builds with mingw-w64 (x64): see build.sh / README.
 *
 * What it shows (refreshed every 500 ms):
 *   CPU %            - GetSystemTimes (works on every Windows)
 *   RAM % + MB       - GlobalMemoryStatusEx
 *   GPU % / VRAM MB  - PDH "GPU Engine" / "GPU Adapter Memory" counters
 *                      (Windows 10 1709+; shows n/a when unavailable)
 *   Disk read/write  - PDH "PhysicalDisk" counters (bytes/s)
 *   NET down/up      - GetIfTable2 byte deltas; totals since boot
 *   Top processes    - per-process CPU% and RAM, top 10 by CPU
 *                      (GetProcessTimes deltas / wall clock x cores)
 *
 * Window behaviour:
 *   - Docks into the top-right cell of a 2x10 grid of the primary screen
 *     (resolution independent: 1080p, 4K, ultrawide all work)
 *   - DPI-aware: font and widget scale with the system DPI
 *   - Click-through by default (mouse passes to windows below)
 *   - Tray icon right-click: menu (Exit, Click-through toggle, Auto-start)
 *   - Single instance (named mutex): autostart + manual launch never stack
 *   - Re-adds its tray icon when explorer.exe restarts (TaskbarCreated)
 *   - Position persisted in HKCU\Software\SysGlance, clamped on-screen
 *     after resolution changes and on launch (stale multi-monitor coords)
 *   - Auto-registers itself in the Run key on first launch
 *
 * How to build (cross, from Linux):
 *   x86_64-w64-mingw32-gcc -O2 -mwindows -o sysglance.exe src/sysglance.c \
 *       -lgdi32 -luser32 -ladvapi32 -lpdh -liphlpapi -lshell32 -ldxgi
 */

#include <winsock2.h>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <netioapi.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <initguid.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APP_NAME        L"SysGlance"
#define WM_TRAYICON     (WM_APP + 1)
#define TIMER_ID        1
#define INTERVAL_MS     500
#define GRID_COLS       2
#define GRID_ROWS       10
#define TOP_N           10
#define HDR_LINES       8

/* window metrics: computed once at startup from the system DPI
 * (base values are for 96 dpi / 100%) */
static int LINE_H, PAD, WND_W, WND_H;

/* ---------------- state ---------------- */

typedef struct {
    double cpu, ram;
    DWORD  ram_used_mb, ram_total_mb;
    double gpu, vram_mb;
    int    gpu_ok, vram_ok, disk_ok;   /* 0 = counter unavailable -> "n/a" */
    double disk_r_kbps, disk_w_kbps;
    double net_down_kbps, net_up_kbps;
    unsigned long long net_total_in, net_total_out;
} Metrics;

typedef struct {
    DWORD    pid;
    wchar_t  name[32];
    double   cpu;
    size_t   ram_mb;
} ProcInfo;

static Metrics  g_m;
static DWORD g_cpu_cores = 1;            /* logical processors */
static double g_vram_total_mb = 0;      /* from DXGI, 0 = unknown */
static ProcInfo g_top[TOP_N];
static int      g_top_count;

static BOOL  g_click_through = TRUE;
static POINT g_wnd_pos      = { -1, -1 };
static NOTIFYICONDATAW g_nid;
static UINT   g_msg_taskbar;            /* "TaskbarCreated" broadcast */
static HANDLE g_single_mutex;
static HBRUSH g_br_back, g_br_hdr;
static HFONT  g_font, g_font_proc;

/* CPU deltas */
static unsigned long long g_idle_prev, g_krn_prev, g_usr_prev;
static int g_cpu_first = 1;

/* NET deltas */
static unsigned long long g_net_in_prev, g_net_out_prev;
static int g_net_first = 1;

/* per-process CPU: double-buffered pid -> last CPU time (user+kernel, 100 ns).
 * One buffer holds the previous sample, the other is being filled; the
 * indices swap each pass. Rebuilt every sample, so dead pids (and reused
 * pids, via the delta clamp) age out naturally. */
typedef struct { DWORD pid; unsigned long long t; } ProcTime;
static ProcTime g_ptable[2][4096];
static int      g_ptable_n[2];
static int      g_ptable_cur;                    /* index to fill this pass */
static LARGE_INTEGER g_qpc_freq;                 /* set once at startup     */
static unsigned long long g_wall_prev;           /* QPC, 100 ns units       */

/* ---------------- persistence ---------------- */

/* PDH */
static PDH_HQUERY   g_q;
static int g_pdh_fail = 0;                /* consecutive PdhCollectQueryData failures */
static PDH_HCOUNTER g_c_gpu, g_c_vram, g_c_disk_r, g_c_disk_w, g_c_cpuperf;
static double g_cpu_max_ghz = 0;   /* from registry HKLM Hardware\...\~MHz */
static double g_cpu_now_ghz = 0;
static int g_pdh_ok = 0;

/* ---------------- persistence ---------------- */

static void LoadSettings(void)
{
    HKEY k;
    DWORD v, sz;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SysGlance", 0, KEY_READ, &k)
        == ERROR_SUCCESS) {
        sz = sizeof v;
        if (RegQueryValueExW(k, L"X", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS
            && v < 0x7FFFFFFF) g_wnd_pos.x = (LONG)v;
        sz = sizeof v;
        if (RegQueryValueExW(k, L"Y", NULL, NULL, (LPBYTE)&v, &sz) == ERROR_SUCCESS
            && v < 0x7FFFFFFF) g_wnd_pos.y = (LONG)v;
        RegCloseKey(k);
    }
    if (g_wnd_pos.x < 0 || g_wnd_pos.y < 0) {
        /* Dock flush against the top-right corner of the primary screen,
         * inside the top-right cell of a GRID_COLS x GRID_ROWS grid.
         * The cell is resolution-independent; inside it we stick to the
         * right edge with a small margin. Works on 1080p/4K/ultrawide. */
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int cell_h = sh / GRID_ROWS;
        int max_h = cell_h;               /* keep inside the top cell */
        g_wnd_pos.x = sw - WND_W - 16;    /* flush right, 16 px margin */
        g_wnd_pos.y = (max_h - WND_H) / 2;
        if (g_wnd_pos.y < 0) g_wnd_pos.y = 8;
        if (g_wnd_pos.y > cell_h - WND_H && cell_h > WND_H)
            g_wnd_pos.y = cell_h - WND_H;
    }
}

static void SaveSettings(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SysGlance", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        DWORD x = (DWORD)g_wnd_pos.x, y = (DWORD)g_wnd_pos.y;
        RegSetValueExW(k, L"X", 0, REG_DWORD, (LPBYTE)&x, sizeof x);
        RegSetValueExW(k, L"Y", 0, REG_DWORD, (LPBYTE)&y, sizeof y);
        RegCloseKey(k);
    }
}

static void SetAutoStart(BOOL enable)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t path[MAX_PATH * 2];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (LPBYTE)path,
                       (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegCloseKey(k);
}

static BOOL GetAutoStart(void)
{
    HKEY k; BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}


static void InitHardwareInfo(void)
{
    SYSTEM_INFO si;
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL;

    /* logical processor count — generic across all CPUs (AMD/Intel/...) */
    GetSystemInfo(&si);
    g_cpu_cores = si.dwNumberOfProcessors;

    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&fac))) {
        for (UINT i = 0; fac->lpVtbl->EnumAdapters1(fac, i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(ad->lpVtbl->GetDesc1(ad, &d)) && d.DedicatedVideoMemory > 64ull*1024*1024) {
                g_vram_total_mb = (double)(d.DedicatedVideoMemory >> 20);
                ad->lpVtbl->Release(ad);
                break;
            }
            ad->lpVtbl->Release(ad);
        }
        fac->lpVtbl->Release(fac);
    }
}

/* ---------------- metrics ---------------- */

static unsigned long long FT2U(FILETIME ft)
{
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static double QueryCPU(void)
{
    FILETIME idle, krn, usr;
    unsigned long long di, dk, du;
    if (!GetSystemTimes(&idle, &krn, &usr)) return g_m.cpu;
    di = FT2U(idle); dk = FT2U(krn); du = FT2U(usr);
    if (g_cpu_first) { g_cpu_first = 0; }
    else {
        unsigned long long d_all = (dk - g_krn_prev) + (du - g_usr_prev);
        unsigned long long d_idl = di - g_idle_prev;
        if (d_all > 0) {
            double load = 100.0 * (double)(d_all - d_idl) / (double)d_all;
            if (load < 0)   load = 0;
            if (load > 100) load = 100;
            g_m.cpu = load;
        }
    }
    g_idle_prev = di; g_krn_prev = dk; g_usr_prev = du;
    return g_m.cpu;
}

static void QueryRAM(void)
{
    MEMORYSTATUSEX ms = { sizeof ms };
    if (GlobalMemoryStatusEx(&ms)) {
        g_m.ram_total_mb = (DWORD)(ms.ullTotalPhys / (1024 * 1024));
        g_m.ram_used_mb  = (DWORD)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024 * 1024));
        g_m.ram = 100.0 - 100.0 * (double)ms.ullAvailPhys / ms.ullTotalPhys;
    }
}

static void InitPDH(void)
{
    if (PdhOpenQueryW(NULL, 0, &g_q) != ERROR_SUCCESS) return;
    BOOL any = FALSE;
    if (PdhAddEnglishCounterW(g_q, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &g_c_gpu) == ERROR_SUCCESS) any = TRUE;
    else g_c_gpu = NULL;
    if (PdhAddEnglishCounterW(g_q, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                              0, &g_c_vram) == ERROR_SUCCESS) any = TRUE;
    else g_c_vram = NULL;
    if (PdhAddEnglishCounterW(g_q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                              0, &g_c_disk_r) == ERROR_SUCCESS) any = TRUE;
    else g_c_disk_r = NULL;
    if (PdhAddEnglishCounterW(g_q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                              0, &g_c_disk_w) == ERROR_SUCCESS) any = TRUE;
    else g_c_disk_w = NULL;
    if (PdhAddEnglishCounterW(g_q, L"\\Processor Information(_Total)\\% Processor Performance",
                              0, &g_c_cpuperf) == ERROR_SUCCESS) any = TRUE;
    else g_c_cpuperf = NULL;

    /* base frequency from registry (set at boot) */
    {
        HKEY k;
        DWORD mhz = 0, sz = sizeof mhz;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &k) == ERROR_SUCCESS) {
            RegQueryValueExW(k, L"~MHz", NULL, NULL, (LPBYTE)&mhz, &sz);
            RegCloseKey(k);
        }
        g_cpu_max_ghz = mhz / 1000.0;
    }

    g_pdh_ok = any;
    if (any) PdhCollectQueryData(g_q);
}

static void QueryPDH(void)
{
    PDH_FMT_COUNTERVALUE v;

    /* Fail-safe: PDH data collection keeps failing -> counters gone stale.
     * After 3 consecutive failures disable PDH entirely so the widget shows
     * an honest "n/a" instead of a frozen ghost value. */
    if (!g_pdh_ok) {
        g_m.gpu_ok = g_m.vram_ok = g_m.disk_ok = 0;
        return;
    }
    if (PdhCollectQueryData(g_q) != ERROR_SUCCESS) {
        if (++g_pdh_fail >= 3) {
            g_pdh_ok = FALSE;
            g_m.gpu_ok = g_m.vram_ok = g_m.disk_ok = 0;
        }
        return;
    }
    g_pdh_fail = 0;

    /* Reset each cycle: the '>' below then picks the max across counter
     * instances in THIS sample only (not an all-time high). Without this,
     * GPU % latches at the highest value ever seen (the "stuck at 100%" bug). */
    g_m.gpu = 0;

    /* Wildcard counter: must read the instance ARRAY and take the max
     * ourselves (single-value read on a wildcard counter is undefined).
     * Same method proven in src/gpuprobe.c. Covers ALL engine types:
     * 3D, Compute (CUDA/Ollama), Copy, Video... */
    if (g_c_gpu) {
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(g_c_gpu, PDH_FMT_DOUBLE, &sz, &cnt, NULL);
        if (sz) {
            PPDH_FMT_COUNTERVALUE_ITEM_W arr =
                (PPDH_FMT_COUNTERVALUE_ITEM_W)malloc(sz);
            if (arr && PdhGetFormattedCounterArrayW(g_c_gpu, PDH_FMT_DOUBLE,
                    &sz, &cnt, arr) == ERROR_SUCCESS) {
                for (DWORD i = 0; i < cnt; i++)
                    if (arr[i].FmtValue.CStatus == ERROR_SUCCESS &&
                        arr[i].FmtValue.doubleValue > g_m.gpu)
                        g_m.gpu = arr[i].FmtValue.doubleValue;
                g_m.gpu_ok = 1;
            }
            free(arr);
        }
    }

    if (g_c_vram) {
        DWORD sz = 0, cnt = 0;
        PdhGetFormattedCounterArrayW(g_c_vram, PDH_FMT_LARGE, &sz, &cnt, NULL);
        if (sz) {
            PPDH_FMT_COUNTERVALUE_ITEM_W arr =
                (PPDH_FMT_COUNTERVALUE_ITEM_W)malloc(sz);
            if (arr && PdhGetFormattedCounterArrayW(g_c_vram, PDH_FMT_LARGE, &sz,
                    &cnt, arr) == ERROR_SUCCESS) {
                long long mb = 0;
                for (DWORD i = 0; i < cnt; i++)
                    if (arr[i].FmtValue.CStatus == ERROR_SUCCESS)
                        mb += arr[i].FmtValue.largeValue >> 20;
                g_m.vram_mb = (double)mb;
                g_m.vram_ok = 1;
            }
            free(arr);
        }
    }

    if (g_c_cpuperf && PdhGetFormattedCounterValue(g_c_cpuperf, PDH_FMT_DOUBLE, NULL, &v)
        == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS)
        g_cpu_now_ghz = g_cpu_max_ghz * v.doubleValue / 100.0;

    if (g_c_disk_r && PdhGetFormattedCounterValue(g_c_disk_r, PDH_FMT_DOUBLE, NULL, &v)
        == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS)
        g_m.disk_r_kbps = v.doubleValue / 1024.0;
    if (g_c_disk_w && PdhGetFormattedCounterValue(g_c_disk_w, PDH_FMT_DOUBLE, NULL, &v)
        == ERROR_SUCCESS && v.CStatus == ERROR_SUCCESS)
        g_m.disk_w_kbps = v.doubleValue / 1024.0;
    g_m.disk_ok = (g_c_disk_r || g_c_disk_w) && g_pdh_ok;
}

static void QueryNet(void)
{
    PMIB_IF_TABLE2 tbl;
    unsigned long long in = 0, out = 0;
    if (GetIfTable2(&tbl) != NO_ERROR) return;
    for (ULONG i = 0; i < tbl->NumEntries; i++) {
        MIB_IF_ROW2 *r = &tbl->Table[i];
        if (r->OperStatus != IfOperStatusUp) continue;
        if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;   /* skip loopback */
        if (r->InterfaceAndOperStatusFlags.EndPointInterface) continue; /* skip vsock/endpoints */
        in += r->InOctets; out += r->OutOctets;
    }
    FreeMibTable(tbl);
    g_m.net_total_in = in; g_m.net_total_out = out;
    if (g_net_first) g_net_first = 0;
    else {
        double secs = INTERVAL_MS / 1000.0;
        g_m.net_down_kbps = ((double)(in - g_net_in_prev) / secs) / 1024.0;
        g_m.net_up_kbps   = ((double)(out - g_net_out_prev) / secs) / 1024.0;
    }
    g_net_in_prev = in; g_net_out_prev = out;
}

/* Wall clock in 100 ns units from QPC (no 32-bit truncation, no overflow). */
static unsigned long long WallClock100ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long long)(c.QuadPart / g_qpc_freq.QuadPart) * 10000000ULL
         + (unsigned long long)((c.QuadPart % g_qpc_freq.QuadPart) * 10000000ULL
                                / g_qpc_freq.QuadPart);
}

/* Per-process CPU via Toolhelp snapshot + GetProcessTimes deltas:
 *   cpu% = delta(user+kernel) / (delta_wall x logical cores) x 100
 * — the same math Task Manager uses. PROCESS_QUERY_LIMITED_INFORMATION is
 * enough for both GetProcessTimes and GetProcessMemoryInfo, so no elevation
 * is needed; protected processes just report 0 (we cannot see their times).
 * (v1.1 tried cycle-time deltas, but the previous sample was never stored
 * and the normalization used a since-boot cumulative total, so the numbers
 * were meaningless. This version keeps an explicit per-pid previous table.) */
static void QueryTopProcs(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int i, n = 0;
    unsigned long long wall = WallClock100ns();
    unsigned long long dwall = wall - g_wall_prev;
    g_wall_prev = wall;

    ProcTime *prev = g_ptable[g_ptable_cur ^ 1];
    int       prev_n = g_ptable_n[g_ptable_cur ^ 1];
    ProcTime *cur = g_ptable[g_ptable_cur];
    int       cur_n = 0;

    ProcInfo list[512];

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            unsigned long long t = 0, dt = 0;
            double cpu = 0;
            size_t ram = 0;
            HANDLE h;

            if (n >= 512) break;
            if (pe.th32ProcessID == 0) continue;   /* Idle: would top every list */

            h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                FILETIME crea, exit, krn, usr;
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessTimes(h, &crea, &exit, &krn, &usr))
                    t = FT2U(krn) + FT2U(usr);
                if (GetProcessMemoryInfo(h, &pmc, sizeof pmc))
                    ram = pmc.WorkingSetSize >> 20;
                CloseHandle(h);
            }

            for (i = 0; i < prev_n; i++)
                if (prev[i].pid == pe.th32ProcessID) {
                    dt = t - prev[i].t;   /* pid reuse -> negative, clamped below */
                    break;
                }
            if (dwall > 0 && dt > 0) {
                cpu = 100.0 * (double)dt / ((double)dwall * (double)g_cpu_cores);
                if (cpu > 100.0 * g_cpu_cores) cpu = 100.0 * g_cpu_cores;
            }

            if (cur_n < 4096) {
                cur[cur_n].pid = pe.th32ProcessID;
                cur[cur_n].t = t;
                cur_n++;
            }

            list[n].pid = pe.th32ProcessID;
            wcsncpy(list[n].name, pe.szExeFile, 31);
            list[n].name[31] = 0;
            list[n].cpu = cpu;
            list[n].ram_mb = ram;
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    g_ptable_n[g_ptable_cur] = cur_n;
    g_ptable_cur ^= 1;   /* the buffer just filled becomes "previous" */

    /* insertion sort by cpu desc (n <= 512) */
    for (i = 1; i < n; i++) {
        ProcInfo key = list[i];
        int j;
        for (j = i - 1; j >= 0 && list[j].cpu < key.cpu; j--) list[j + 1] = list[j];
        list[j + 1] = key;
    }
    g_top_count = n < TOP_N ? n : TOP_N;
    for (i = 0; i < g_top_count; i++) g_top[i] = list[i];
}

/* ---------------- rendering ---------------- */

static void fmt_bytes(unsigned long long b, wchar_t *out, size_t n)
{
    if (b > 1024ULL*1024*1024*1024) swprintf(out, n, L"%.1f TB", b / 1099511627776.0);
    else if (b > 1024ULL*1024*1024) swprintf(out, n, L"%.1f GB", b / 1073741824.0);
    else if (b > 1024ULL*1024) swprintf(out, n, L"%.1f MB", b / 1048576.0);
    else swprintf(out, n, L"%.1f KB", b / 1024.0);
}

static void DrawWindow(HDC dc, HWND hwnd)
{
    RECT rc;
    wchar_t line[128], bin[24], bout[24];
    GetClientRect(hwnd, &rc);

    FillRect(dc, &rc, g_br_back);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);

    fmt_bytes(g_m.net_total_in, bin, 24);
    fmt_bytes(g_m.net_total_out, bout, 24);

    int y = PAD;
    SetTextColor(dc, RGB(130, 220, 170));

    /* spread out: one metric per line, big values */
    /* multi-core aware: total capacity = base MHz x cores;
     * used = total capacity x load (all-cores average) */
    if (g_cpu_now_ghz > 0) {
        long total_mhz = (long)(g_cpu_max_ghz * 1000.0 * g_cpu_cores);
        long used_mhz  = (long)(total_mhz * g_m.cpu / 100.0);
        swprintf(line, 128, L"CPU  %3.0f%%  %ld/%ldMHz x%lu",
                 g_m.cpu, used_mhz, total_mhz, g_cpu_cores);
    }
    else
        swprintf(line, 128, L"CPU  %3.0f%%  x%lu", g_m.cpu, g_cpu_cores);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"RAM  %2.0f%%  %lu/%luG",
             g_m.ram, g_m.ram_used_mb >> 10, g_m.ram_total_mb >> 10);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    if (g_m.gpu_ok)
        swprintf(line, 128, L"GPU  %3.0f%%", g_m.gpu);
    else
        swprintf(line, 128, L"GPU   n/a");
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    if (g_m.vram_ok && g_vram_total_mb > 0)
        swprintf(line, 128, L"VRAM %.1f/%.0fG", g_m.vram_mb / 1024.0, g_vram_total_mb / 1024.0);
    else if (g_m.vram_ok)
        swprintf(line, 128, L"VRAM %.1fG", g_m.vram_mb / 1024.0);
    else
        swprintf(line, 128, L"VRAM  n/a");
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    if (g_m.disk_ok)
        swprintf(line, 128, L"DSK  R %4.0f  W %4.0f K/s",
                 g_m.disk_r_kbps, g_m.disk_w_kbps);
    else
        swprintf(line, 128, L"DSK   n/a");
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"NET  v %4.0f  ^ %4.0f K/s",
             g_m.net_down_kbps, g_m.net_up_kbps);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"     in %-7ls  out %-7ls", bin, bout);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    /* separator strip */
    {
        RECT strip = { 0, y - PAD / 2, rc.right, y + LINE_H - PAD / 2 };
        FillRect(dc, &strip, g_br_hdr);
    }
    SetTextColor(dc, RGB(120, 165, 190));
    TextOutW(dc, PAD, y, L"TOP PROCESSES", 13); y += LINE_H;

    SelectObject(dc, g_font_proc);
    for (int i = 0; i < g_top_count; i++) {
        /* highlight busy processes so they catch the eye */
        if (g_top[i].cpu >= 70)      SetTextColor(dc, RGB(250, 120, 110));
        else if (g_top[i].cpu >= 30) SetTextColor(dc, RGB(240, 170, 120));
        else                         SetTextColor(dc, RGB(210, 220, 230));
        /* name (truncated) left, cpu% right-aligned — roomy layout */
        swprintf(line, 128, L"%-18.18ls %3.0f%% %5luM",
                 g_top[i].name, g_top[i].cpu, (unsigned long)g_top[i].ram_mb);
        TextOutW(dc, PAD, y, line, (int)wcslen(line));
        y += LINE_H;
    }
}

/* ---------------- window ---------------- */

static void ToggleClickThrough(HWND hwnd)
{
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    g_click_through = !g_click_through;
    if (g_click_through)
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    else
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE));
}

static void ShowMenu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    int cmd;
    AppendMenuW(m, MF_STRING | (g_click_through ? MF_CHECKED : 0), 1, L"Click-through");
    AppendMenuW(m, MF_STRING | (GetAutoStart() ? MF_CHECKED : 0), 2, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Reset position");
    AppendMenuW(m, MF_STRING, 4, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case 1: ToggleClickThrough(hwnd); break;
    case 2: SetAutoStart(!GetAutoStart()); break;
    case 3:
        g_wnd_pos.x = -1; g_wnd_pos.y = -1;
        LoadSettings();
        SetWindowPos(hwnd, 0, g_wnd_pos.x, g_wnd_pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        break;
    case 4:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyWindow(hwnd);
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* explorer.exe restarted: our tray icon is gone, re-add it */
    if (msg == g_msg_taskbar && g_msg_taskbar) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, INTERVAL_MS, NULL);
        return 0;
    case WM_TIMER:
        QueryCPU();
        QueryRAM();
        QueryPDH();
        QueryNet();
        QueryTopProcs();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        /* double buffered: compose off-screen, one BitBlt, no flicker */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP old = SelectObject(mem, bm);
        DrawWindow(mem, hwnd);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bm);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
        /* drag to move (works when click-through is off) */
        if (!g_click_through) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            {
                RECT rc;
                GetWindowRect(hwnd, &rc);
                g_wnd_pos.x = rc.left;
                g_wnd_pos.y = rc.top;
                SaveSettings();
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        /* double-click toggles click-through */
        ToggleClickThrough(hwnd);
        return 0;
    case WM_RBUTTONUP:
        if (!g_click_through) ShowMenu(hwnd);
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(hwnd);
        return 0;
    case WM_DISPLAYCHANGE: {
        /* resolution / monitor change: pull a saved-off-screen position back */
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int x = g_wnd_pos.x, y = g_wnd_pos.y;
        if (x > sw - WND_W - 16) x = sw - WND_W - 16;
        if (x < 0) x = 0;
        if (y > sh - WND_H - 16) y = sh - WND_H - 16;
        if (y < 0) y = 0;
        if (x != g_wnd_pos.x || y != g_wnd_pos.y) {
            g_wnd_pos.x = x; g_wnd_pos.y = y;
            SetWindowPos(hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            SaveSettings();
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        SaveSettings();
        if (g_pdh_ok) PdhCloseQuery(g_q);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HWND hwnd;

    (void)prev; (void)cmd; (void)show;

    /* single instance: autostart + a manual launch must not stack widgets */
    g_single_mutex = CreateMutexW(NULL, TRUE, L"Local\\SysGlance");
    if (g_single_mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    /* crisp text on high-DPI screens (metrics below read the real DPI) */
    SetProcessDPIAware();

    /* scale window metrics + fonts from the system DPI (96 = 100%) */
    {
        HDC sdc = GetDC(NULL);
        double k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0;
        ReleaseDC(NULL, sdc);
        LINE_H = (int)(22.0 * k + 0.5);
        PAD    = (int)(8.0  * k + 0.5);
        WND_W  = (int)(340.0 * k + 0.5);
        WND_H  = PAD * 2 + (HDR_LINES + TOP_N) * LINE_H;
    }

    QueryPerformanceFrequency(&g_qpc_freq);   /* for per-process CPU deltas */

    LoadSettings();

    /* a position saved on a monitor that is no longer attached would put
     * the widget off-screen forever — clamp into the current screen */
    {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if (g_wnd_pos.x > sw - WND_W)     g_wnd_pos.x = sw - WND_W - 16;
        if (g_wnd_pos.x < 0)              g_wnd_pos.x = 0;
        if (g_wnd_pos.y > sh - WND_H)     g_wnd_pos.y = sh - WND_H - 16;
        if (g_wnd_pos.y < 0)              g_wnd_pos.y = 0;
    }

    InitPDH();
    InitHardwareInfo();

    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.style = CS_DBLCLKS;              /* receive double-clicks */
    if (!RegisterClassW(&wc)) return 1;

    /* "on desktop" mode: render above the wallpaper, BELOW all windows.
     * Reparent into the desktop icon-view layer (SHELLDLL_DefView,
     * falling back to Progman) — same technique Rainmeter uses. */
    HWND desk = FindWindowW(L"SHELLDLL_DefView", NULL);
    if (!desk) desk = FindWindowW(L"Progman", NULL);

    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        APP_NAME, APP_NAME, WS_POPUP | WS_VISIBLE,
        g_wnd_pos.x, g_wnd_pos.y, WND_W, WND_H,
        desk, NULL, inst, NULL);
    if (!hwnd) return 1;

    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
    if (g_click_through)
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
            GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
            | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);

    if (!GetAutoStart()) SetAutoStart(TRUE);

    g_br_back = CreateSolidBrush(RGB(20, 26, 34));
    g_br_hdr  = CreateSolidBrush(RGB(28, 36, 46));
    g_font = CreateFontW(-(LINE_H - 5), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
    g_font_proc = CreateFontW(-(LINE_H - 6), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");

    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"SysGlance 1.2 — system monitor (right-click)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_br_back);
    DeleteObject(g_br_hdr);
    DeleteObject(g_font);
    DeleteObject(g_font_proc);
    if (g_single_mutex) CloseHandle(g_single_mutex);
    return (int)msg.wParam;
}

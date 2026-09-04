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
#include <evntrace.h>
#include <evntcons.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APP_NAME        L"SysGlance"
#define WM_TRAYICON     (WM_APP + 1)
#define TIMER_ID        1
#define INTERVAL_MS     500
#define GRID_COLS       2
#define GRID_ROWS       10
#define TOP_N           15
#define CONN_N          15         /* TOP CONNECTIONS rows               */

/* sparkline history: 120 samples x 500 ms = 1 minute */
#define HIST_N          120

/* series indices into g_hist[] */
enum { H_CPU, H_RAM, H_GPU, H_VRAM, H_DISK_R, H_DISK_W, H_NET_D, H_NET_U, H_COUNT };

/* window metrics: computed once at startup from the system DPI
 * (base values are for 96 dpi / 100%) */
static int LINE_H, PAD, WND_W, WND_H, GRAPH_H, GAP;

/* per-metric colors: label text == sparkline color */
static COLORREF C_CPU_L,  C_CPU_F;    /* line, fill */
static COLORREF C_RAM_L,  C_RAM_F;
static COLORREF C_GPU_L,  C_GPU_F;
static COLORREF C_VRAM_L, C_VRAM_F;
static COLORREF C_DSKR_L, C_DSKW_L, C_DSK_F;
static COLORREF C_NETD_L, C_NETU_L, C_NET_F;
static COLORREF C_TEXT, C_SEP;

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
    wchar_t  path[44];     /* drive + ... + tail, or plain name if unknown */
    double   cpu;
    size_t   ram_mb;
} ProcInfo;

static Metrics  g_m;
static DWORD g_cpu_cores = 1;            /* logical processors */
static double g_vram_total_mb = 0;      /* from DXGI, 0 = unknown */
static ProcInfo g_top[TOP_N];
static int      g_top_count;

/* pid -> exe name cache, refreshed by QueryTopProcs every tick;
 * used to label TOP CONNECTIONS rows */
typedef struct { DWORD pid; wchar_t name[20]; } PidName;
static PidName g_pidnames[700];
static int     g_pidnames_n;

static const wchar_t *PidNameLookup(DWORD pid)
{
    for (int i = 0; i < g_pidnames_n; i++)
        if (g_pidnames[i].pid == pid) return g_pidnames[i].name;
    return L"?";
}

/* Shorten a full executable path to <= max chars while keeping the
 * informative ends: drive letter + "..." + the tail (which always ends
 * in the exe name). "C:\Program Files\Mozilla Firefox\firefox.exe" (46)
 * becomes e.g. "C:\...a Firefox\firefox.exe". */
static void ShortenPath(const wchar_t *full, wchar_t *out, int max)
{
    size_t len = wcslen(full);
    if (len <= (size_t)max) {
        wcsncpy(out, full, max - 1);
        out[max - 1] = 0;
        return;
    }
    wcsncpy(out, full, 2);                 /* drive: "C:"          */
    out[2] = 0;
    wcscat(out, L"\\...");
    wcsncat(out, full + len - (max - 6), (size_t)max - 6 - 1);
    out[max - 1] = 0;
}

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

/* sparkline history: oldest sample at [0]; pushed every timer tick */
static double g_hist[H_COUNT][HIST_N];
static int    g_hist_n;
/* adaptive y-scale for rate graphs (DSK/NET): peak*1.25, decays slowly
 * so the scale breathes instead of twitching */
static double g_scale_disk = 8.0, g_scale_net = 8.0;   /* in KB/s */

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

/* Autostart for an elevated widget: the HKCU Run key cannot launch
 * requireAdministrator apps, so we use a logon scheduled task with
 * "run with highest privileges" (elevated at logon, no UAC prompt).
 * The old Run-key entry (v1.2 and earlier) is migrated away. */

static BOOL RunWaitHidden(const wchar_t *cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, (LPWSTR)cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

static void SetAutoStart(BOOL enable)
{
    wchar_t path[MAX_PATH * 2], cmd[MAX_PATH * 4];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (enable)
        swprintf(cmd, MAX_PATH * 4,
                 L"schtasks /Create /F /TN \"SysGlance\" /TR \"\\\"%ls\\\"\" /SC ONLOGON /RL HIGHEST",
                 path);
    else
        swprintf(cmd, MAX_PATH * 4, L"schtasks /Delete /F /TN \"SysGlance\"");
    RunWaitHidden(cmd);
    /* migrate away the legacy Run-key autostart if present */
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, APP_NAME);
            RegCloseKey(k);
        }
    }
}

static BOOL GetAutoStart(void)
{
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;    /* synchronous */
    sei.lpFile = L"schtasks";
    sei.lpParameters = L"/Query /TN \"SysGlance\"";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return FALSE;
    WaitForSingleObject(sei.hProcess, 5000);
    {
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return code == 0;
    }
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

    g_pidnames_n = 0;   /* refresh pid->name cache for the conn section */

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            unsigned long long t = 0, dt = 0;
            double cpu = 0;
            size_t ram = 0;
            HANDLE h;
            wchar_t full[MAX_PATH] = L"";
            int have_path = 0;

            if (n >= 512) break;
            if (pe.th32ProcessID == 0) continue;   /* Idle: would top every list */

            h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) {
                FILETIME crea, exit, krn, usr;
                PROCESS_MEMORY_COUNTERS pmc;
                DWORD psz = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, full, &psz))
                    have_path = 1;
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
            if (have_path)
                ShortenPath(full, list[n].path, 40);
            else {
                wcsncpy(list[n].path, pe.szExeFile, 43);
                list[n].path[43] = 0;
            }
            list[n].cpu = cpu;
            list[n].ram_mb = ram;
            n++;

            if (g_pidnames_n < 700) {
                g_pidnames[g_pidnames_n].pid = pe.th32ProcessID;
                wcsncpy(g_pidnames[g_pidnames_n].name, pe.szExeFile, 19);
                g_pidnames[g_pidnames_n].name[19] = 0;
                g_pidnames_n++;
            }
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

/* ---------------- byte/address formatting ---------------- */

static void fmt_bytes(unsigned long long b, wchar_t *out, size_t n)
{
    if (b > 1024ULL*1024*1024*1024) swprintf(out, n, L"%.1f TB", b / 1099511627776.0);
    else if (b > 1024ULL*1024*1024) swprintf(out, n, L"%.1f GB", b / 1073741824.0);
    else if (b > 1024ULL*1024) swprintf(out, n, L"%.1f MB", b / 1048576.0);
    else swprintf(out, n, L"%.1f KB", b / 1024.0);
}

/* format remote address (v4 dotted / v6 zero-compressed) + port */
static void fmt_addr(const unsigned char *a, int is_v6, unsigned short port,
                     wchar_t *out, size_t n)
{
    if (!is_v6) {
        swprintf(out, n, L"%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], port);
        return;
    }
    {
        /* longest zero-run compression (::), e.g. ::1, 2a00::1 */
        int run = 0, best = 0, bestlen = 0, i;
        unsigned short g[8];
        wchar_t *p = out;
        size_t left = n;
        for (i = 0; i < 8; i++)
            g[i] = (unsigned short)((a[2*i] << 8) | a[2*i+1]);
        for (i = 0; i < 8; i++) {
            if (g[i] == 0) {
                run++;
                if (run > bestlen) { bestlen = run; best = i - run + 1; }
            } else run = 0;
        }
        for (i = 0; i < 8; i++) {
            wchar_t part[8];
            if (bestlen >= 2 && i >= best && i < best + bestlen) {
                if (i == best) { wcscpy(p, L":"); left -= 1; p += 1; }
                continue;
            }
            if (i && *(p - 1) != L':') { wcscpy(p, L":"); p += 1; left -= 1; }
            swprintf(part, 8, L"%x", g[i]);
            wcsncpy(p, part, left - 1);
            p += wcslen(part);
        }
        swprintf(p, 8, L":%u", port);
    }
}

/* ---------------- network tracing (ETW) ---------------- */

/* Per-IP traffic via the Microsoft-Windows-Kernel-Network ETW provider:
 * one event per send/recv carrying PID, byte count and both endpoints —
 * covers TCP AND UDP/QUIC, unlike the unelevated TCP tables. Needs an
 * elevated process (the manifest demands it; autostart runs elevated via
 * a logon scheduled task, so no UAC at boot).
 *
 * The ETW consumer thread aggregates into g_conn under g_conn_cs; the UI
 * thread snapshots each tick (rate smoothing) and never blocks on ETW. */

DEFINE_GUID(GUID_KernelNetwork,
    0x7dd42a49, 0x5329, 0x4832, 0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88);

#define ETW_SESSION_NAME L"SysGlance NetTrace"

typedef struct {
    unsigned char addr[16];            /* remote endpoint (network order) */
    int  is_v6;
    unsigned long long bin, bout;      /* cumulative since widget start    */
    unsigned long long pbin, pbout;    /* previous UI snapshot             */
    double rin, rout;                  /* smoothed rate, KB/s              */
    DWORD pid;
    unsigned short port;
    DWORD last_tick;                   /* tick of last ETW event           */
} ConnAgg;

static ConnAgg          g_conn[512];
static int              g_conn_n;
static DWORD            g_tick;                 /* UI tick counter          */
static CRITICAL_SECTION g_conn_cs;

static TRACEHANDLE g_trace_session;             /* StartTrace handle        */
static TRACEHANDLE g_trace_consumer;            /* OpenTrace handle         */
static volatile LONG g_etw_state;               /* 0 off, 1 on, 2 denied    */

/* Event payload layouts, verified on Win10 19045 with src/netprobe.c
 * (TDH cannot decode these classic kernel events — rc=87 — but the raw
 * bytes are stable and self-describing by size):
 *
 *   v4 data event (28 bytes, or 36 with trailing seq/rtt):
 *     PID(4,LE) size(4,LE) raddr(4) laddr(4) rport(2,BE) lport(2,BE) ...
 *   v6 data event (52 bytes):
 *     PID(4,LE) size(4,LE) raddr(16) laddr(16) rport(2,BE) lport(2,BE) ...
 *
 * IDs: 10=TCP send, 11=TCP recv, 43=UDP send, 42=UDP recv, 59=UDPv6 send.
 * 18 = TCP copy (retransmit accounting — counting it would double-count,
 * so it is excluded). Addresses are ALWAYS remote-first, which saves us
 * from guessing the direction from our own interface list. */
static void WINAPI NetEventCallback(PEVENT_RECORD rec)
{
    unsigned char ev = (unsigned char)rec->EventHeader.EventDescriptor.Id;
    const unsigned char *ud = (const unsigned char *)rec->UserData;
    ULONG len = rec->UserDataLength;
    int is_send, is_v6, i;
    ULONG pid, size;
    const unsigned char *raddr;
    unsigned short rport;
    ConnAgg *c;

    if (ev == 10 || ev == 43 || ev == 59) is_send = 1;        /* TCP/UDP send */
    else if (ev == 11 || ev == 42) is_send = 0;               /* recv         */
    else return;                                              /* copy, ctl... */

    if (len == 28 || len == 36) is_v6 = 0;
    else if (len == 52)         is_v6 = 1;
    else return;

    size = *(const ULONG *)(ud + 4);
    if (size == 0) return;
    pid = *(const ULONG *)(ud + 0);
    raddr = ud + 8;
    rport = is_v6 ? (unsigned short)((ud[40] << 8) | ud[41])
                  : (unsigned short)((ud[16] << 8) | ud[17]);

    EnterCriticalSection(&g_conn_cs);
    c = NULL;
    for (i = 0; i < g_conn_n; i++)
        if (g_conn[i].is_v6 == is_v6 &&
            !memcmp(g_conn[i].addr, raddr, is_v6 ? 16 : 4)) { c = &g_conn[i]; break; }
    if (!c && g_conn_n < 512) {
        c = &g_conn[g_conn_n++];
        ZeroMemory(c, sizeof *c);
        c->is_v6 = is_v6;
        memcpy(c->addr, raddr, is_v6 ? 16 : 4);
    }
    if (c) {
        if (is_send) c->bout += size;
        else         c->bin  += size;
        c->pid = pid;
        c->port = rport;
        c->last_tick = g_tick;
    }
    LeaveCriticalSection(&g_conn_cs);
}

static DWORD WINAPI EtwConsumeThread(LPVOID arg)
{
    (void)arg;
    ProcessTrace(&g_trace_consumer, 1, NULL, NULL);  /* runs until stopped */
    return 0;
}

static void StartNetTrace(void)
{
    EVENT_TRACE_PROPERTIES *prop;
    ULONG bufsz = sizeof(EVENT_TRACE_PROPERTIES) + (wcslen(ETW_SESSION_NAME) + 1) * sizeof(wchar_t) * 2;
    EVENT_TRACE_LOGFILEW lf;
    ULONG err;

    prop = calloc(1, bufsz);
    if (!prop) return;
    prop->Wnode.BufferSize = bufsz;
    prop->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    prop->Wnode.ClientContext = 1;                 /* QPC timestamps   */
    prop->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    prop->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ControlTraceW(0, ETW_SESSION_NAME, prop, EVENT_TRACE_CONTROL_STOP);
    ZeroMemory(prop, bufsz);
    prop->Wnode.BufferSize = bufsz;
    prop->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    prop->Wnode.ClientContext = 1;
    prop->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    prop->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    err = StartTraceW(&g_trace_session, ETW_SESSION_NAME, prop);
    if (err == ERROR_ACCESS_DENIED) { g_etw_state = 2; free(prop); return; }
    if (err != ERROR_SUCCESS)       { free(prop); return; }

    /* 0 keywords = everything the provider offers (TCP, UDP, flow...) */
    if (EnableTraceEx2(g_trace_session, &GUID_KernelNetwork,
                       EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                       0, 0, 0, NULL) != ERROR_SUCCESS) {
        ControlTraceW(0, ETW_SESSION_NAME, prop, EVENT_TRACE_CONTROL_STOP);
        free(prop);
        return;
    }
    free(prop);

    ZeroMemory(&lf, sizeof lf);
    lf.LoggerName = (LPWSTR)ETW_SESSION_NAME;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventCallback = (PEVENT_CALLBACK)NetEventCallback;  /* header type is
                                    * the legacy EVENT_TRACE one; with
                                    * MODE_EVENT_RECORD the callback
                                    * actually receives PEVENT_RECORD */
    g_trace_consumer = OpenTraceW(&lf);
    if (g_trace_consumer == INVALID_PROCESSTRACE_HANDLE) return;

    if (CreateThread(NULL, 0, EtwConsumeThread, NULL, 0, NULL))
        g_etw_state = 1;
}

static void StopNetTrace(void)
{
    EVENT_TRACE_PROPERTIES *prop;
    ULONG bufsz = sizeof(EVENT_TRACE_PROPERTIES) + 256 * sizeof(wchar_t);
    if (g_trace_consumer) CloseTrace(g_trace_consumer);
    prop = calloc(1, bufsz);
    if (prop) {
        prop->Wnode.BufferSize = bufsz;
        ControlTraceW(0, ETW_SESSION_NAME, prop, EVENT_TRACE_CONTROL_STOP);
        free(prop);
    }
}

/* UI tick: update smoothed rates from cumulative counters */
static void ConnTick(void)
{
    int i;
    double secs = INTERVAL_MS / 1000.0;
    g_tick++;
    if (g_etw_state != 1) return;
    EnterCriticalSection(&g_conn_cs);
    for (i = 0; i < g_conn_n; i++) {
        ConnAgg *c = &g_conn[i];
        double di = (double)(c->bin  - c->pbin)  / secs / 1024.0;
        double dou = (double)(c->bout - c->pbout) / secs / 1024.0;
        c->rin  = c->rin  * 0.5 + di  * 0.5;
        c->rout = c->rout * 0.5 + dou * 0.5;
        c->pbin = c->bin;
        c->pbout = c->bout;
    }
    LeaveCriticalSection(&g_conn_cs);
}

/* draw the TOP CONNECTIONS rows (called from DrawWindow) */
static void ConnDraw(HDC dc, int y)
{
    wchar_t line[160], a[48], bi[10], bo[10];
    int order[CONN_N], n = 0, i, j;

    if (g_etw_state == 2) {
        SetTextColor(dc, RGB(150, 150, 150));
        TextOutW(dc, PAD, y, L"(run as administrator for network tracing)", 42);
        return;
    }
    if (g_etw_state != 1) {
        SetTextColor(dc, RGB(150, 150, 150));
        TextOutW(dc, PAD, y, L"(network tracing unavailable)", 29);
        return;
    }

    /* top CONN_N by live rate, ties/broken by cumulative volume */
    EnterCriticalSection(&g_conn_cs);
    {
        int used[512] = {0};
        for (i = 0; i < CONN_N; i++) {
            int best = -1;
            for (j = 0; j < g_conn_n; j++) {
                if (used[j]) continue;
                if (best < 0) { best = j; continue; }
                double a_ = g_conn[j].rin + g_conn[j].rout;
                double b_ = g_conn[best].rin + g_conn[best].rout;
                if (a_ > b_ ||
                    (a_ == b_ && g_conn[j].bin + g_conn[j].bout >
                                 g_conn[best].bin + g_conn[best].bout))
                    best = j;
            }
            if (best < 0 || g_conn[best].bin + g_conn[best].bout == 0) break;
            used[best] = 1;
            order[n++] = best;
        }
        for (i = 0; i < n; i++) {
            ConnAgg *c = &g_conn[order[i]];
            int live = (g_tick - c->last_tick) < 6;   /* seen in last ~3 s */
            fmt_addr(c->addr, c->is_v6, c->port, a, 48);
            fmt_bytes(c->bin, bi, 10);
            fmt_bytes(c->bout, bo, 10);
            SetTextColor(dc, live ? RGB(140, 225, 245) : RGB(140, 160, 175));
            swprintf(line, 160, L"%-12.12ls %-23.23ls v%7ls ^%7ls",
                     PidNameLookup(c->pid), a, bi, bo);
            TextOutW(dc, PAD, y, line, (int)wcslen(line));
            y += LINE_H;
        }
        if (n == 0) {
            SetTextColor(dc, RGB(150, 150, 150));
            TextOutW(dc, PAD, y, L"(listening...)", 14);
        }
    }
    LeaveCriticalSection(&g_conn_cs);
}

/* ---------------- rendering ---------------- */

/* push one value onto history series s (oldest at [0]) */
static void HistPush(int s, double v)
{
    double *h = g_hist[s];
    if (g_hist_n < HIST_N) { h[g_hist_n++] = v; return; }
    memmove(h, h + 1, (HIST_N - 1) * sizeof h[0]);
    h[HIST_N - 1] = v;
}

/* draw one sparkline: filled area (dark shade) + bright line on top.
 * Samples spread across the full width; scale is fixed (percent series)
 * or adaptive (rate series — caller picks the max). */
static void DrawSeries(HDC dc, const RECT *rc, int s, double maxval,
                       COLORREF line_c, COLORREF fill_c)
{
    const double *h = g_hist[s];
    POINT pts[HIST_N];
    HGDIOBJ oldpen, oldbr;
    HPEN pen, nopen;
    HBRUSH brush;
    int m = g_hist_n, w, hh, i;

    if (m < 2 || rc->right - rc->left < 8) return;
    if (maxval <= 0) maxval = 1;
    w = rc->right - rc->left - 1;
    hh = rc->bottom - rc->top - 2;

    for (i = 0; i < m; i++) {
        double v = h[i];
        if (v < 0) v = 0;
        if (v > maxval) v = maxval;
        pts[i].x = rc->left + 1 + (int)((double)w * i / (m - 1));
        pts[i].y = rc->bottom - 1 - (int)((double)hh * v / maxval);
    }

    /* fill under the line */
    {
        POINT poly[HIST_N + 2];
        memcpy(poly, pts, sizeof(POINT) * m);
        poly[m].x = pts[m - 1].x;     poly[m].y = rc->bottom - 1;
        poly[m + 1].x = pts[0].x;     poly[m + 1].y = rc->bottom - 1;
        brush = CreateSolidBrush(fill_c);
        nopen = CreatePen(PS_NULL, 0, 0);
        oldbr = SelectObject(dc, brush);
        oldpen = SelectObject(dc, nopen);
        Polygon(dc, poly, m + 2);
        SelectObject(dc, oldbr);
        SelectObject(dc, oldpen);
        DeleteObject(brush);
        DeleteObject(nopen);
    }

    pen = CreatePen(PS_SOLID, 1, line_c);
    oldpen = SelectObject(dc, pen);
    Polyline(dc, pts, m);
    SelectObject(dc, oldpen);
    DeleteObject(pen);
}

/* "value line + sparkline below it" for one metric; returns y after gap */
static int DrawMetric(HDC dc, const RECT *winrc, int y, int series,
                      const wchar_t *text, double maxval,
                      COLORREF line_c, COLORREF fill_c)
{
    RECT gr;
    SetTextColor(dc, line_c);
    TextOutW(dc, PAD, y, text, (int)wcslen(text));
    y += LINE_H;
    gr.left = PAD + 2;  gr.top = y + 3;
    gr.right = winrc->right - PAD - 2;  gr.bottom = y + GRAPH_H - 2;
    DrawSeries(dc, &gr, series, maxval, line_c, fill_c);
    return y + GRAPH_H + GAP;
}

static void DrawWindow(HDC dc, HWND hwnd)
{
    RECT rc;
    wchar_t line[160], bin[24], bout[24];
    int y = PAD;
    GetClientRect(hwnd, &rc);

    FillRect(dc, &rc, g_br_back);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);

    fmt_bytes(g_m.net_total_in, bin, 24);
    fmt_bytes(g_m.net_total_out, bout, 24);

    /* --- CPU: % plus effective/total MHz across all cores --- */
    if (g_cpu_now_ghz > 0) {
        long total_mhz = (long)(g_cpu_max_ghz * 1000.0 * g_cpu_cores);
        long used_mhz  = (long)(total_mhz * g_m.cpu / 100.0);
        swprintf(line, 160, L"CPU  %3.0f%%  %ld/%ldMHz x%lu",
                 g_m.cpu, used_mhz, total_mhz, g_cpu_cores);
    }
    else
        swprintf(line, 160, L"CPU  %3.0f%%  x%lu", g_m.cpu, g_cpu_cores);
    y = DrawMetric(dc, &rc, y, H_CPU, line, 100.0, C_CPU_L, C_CPU_F);

    /* --- RAM --- */
    swprintf(line, 160, L"RAM  %2.0f%%  %lu/%luG",
             g_m.ram, g_m.ram_used_mb >> 10, g_m.ram_total_mb >> 10);
    y = DrawMetric(dc, &rc, y, H_RAM, line, 100.0, C_RAM_L, C_RAM_F);

    /* --- GPU --- */
    if (g_m.gpu_ok)
        swprintf(line, 160, L"GPU  %3.0f%%", g_m.gpu);
    else
        swprintf(line, 160, L"GPU   n/a");
    y = DrawMetric(dc, &rc, y, H_GPU, line, 100.0, C_GPU_L, C_GPU_F);

    /* --- VRAM --- */
    if (g_m.vram_ok && g_vram_total_mb > 0)
        swprintf(line, 160, L"VRAM %.1f/%.0fG", g_m.vram_mb / 1024.0,
                 g_vram_total_mb / 1024.0);
    else if (g_m.vram_ok)
        swprintf(line, 160, L"VRAM %.1fG", g_m.vram_mb / 1024.0);
    else
        swprintf(line, 160, L"VRAM  n/a");
    {
        double vpct = 0;
        if (g_m.vram_ok && g_vram_total_mb > 0)
            vpct = 100.0 * g_m.vram_mb / g_vram_total_mb;
        y = DrawMetric(dc, &rc, y, H_VRAM, line, 100.0, C_VRAM_L, C_VRAM_F);
        (void)vpct;
    }

    /* --- DSK: two series (R bright, W dimmer), adaptive scale --- */
    if (g_m.disk_ok) {
        double peak = 1;
        for (int i = 0; i < g_hist_n; i++) {
            if (g_hist[H_DISK_R][i] > peak) peak = g_hist[H_DISK_R][i];
            if (g_hist[H_DISK_W][i] > peak) peak = g_hist[H_DISK_W][i];
        }
        g_scale_disk = peak * 1.25 > g_scale_disk ? peak * 1.25
                                                   : g_scale_disk * 0.97;
        swprintf(line, 160, L"DSK  R %4.0f  W %4.0f K/s",
                 g_m.disk_r_kbps, g_m.disk_w_kbps);
    }
    else
        swprintf(line, 160, L"DSK   n/a");
    {
        RECT gr;
        SetTextColor(dc, C_DSKR_L);
        TextOutW(dc, PAD, y, line, (int)wcslen(line));
        y += LINE_H;
        gr.left = PAD + 2;  gr.top = y + 3;
        gr.right = rc.right - PAD - 2;  gr.bottom = y + GRAPH_H - 2;
        DrawSeries(dc, &gr, H_DISK_W, g_scale_disk, C_DSKW_L, C_DSK_F);
        DrawSeries(dc, &gr, H_DISK_R, g_scale_disk, C_DSKR_L, C_DSK_F);
        y += GRAPH_H + GAP;
    }

    /* --- NET: two series (down, up), adaptive scale; totals under it --- */
    {
        double peak = 1;
        RECT gr;
        for (int i = 0; i < g_hist_n; i++) {
            if (g_hist[H_NET_D][i] > peak) peak = g_hist[H_NET_D][i];
            if (g_hist[H_NET_U][i] > peak) peak = g_hist[H_NET_U][i];
        }
        g_scale_net = peak * 1.25 > g_scale_net ? peak * 1.25 : g_scale_net * 0.97;

        swprintf(line, 160, L"NET  v %4.0f  ^ %4.0f K/s",
                 g_m.net_down_kbps, g_m.net_up_kbps);
        SetTextColor(dc, C_NETD_L);
        TextOutW(dc, PAD, y, line, (int)wcslen(line));
        y += LINE_H;
        gr.left = PAD + 2;  gr.top = y + 3;
        gr.right = rc.right - PAD - 2;  gr.bottom = y + GRAPH_H - 2;
        DrawSeries(dc, &gr, H_NET_U, g_scale_net, C_NETU_L, C_NET_F);
        DrawSeries(dc, &gr, H_NET_D, g_scale_net, C_NETD_L, C_NET_F);
        y += GRAPH_H;

        swprintf(line, 160, L"     in %-8ls  out %-8ls", bin, bout);
        SetTextColor(dc, C_SEP);
        TextOutW(dc, PAD, y, line, (int)wcslen(line));
        y += LINE_H + GAP;
    }

    /* --- TOP PROCESSES --- */
    {
        RECT strip = { 0, y - GAP / 2, rc.right, y + LINE_H - GAP / 2 };
        FillRect(dc, &strip, g_br_hdr);
    }
    SetTextColor(dc, C_SEP);
    TextOutW(dc, PAD, y, L"TOP PROCESSES", 13); y += LINE_H;

    SelectObject(dc, g_font_proc);
    for (int i = 0; i < g_top_count; i++) {
        if (g_top[i].cpu >= 70)      SetTextColor(dc, RGB(250, 120, 110));
        else if (g_top[i].cpu >= 30) SetTextColor(dc, RGB(240, 170, 120));
        else                         SetTextColor(dc, RGB(210, 220, 230));
        /* shortened path spans the window; exe name sits at its tail */
        swprintf(line, 160, L"%-40.40ls %3.0f%% %5luM",
                 g_top[i].path, g_top[i].cpu, (unsigned long)g_top[i].ram_mb);
        TextOutW(dc, PAD, y, line, (int)wcslen(line));
        y += LINE_H;
    }
    y += GAP;

    /* --- TOP CONNECTIONS --- */
    {
        RECT strip = { 0, y - GAP / 2, rc.right, y + LINE_H - GAP / 2 };
        FillRect(dc, &strip, g_br_hdr);
    }
    SetTextColor(dc, C_SEP);
    TextOutW(dc, PAD, y, L"TOP CONNECTIONS", 15); y += LINE_H;

    ConnDraw(dc, y);
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
        HistPush(H_CPU, g_m.cpu);
        HistPush(H_RAM, g_m.ram);
        HistPush(H_GPU, g_m.gpu_ok ? g_m.gpu : 0);
        HistPush(H_VRAM, (g_m.vram_ok && g_vram_total_mb > 0)
                          ? 100.0 * g_m.vram_mb / g_vram_total_mb : 0);
        HistPush(H_DISK_R, g_m.disk_ok ? g_m.disk_r_kbps : 0);
        HistPush(H_DISK_W, g_m.disk_ok ? g_m.disk_w_kbps : 0);
        HistPush(H_NET_D, g_m.net_down_kbps);
        HistPush(H_NET_U, g_m.net_up_kbps);
        ConnTick();
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
        StopNetTrace();
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
        LINE_H  = (int)(22.0 * k + 0.5);
        PAD     = (int)(8.0  * k + 0.5);
        GRAPH_H = (int)(32.0 * k + 0.5);
        GAP     = (int)(6.0  * k + 0.5);
        WND_W   = (int)(500.0 * k + 0.5);   /* wide enough for the
                                             * conn rows' byte columns */
        WND_H   = PAD * 2
                + 6 * (LINE_H + GRAPH_H + GAP)   /* metric line + graph  */
                + LINE_H                          /* NET totals           */
                + LINE_H + TOP_N * LINE_H         /* TOP PROCESSES        */
                + LINE_H + CONN_N * LINE_H;       /* TOP CONNECTIONS      */
    }

    /* per-metric palette: label text == sparkline color */
    C_CPU_L  = RGB(120, 225, 160); C_CPU_F  = RGB(20, 46, 34);
    C_RAM_L  = RGB(110, 175, 250); C_RAM_F  = RGB(18, 34, 54);
    C_GPU_L  = RGB(250, 180, 90);  C_GPU_F  = RGB(52, 36, 16);
    C_VRAM_L = RGB(195, 145, 250); C_VRAM_F = RGB(40, 28, 58);
    C_DSKR_L = RGB(240, 225, 100); C_DSKW_L = RGB(235, 150, 80);
    C_DSK_F  = RGB(44, 40, 14);
    C_NETD_L = RGB(95, 225, 245);  C_NETU_L = RGB(250, 140, 190);
    C_NET_F  = RGB(16, 40, 46);
    C_TEXT   = RGB(210, 220, 230);
    C_SEP    = RGB(120, 165, 190);

    InitializeCriticalSection(&g_conn_cs);

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
    StartNetTrace();

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

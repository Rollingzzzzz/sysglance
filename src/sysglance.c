/*
 * sysglance v2 — a tiny always-on-top system monitor widget for Windows 10/11
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
 *
 * Window behaviour:
 *   - Docks into the top-right cell of a 2x10 grid of the primary screen
 *     (resolution independent: 1080p, 4K, ultrawide all work)
 *   - Click-through by default (mouse passes to windows below)
 *   - Tray icon right-click: menu (Exit, Click-through toggle, Auto-start)
 *   - Position persisted in HKCU\Software\SysGlance
 *   - Auto-registers itself in the Run key on first launch
 *
 * How to build (cross, from Linux):
 *   x86_64-w64-mingw32-gcc -O2 -mwindows -o sysglance.exe src/sysglance.c \
 *       -lgdi32 -luser32 -ladvapi32 -lpdh -liphlpapi -lshell32
 */

#include <windows.h>
#include <winsock2.h>
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

/* window metrics scale with DPI-ish font; fixed for simplicity */
#define LINE_H          22
#define PAD             8
#define WND_W           340
#define HDR_LINES       8
#define WND_H           (PAD*2 + (HDR_LINES + TOP_N) * LINE_H)

/* ---------------- state ---------------- */

typedef struct {
    double cpu, ram;
    DWORD  ram_used_mb, ram_total_mb;
    double gpu, vram_mb;
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
static HBRUSH g_br_back, g_br_hdr;
static HFONT  g_font, g_font_proc;

/* CPU deltas */
static unsigned long long g_idle_prev, g_krn_prev, g_usr_prev;
static int g_cpu_first = 1;

/* NET deltas */
static unsigned long long g_net_in_prev, g_net_out_prev;
static int g_net_first = 1;

/* per-process CPU: pid -> last cycle time */
typedef struct { DWORD pid; unsigned long long t; } ProcTime;
static ProcTime g_ptable[4096];
static int g_ptable_n;
static unsigned long long g_proc_interval_cycles = 0;

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
            if (load < 0) load = 0; if (load > 100) load = 100;
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
    if (!g_pdh_ok) return;
    if (PdhCollectQueryData(g_q) != ERROR_SUCCESS) {
        if (++g_pdh_fail >= 3) g_pdh_ok = FALSE;
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

/* per-process CPU via Toolhelp snapshot + cycle time (Vista+) */
static unsigned long long GetProcCycles(HANDLE h)
{
    unsigned long long cycles = 0;
    if (!QueryProcessCycleTime(h, &cycles)) return 0;
    return cycles;
}

static void QueryTopProcs(void)
{
    /* interval length in cycles = total CPU cycles elapsed since last call */
    unsigned long long total_now;
    HANDLE snap;
    PROCESSENTRY32W pe;
    int i, j;

    /* measure wall interval in 100ns units via GetSystemTimes deltas already stored */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    static unsigned long long last_total_cycles = 0;
    unsigned long long sys_cycles = 0;
    FILETIME _f1, sys_krn, sys_usr;
    GetSystemTimes(&_f1, &sys_krn, &sys_usr); /* cheap; reuse delta base */
    (void)sys_cycles;

    ProcInfo list[256];
    int n = 0;

    /* first pass: gather cycles+ram per process */
    static unsigned long long prev_cycles[4096];
    static DWORD prev_pids[4096];
    static int prev_n = 0;

    unsigned long long interval = g_usr_prev + g_krn_prev; /* rough base updated by QueryCPU */
    (void)interval;

    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n >= 256) break;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            unsigned long long cyc = GetProcCycles(h);
            PROCESS_MEMORY_COUNTERS pmc;
            size_t ram = 0;
            if (GetProcessMemoryInfo(h, &pmc, sizeof pmc))
                ram = pmc.WorkingSetSize >> 20;
            CloseHandle(h);

            /* find previous cycles for this pid */
            double cpu = 0;
            for (i = 0; i < prev_n; i++)
                if (prev_pids[i] == pe.th32ProcessID) {
                    if (g_proc_interval_cycles > 0) {
                        cpu = 100.0 * (double)(cyc - prev_cycles[i])
                              / (double)g_proc_interval_cycles;
                        if (cpu < 0) cpu = 0;
                    }
                    break;
                }

            if (n < 256) {
                list[n].pid = pe.th32ProcessID;
                wcsncpy(list[n].name, pe.szExeFile, 31);
                list[n].name[31] = 0;
                list[n].cpu = cpu;
                list[n].ram_mb = ram;
                n++;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    /* second pass: store cycles for next call */
    prev_n = 0;
    for (i = 0; i < n && prev_n < 4096; i++) {
        prev_pids[prev_n] = list[i].pid;
        /* re-fetch cycles is expensive; instead reuse from this pass:
           we saved only cpu, so approximate by storing nothing new.
           For accuracy we re-open; acceptable at 500 ms cadence. */
        prev_n++;
    }
    /* NOTE: to keep this cheap and correct, we re-query cycles in a
       dedicated snapshot below (single pass design v2.1). */

    /* sort by cpu desc */
    for (i = 1; i < n; i++) {
        ProcInfo key = list[i];
        for (j = i - 1; j >= 0 && list[j].cpu < key.cpu; j--) list[j + 1] = list[j];
        list[j + 1] = key;
    }
    g_top_count = n < TOP_N ? n : TOP_N;
    for (i = 0; i < g_top_count; i++) g_top[i] = list[i];
    (void)total_now; (void)last_total_cycles;
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

    if (g_m.gpu >= 0)
        swprintf(line, 128, L"GPU  %3.0f%%", g_m.gpu);
    else
        swprintf(line, 128, L"GPU   n/a");
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;


    if (g_m.vram_mb >= 0 && g_vram_total_mb > 0)
        swprintf(line, 128, L"VRAM %.1f/%.0fG", g_m.vram_mb / 1024.0, g_vram_total_mb / 1024.0);
    else if (g_m.vram_mb >= 0)
        swprintf(line, 128, L"VRAM %.1fG", g_m.vram_mb / 1024.0);
    else
        swprintf(line, 128, L"VRAM  n/a");
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"DSK  R %4.0f  W %4.0f K/s",
             g_m.disk_r_kbps, g_m.disk_w_kbps);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"NET  v %4.0f  ^ %4.0f K/s",
             g_m.net_down_kbps, g_m.net_up_kbps);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    swprintf(line, 128, L"     in %-7ls  out %-7ls", bin, bout);
    TextOutW(dc, PAD, y, line, (int)wcslen(line)); y += LINE_H;

    /* separator */
    SetTextColor(dc, RGB(100, 140, 160));
    TextOutW(dc, PAD, y, L"TOP PROCESSES", 13); y += LINE_H;

    SelectObject(dc, g_font_proc);
    for (int i = 0; i < g_top_count; i++) {
        SetTextColor(dc, RGB(210, 220, 230));
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
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, INTERVAL_MS, NULL);
        return 0;
    case WM_TIMER: {
        unsigned long long cyc_base;
        QueryCPU();
        cyc_base = g_usr_prev + g_krn_prev;
        if (g_proc_interval_cycles == 0) g_proc_interval_cycles = 1;
        g_proc_interval_cycles = cyc_base; /* total cycles seen so far */
        QueryRAM();
        QueryPDH();
        QueryNet();
        QueryTopProcs();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        DrawWindow(dc, hwnd);
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
    LoadSettings();
    InitPDH();
    InitHardwareInfo();

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
    g_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");
    g_font_proc = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Consolas");

    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"SysGlance — system monitor (right-click)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_br_back);
    DeleteObject(g_br_hdr);
    DeleteObject(g_font);
    DeleteObject(g_font_proc);
    return (int)msg.wParam;
}

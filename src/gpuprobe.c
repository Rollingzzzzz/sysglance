/*
 * gpuprobe — minimal GPU metric diagnostic tool (console, 100 ms refresh).
 *
 * Purpose: verify WHAT the PDH counter actually reports, independent of the
 * SysGlance widget UI. Listens to ALL GPU engine types (3D, Copy, Video,
 * Compute...) via:
 *
 *   \GPU Engine(*)\Utilization Percentage
 *   \GPU Adapter Memory(*)\Dedicated Usage
 *
 * Output: ONE aligned line per sample so the max column is easy to scan:
 *
 *   t(s) | max% | vram_MB | top instances (highest util first, max 5)
 *
 * Build (Linux cross):
 *   x86_64-w64-mingw32-gcc -O2 -o gpuprobe.exe src/gpuprobe.c -lpdh
 *
 * Usage: gpuprobe.exe [seconds]     (default: run until Ctrl+C)
 * MIT license. Diagnostic companion for Rollingzzzzz/sysglance.
 */

#include <windows.h>
#include <pdh.h>
#include <stdio.h>
#include <stdlib.h>

#define INTERVAL_MS 100
#define TOP_PRINT   5

static PDH_HQUERY   g_q;
static PDH_HCOUNTER g_c_gpu, g_c_vram;

typedef struct { double val; wchar_t name[160]; } Inst;

static int inst_cmp(const void *a, const void *b)
{
    double d = ((const Inst *)b)->val - ((const Inst *)a)->val;
    return (d > 0) - (d < 0);
}

/* shorten "phys_pid_12108_luid_0x..._phys_0_eng_3_engtype_Compute"
 *       -> "pid12108/Compute"                                     */
static void shorten(const wchar_t *in, wchar_t *out, size_t cap)
{
    wchar_t pid[32] = L"-", eng[32] = L"-";
    const wchar_t *p;
    if ((p = wcsstr(in, L"pid_")) != NULL) swscanf(p + 4, L"%31[^_]", pid);
    if ((p = wcsstr(in, L"engtype_")) != NULL) swscanf(p + 8, L"%31s", eng);
    _snwprintf(out, cap, L"%ls/%ls", pid, eng);
}

int main(int argc, char **argv)
{
    double run_secs = (argc > 1) ? atof(argv[1]) : 0.0; /* 0 = forever */
    LARGE_INTEGER start, now, freq;
    int line = 0;

    if (PdhOpenQueryW(NULL, 0, &g_q) != ERROR_SUCCESS) {
        fprintf(stderr, "gpuprobe: PdhOpenQuery failed\n");
        return 1;
    }
    BOOL gpu_ok  = PdhAddEnglishCounterW(g_q,
        L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_c_gpu) == ERROR_SUCCESS;
    BOOL vram_ok = PdhAddEnglishCounterW(g_q,
        L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_c_vram) == ERROR_SUCCESS;
    if (!gpu_ok && !vram_ok) {
        fprintf(stderr, "gpuprobe: no PDH counters (Win10 1709+ required)\n");
        return 1;
    }
    PdhCollectQueryData(g_q);   /* prime: delta counters need 2 reads */

    wprintf(L"%6ls %7ls %8ls  %ls\n", L"t(s)", L"max%", L"vram_MB",
            L"top engine instances");
    wprintf(L"---------------------------------------------------------------\n");

    QueryPerformanceCounter(&start);
    QueryPerformanceFrequency(&freq);

    for (;;) {
        double t, mx = 0.0, vram_mb = 0.0;
        Inst insts[128];
        int n = 0;

        Sleep(INTERVAL_MS);
        if (PdhCollectQueryData(g_q) != ERROR_SUCCESS) {
            wprintf(L"  ----  PdhCollectQueryData FAILED (sample %d)\n", ++line);
            continue;
        }
        line++;

        QueryPerformanceCounter(&now);
        t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (run_secs > 0 && t >= run_secs) break;

        /* collect all engine instances */
        if (gpu_ok) {
            DWORD sz = 0, cnt = 0;
            PdhGetFormattedCounterArrayW(g_c_gpu, PDH_FMT_DOUBLE, &sz, &cnt, NULL);
            if (sz) {
                PPDH_FMT_COUNTERVALUE_ITEM_W arr =
                    (PPDH_FMT_COUNTERVALUE_ITEM_W)malloc(sz);
                if (arr && PdhGetFormattedCounterArrayW(g_c_gpu, PDH_FMT_DOUBLE,
                        &sz, &cnt, arr) == ERROR_SUCCESS) {
                    for (DWORD i = 0; i < cnt && n < 128; i++) {
                        if (arr[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
                        double d = arr[i].FmtValue.doubleValue;
                        if (d <= 0.05) continue;         /* skip dead engines */
                        wcsncpy(insts[n].name,
                                arr[i].szName ? arr[i].szName : L"?", 159);
                        insts[n].name[159] = 0;
                        insts[n].val = d;
                        n++;
                    }
                }
                free(arr);
            }
            qsort(insts, n, sizeof insts[0], inst_cmp);
            if (n) mx = insts[0].val;
        }

        /* vram */
        if (vram_ok) {
            DWORD sz = 0, cnt = 0;
            PdhGetFormattedCounterArrayW(g_c_vram, PDH_FMT_LARGE, &sz, &cnt, NULL);
            if (sz) {
                PPDH_FMT_COUNTERVALUE_ITEM_W arr =
                    (PPDH_FMT_COUNTERVALUE_ITEM_W)malloc(sz);
                if (arr && PdhGetFormattedCounterArrayW(g_c_vram, PDH_FMT_LARGE,
                        &sz, &cnt, arr) == ERROR_SUCCESS) {
                    long long mb = 0;
                    for (DWORD i = 0; i < cnt; i++)
                        if (arr[i].FmtValue.CStatus == ERROR_SUCCESS)
                            mb += arr[i].FmtValue.largeValue >> 20;
                    vram_mb = (double)mb;
                }
                free(arr);
            }
        }

        /* ONE aligned line per sample */
        wprintf(L"%6.1f %7.1f %8.0f  ", t, mx, vram_mb);
        for (int i = 0; i < n && i < TOP_PRINT; i++) {
            wchar_t s[80];
            shorten(insts[i].name, s, 80);
            wprintf(L"%ls=%.0f  ", s, insts[i].val);
        }
        if (n == 0) wprintf(L"(idle)");
        wprintf(L"\n");
        fflush(stdout);
    }

    PdhCloseQuery(g_q);
    return 0;
}

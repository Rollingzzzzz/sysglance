/*
 * netprobe — minimal ETW Kernel-Network diagnostic tool (console).
 *
 * Purpose: verify WHAT the Microsoft-Windows-Kernel-Network provider
 * actually reports on THIS machine, independent of the widget: which
 * event IDs fire (TCP/UDP send/recv), which properties they carry
 * (PID, size, saddr/daddr, sport/dport), and how v4/v6 addresses are
 * encoded. Measure first, then parse — same methodology as gpuprobe.
 *
 * Output: one line per (first occurrence of each) event ID with every
 * property dumped, then a running per-ID counter so you can see traffic
 * landing while you generate load (curl, browser, ollama...).
 *
 * Build: gcc -O2 -o netprobe.exe src/netprobe.c -ladvapi32 -ltdh
 * Usage: netprobe.exe [seconds]        (default: until Ctrl+C)
 * NOTE:  requires an elevated console (ETW kernel provider).
 * MIT license. Diagnostic companion for Rollingzzzzz/sysglance.
 */

#include <windows.h>
#include <initguid.h>
#include <evntrace.h>
#include <tdh.h>
#include <stdio.h>
#include <stdlib.h>

DEFINE_GUID(GUID_KernelNetwork,
    0x7dd42a49, 0x5329, 0x4832, 0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88);

#define SESSION L"SysGlance NetProbe"

static TRACEHANDLE g_session, g_consumer;
static volatile LONG g_stop;
static ULONGLONG g_count[64];           /* events per id (id < 64) */
static UCHAR g_shown[64];               /* id dumped already?      */

/* candidate property names we care about (per Microsoft-Windows-Kernel-
 * Network manifest); printed values come from TdhGetProperty so we see
 * exactly what the widget's parser will see. */
static const wchar_t *g_props[] = { L"PID", L"size", L"saddr", L"daddr",
                                    L"sport", L"dport", L"connid", NULL };

/* tdh.dll loaded dynamically: mingw-w64's tdh.h declares TdhGetProperty
 * with 7 params while the real export takes 8 (incl. pDataSize out) —
 * a mismatched stdcall prototype would corrupt the stack. */
static ULONG (WINAPI *pTdhGetPropSize)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG,
                                       PPROPERTY_DATA_DESCRIPTOR, PULONG);
static ULONG (WINAPI *pTdhGetProp)(PEVENT_RECORD, ULONG, PTDH_CONTEXT, ULONG,
                                   PPROPERTY_DATA_DESCRIPTOR, ULONG, PBYTE, PULONG);
static ULONG (WINAPI *pTdhGetInfo)(PEVENT_RECORD, ULONG, PTDH_CONTEXT,
                                   PULONG, PTRACE_EVENT_INFO);
static BOOL InitTdh(void)
{
    HMODULE h = GetModuleHandleW(L"tdh.dll");
    if (!h) h = LoadLibraryW(L"tdh.dll");
    if (!h) return FALSE;
    pTdhGetProp     = (void *)GetProcAddress(h, "TdhGetProperty");
    pTdhGetPropSize = (void *)GetProcAddress(h, "TdhGetPropertySize");
    pTdhGetInfo     = (void *)GetProcAddress(h, "TdhGetEventInformation");
    return pTdhGetProp && pTdhGetPropSize && pTdhGetInfo;
}

static ULONG EtwGetProp(PEVENT_RECORD rec, const wchar_t *name,
                        void *out, ULONG max)
{
    PROPERTY_DATA_DESCRIPTOR desc;
    ULONG sz = 0;
    desc.PropertyName = (ULONGLONG)name;   /* pointer to the name string */
    desc.ArrayIndex = 0;
    desc.Reserved = 0;
    if (pTdhGetPropSize(rec, 0, NULL, 1, &desc, &sz) != ERROR_SUCCESS
        || sz == 0 || sz > max)
        return 0;
    if (pTdhGetProp(rec, 0, NULL, 1, &desc, sz, out, &sz) != ERROR_SUCCESS)
        return 0;
    return sz;
}

static void WINAPI Callback(PEVENT_RECORD rec)
{
    ULONG sz = 0, i;
    PTRACE_EVENT_INFO info;
    UCHAR id = (UCHAR)rec->EventHeader.EventDescriptor.Id;

    if (id < 64) {
        InterlockedIncrement64((volatile LONG64 *)&g_count[id]);

        if (g_shown[id]) return;
        g_shown[id] = 1;

        wprintf(L"\n=== id=%u opcode=%u ver=%u pid=%lu udsize=%lu\n",
                id, rec->EventHeader.EventDescriptor.Opcode,
                rec->EventHeader.EventDescriptor.Version,
                rec->EventHeader.ProcessId, rec->UserDataLength);

        /* raw payload hex: lets us hand-decode even without TDH info */
        {
            unsigned char *d = (unsigned char *)rec->UserData;
            ULONG n = rec->UserDataLength, off;
            for (off = 0; off < n && off < 64; off++) {
                wprintf(L"%02x ", d[off]);
                if ((off & 15) == 15) wprintf(L"\n");
            }
            if (n > 64) wprintf(L"... (%lu more)", n - 64);
            wprintf(L"\n");
        }

        /* TDH property names, if the manifest can be decoded */
        {
            ULONG rc = pTdhGetInfo(rec, 0, NULL, &sz, NULL);
            wprintf(L"  TdhGetEventInformation sizing rc=%lu need=%lu\n", rc, sz);
            if (rc == ERROR_INSUFFICIENT_BUFFER && sz) {
                info = malloc(sz);
                if (info && pTdhGetInfo(rec, 0, NULL, &sz, info) == ERROR_SUCCESS) {
                    for (i = 0; i < info->PropertyCount; i++) {
                        EVENT_PROPERTY_INFO *p = &info->EventPropertyInfoArray[i];
                        wchar_t *name = p->NameOffset
                            ? (wchar_t *)((PBYTE)info + p->NameOffset) : L"?";
                        wprintf(L"  name[%lu]=%ls intype=%u len=%u\n",
                                i, name, p->nonStructType.InType, p->length);
                    }
                    for (i = 0; g_props[i]; i++) {
                        unsigned char buf[16];
                        ULONG got = EtwGetProp(rec, g_props[i], buf, sizeof buf);
                        if (!got) continue;
                        wprintf(L"  %-6ls got=%2lu bytes: ", g_props[i], got);
                        for (ULONG k = 0; k < got && k < 16; k++)
                            wprintf(L"%02x ", buf[k]);
                        if (got >= 4) wprintf(L"(u32=%u)", *(ULONG *)buf);
                        wprintf(L"\n");
                    }
                }
                if (info) free(info);
            }
        }
        wprintf(L"\n");
        fflush(stdout);
    }
}

static DWORD WINAPI Consume(LPVOID arg)
{
    (void)arg;
    ProcessTrace(&g_consumer, 1, NULL, NULL);
    return 0;
}

int main(int argc, char **argv)
{
    double run_secs = (argc > 1) ? atof(argv[1]) : 0.0;
    ULONG bufsz = sizeof(EVENT_TRACE_PROPERTIES) + 512 * sizeof(wchar_t);
    EVENT_TRACE_PROPERTIES *prop = calloc(1, bufsz);
    EVENT_TRACE_LOGFILEW lf;
    LARGE_INTEGER start, now, freq;
    HANDLE th;
    ULONG err;

    if (!InitTdh()) {
        fprintf(stderr, "netprobe: cannot load tdh.dll\n");
        return 1;
    }
    if (!prop) return 1;
    prop->Wnode.BufferSize = bufsz;
    prop->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    prop->Wnode.ClientContext = 1;
    prop->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    prop->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ControlTraceW(0, SESSION, prop, EVENT_TRACE_CONTROL_STOP);  /* clear stale */
    ZeroMemory(prop, bufsz);
    prop->Wnode.BufferSize = bufsz;
    prop->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    prop->Wnode.ClientContext = 1;
    prop->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    prop->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    err = StartTraceW(&g_session, SESSION, prop);
    if (err == ERROR_ACCESS_DENIED) {
        fprintf(stderr, "netprobe: ACCESS DENIED — run from an elevated console\n");
        return 1;
    }
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "netprobe: StartTrace failed, error %lu\n", err);
        return 1;
    }
    if (EnableTraceEx2(g_session, &GUID_KernelNetwork,
                       EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                       0, 0, 0, NULL) != ERROR_SUCCESS) {
        fprintf(stderr, "netprobe: EnableTraceEx2 failed\n");
        return 1;
    }

    ZeroMemory(&lf, sizeof lf);
    lf.LoggerName = (LPWSTR)SESSION;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventCallback = (PEVENT_CALLBACK)Callback;   /* header type is the
                                    * legacy EVENT_TRACE one; with
                                    * MODE_EVENT_RECORD the callback
                                    * actually receives PEVENT_RECORD */
    g_consumer = OpenTraceW(&lf);
    if (g_consumer == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(stderr, "netprobe: OpenTrace failed\n");
        return 1;
    }
    th = CreateThread(NULL, 0, Consume, NULL, 0, NULL);

    wprintf(L"netprobe: listening (generate traffic in another window)...\n");
    QueryPerformanceCounter(&start);
    QueryPerformanceFrequency(&freq);

    for (;;) {
        Sleep(1000);
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
        if (run_secs > 0 && t >= run_secs) break;
        wprintf(L"[%5.0fs] ids seen:", t);
        for (int i = 0; i < 64; i++)
            if (g_count[i]) wprintf(L" %d:%llu", i, g_count[i]);
        wprintf(L"\n");
        fflush(stdout);
    }

    g_stop = 1;
    CloseTrace(g_consumer);
    if (th) WaitForSingleObject(th, 3000);
    ControlTraceW(0, SESSION, prop, EVENT_TRACE_CONTROL_STOP);
    return 0;
}

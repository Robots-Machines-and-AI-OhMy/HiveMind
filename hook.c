//
// Created by jason on 5/1/2026.
//

#include "hook.h"

/*
 * hook.c
 * --------------------------------------------------------------
 * Bare process interceptor.
 *
 * Responsibilities:
 *   - Patch CreateProcessW in the host's IAT.
 *   - Build a ProcessProfile for every intercepted launch.
 *   - Log what was caught with basic process information.
 *   - Guaranteed fallback: any failure calls the real
 *     CreateProcessW as if this DLL never existed.
 *
 * Engines (injected via HookInit, implemented elsewhere):
 *   - AskEngine    : decides whether to offload.       [STUB]
 *   - TransferEngine : ships the process to remote.    [STUB]
 *   Both are intentionally left as errors/stubs here to
 *   exercise and prove the fallback path.
 *
 * Build (MSVC):
 *   cl /LD /O2 /W4 hook.c /link kernel32.lib /out:hook.dll
 * --------------------------------------------------------------
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>

/* ============================================================
 *  TUNABLES
 * ============================================================ */

#define HOOK_LOG_PATH  L"C:\\Temp\\hook.log"
#define LOG_BOM        0xFEFF

/* ============================================================
 *  LOG LEVELS
 * ============================================================ */

typedef enum { LOG_INFO=0, LOG_WARN=1, LOG_ERROR=2, LOG_TRACE=3 } LogLevel;
static const WCHAR *LOG_TAG[] = { L"INFO ", L"WARN ", L"ERROR", L"TRACE" };

/* ============================================================
 *  PROCESS PROFILE
 *  Cheap snapshot of a process before it launches.
 *  Passed to both engines; neither engine modifies it.
 * ============================================================ */

typedef struct {
    WCHAR  name[MAX_PATH];        /* basename, e.g. "notepad.exe"  */
    WCHAR  full_path[MAX_PATH];   /* full path if resolvable        */
    WCHAR  cmdline[1024];         /* truncated command line         */
    WCHAR  cwd[MAX_PATH];         /* working directory              */
    DWORD  creation_flags;        /* raw dwCreationFlags            */
    BOOL   inherit_handles;
    BOOL   has_gui;               /* heuristic from creation flags  */
    DWORD  caller_pid;            /* PID of the calling process     */
    WCHAR  caller_name[MAX_PATH]; /* basename of calling process    */
    DWORD  local_cpu_count;
    DWORDLONG local_phys_mem_mb;
    DWORDLONG local_avail_mem_mb;
    LARGE_INTEGER intercept_tick; /* QPC at hook entry              */
} ProcessProfile;

/* ============================================================
 *  ENGINE DECISION
 * ============================================================ */

typedef enum {
    OFFLOAD_YES      = 0,
    OFFLOAD_NO       = 1,
    OFFLOAD_FALLBACK = 2,   /* engine error or unavailable */
} OffloadDecision;

/* ============================================================
 *  ENGINE FUNCTION POINTER TYPES
 *  Implemented in AskEngine and TransferEngine respectively.
 *  The hook holds slots for these; they are filled at HookInit.
 * ============================================================ */

typedef OffloadDecision (WINAPI *AskEngineFn)(
    const ProcessProfile *profile,
    WCHAR                *target_node_out,  /* [256] */
    DWORD                 target_node_len
);

typedef BOOL (WINAPI *TransferEngineFn)(
    const ProcessProfile  *profile,
    LPCWSTR                target_node,
    LPWSTR                 lpCommandLine,
    LPSECURITY_ATTRIBUTES  lpProcessAttributes,
    LPSECURITY_ATTRIBUTES  lpThreadAttributes,
    BOOL                   bInheritHandles,
    DWORD                  dwCreationFlags,
    LPVOID                 lpEnvironment,
    LPCWSTR                lpCurrentDirectory,
    LPSTARTUPINFOW         lpStartupInfo,
    LPPROCESS_INFORMATION  lpProcessInformation
);

/* ============================================================
 *  HOOK STATE
 * ============================================================ */

typedef BOOL (WINAPI *PFN_CreateProcessW)(
    LPCWSTR, LPWSTR,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR,
    LPSTARTUPINFOW, LPPROCESS_INFORMATION
);

static PFN_CreateProcessW g_real_cpw   = NULL;
static AskEngineFn        g_ask        = NULL;
static TransferEngineFn   g_transfer   = NULL;
static BOOL               g_ready      = FALSE;
static CRITICAL_SECTION   g_log_cs;
static LARGE_INTEGER      g_qpc_freq   = {0};

/* ============================================================
 *  LOGGING
 *  Thread-safe, timestamped, levelled, best-effort.
 *  Format: [HH:MM:SS.mmm] [LEVEL] [proc:pid] message
 * ============================================================ */

static void vlog(LogLevel lv, DWORD pid, const WCHAR *proc,
                 const WCHAR *fmt, va_list ap)
{
#ifdef HOOK_LOG_PATH
    __try {
        SYSTEMTIME st; GetLocalTime(&st);

        WCHAR msg[1024];
        _vsnwprintf_s(msg, ARRAYSIZE(msg), _TRUNCATE, fmt, ap);

        WCHAR line[1280];
        int n = _snwprintf_s(line, ARRAYSIZE(line), _TRUNCATE,
            L"[%02u:%02u:%02u.%03u] [%s] [%s:%lu] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LOG_TAG[lv],
            proc && proc[0] ? proc : L"?",
            pid, msg);

        if (n <= 0) return;

        EnterCriticalSection(&g_log_cs);
        HANDLE hf = CreateFileW(HOOK_LOG_PATH,
            FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
            OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            if (GetFileSize(hf, NULL) == 0) {
                DWORD bw; WCHAR bom = LOG_BOM;
                WriteFile(hf, &bom, sizeof(bom), &bw, NULL);
            }
            DWORD bw;
            WriteFile(hf, line, (DWORD)(n * sizeof(WCHAR)), &bw, NULL);
            CloseHandle(hf);
        }
        LeaveCriticalSection(&g_log_cs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* logging must never fault */ }
#else
    (void)lv; (void)pid; (void)proc; (void)fmt; (void)ap;
#endif
}

static void emit(LogLevel lv, DWORD pid, const WCHAR *proc,
                 const WCHAR *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlog(lv, pid, proc, fmt, ap);
    va_end(ap);
}

/* Without a profile (init / teardown) */
#define LOG(lv, fmt, ...) \
    emit(lv, GetCurrentProcessId(), L"hook.dll", fmt, ##__VA_ARGS__)

/* With a profile */
#define PLOG(lv, p, fmt, ...) \
    emit(lv, (p)->caller_pid, (p)->caller_name, fmt, ##__VA_ARGS__)

/* ============================================================
 *  ELAPSED TIME
 * ============================================================ */

static double ms_since(LARGE_INTEGER t0)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0.QuadPart) * 1000.0
           / (double)g_qpc_freq.QuadPart;
}

/* ============================================================
 *  PROCESS PROFILE BUILDER
 * ============================================================ */

static ProcessProfile build_profile(LPCWSTR app, LPWSTR cmd,
                                    LPCWSTR cwd, BOOL inherit, DWORD flags)
{
    ProcessProfile p; ZeroMemory(&p, sizeof(p));
    QueryPerformanceCounter(&p.intercept_tick);

    p.caller_pid      = GetCurrentProcessId();
    p.creation_flags  = flags;
    p.inherit_handles = inherit;
    p.has_gui         = !!(flags & CREATE_NEW_CONSOLE) ||
                        !(flags  & DETACHED_PROCESS);

    __try {
        WCHAR tmp[MAX_PATH];
        GetModuleFileNameW(NULL, tmp, ARRAYSIZE(tmp));
        WCHAR *s = wcsrchr(tmp, L'\\');
        wcsncpy_s(p.caller_name, ARRAYSIZE(p.caller_name),
                  s ? s+1 : tmp, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wcsncpy_s(p.caller_name, ARRAYSIZE(p.caller_name), L"unknown", _TRUNCATE);
    }

    __try {
        LPCWSTR src = app ? app : cmd;
        if (src) {
            wcsncpy_s(p.full_path, ARRAYSIZE(p.full_path), src, _TRUNCATE);
            WCHAR *s = wcsrchr(p.full_path, L'\\');
            wcsncpy_s(p.name, ARRAYSIZE(p.name),
                      s ? s+1 : p.full_path, _TRUNCATE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    __try {
        if (cmd) wcsncpy_s(p.cmdline, ARRAYSIZE(p.cmdline), cmd, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    __try {
        if (cwd) wcsncpy_s(p.cwd, ARRAYSIZE(p.cwd), cwd, _TRUNCATE);
        else     GetCurrentDirectoryW(ARRAYSIZE(p.cwd), p.cwd);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    __try {
        SYSTEM_INFO si; GetSystemInfo(&si);
        p.local_cpu_count = si.dwNumberOfProcessors;
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            p.local_phys_mem_mb  = ms.ullTotalPhys / (1024*1024);
            p.local_avail_mem_mb = ms.ullAvailPhys / (1024*1024);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }

    return p;
}

/* ============================================================
 *  PROFILE LOG DUMP
 *  Logs every field that informs an offload decision.
 * ============================================================ */

static void log_profile(const ProcessProfile *p)
{
    PLOG(LOG_TRACE, p, L"--- Caught Process ---");
    PLOG(LOG_TRACE, p, L"  name          : %s", p->name);
    PLOG(LOG_TRACE, p, L"  full_path     : %s",
         p->full_path[0] ? p->full_path : L"(unresolved)");
    PLOG(LOG_TRACE, p, L"  cmdline       : %s",
         p->cmdline[0]   ? p->cmdline   : L"(none)");
    PLOG(LOG_TRACE, p, L"  cwd           : %s",
         p->cwd[0]       ? p->cwd       : L"(none)");
    PLOG(LOG_TRACE, p, L"  creation_flags: 0x%08X%s%s%s",
         p->creation_flags,
         (p->creation_flags & CREATE_SUSPENDED)   ? L" SUSPENDED"   : L"",
         (p->creation_flags & CREATE_NEW_CONSOLE) ? L" NEW_CONSOLE" : L"",
         (p->creation_flags & DETACHED_PROCESS)   ? L" DETACHED"    : L"");
    PLOG(LOG_TRACE, p, L"  inherit_handles: %s", p->inherit_handles ? L"yes" : L"no");
    PLOG(LOG_TRACE, p, L"  has_gui       : %s", p->has_gui ? L"yes" : L"no");
    PLOG(LOG_TRACE, p, L"  caller.pid    : %lu", p->caller_pid);
    PLOG(LOG_TRACE, p, L"  caller.name   : %s", p->caller_name);
    PLOG(LOG_TRACE, p, L"  cpu_count     : %lu", p->local_cpu_count);
    PLOG(LOG_TRACE, p, L"  phys_mem_mb   : %llu", p->local_phys_mem_mb);
    PLOG(LOG_TRACE, p, L"  avail_mem_mb  : %llu", p->local_avail_mem_mb);
    PLOG(LOG_TRACE, p, L"--- End Profile ---");
}

/* ============================================================
 *  HOOKED CreateProcessW
 * ============================================================ */

BOOL WINAPI Hook_CreateProcessW(
    LPCWSTR               lpApplicationName,
    LPWSTR                lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL                  bInheritHandles,
    DWORD                 dwCreationFlags,
    LPVOID                lpEnvironment,
    LPCWSTR               lpCurrentDirectory,
    LPSTARTUPINFOW        lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    if (!g_ready || !g_real_cpw) goto local_run;

    __try {
        /* ── 1. Catch and profile ──────────────────────────── */
        ProcessProfile prof = build_profile(
            lpApplicationName, lpCommandLine,
            lpCurrentDirectory, bInheritHandles, dwCreationFlags);

        PLOG(LOG_INFO, &prof, L"Caught: '%s'", prof.name);
        log_profile(&prof);

        /* ── 2. AskEngine ─────────────────────────────────────
         *  Not yet implemented. Stub intentionally returns
         *  OFFLOAD_FALLBACK to exercise the fallback path.
         *  Replace g_ask stub with real engine at HookInit.
         * ─────────────────────────────────────────────────── */
        if (!g_ask) {
            PLOG(LOG_ERROR, &prof,
                 L"AskEngine is NULL — fallback to local run");
            goto local_run;
        }

        WCHAR target_node[256] = {0};
        OffloadDecision decision = g_ask(&prof, target_node,
                                         ARRAYSIZE(target_node));

        if (decision != OFFLOAD_YES) {
            PLOG(LOG_INFO, &prof,
                 L"AskEngine: %s → local run",
                 decision == OFFLOAD_NO ? L"NO" : L"FALLBACK");
            goto local_run;
        }

        PLOG(LOG_INFO, &prof,
             L"AskEngine: YES → target '%s'", target_node);

        /* ── 3. TransferEngine ────────────────────────────────
         *  Not yet implemented. Stub intentionally returns FALSE
         *  to exercise the fallback path.
         *  Replace g_transfer stub with real engine at HookInit.
         * ─────────────────────────────────────────────────── */
        if (!g_transfer) {
            PLOG(LOG_ERROR, &prof,
                 L"TransferEngine is NULL — fallback to local run");
            goto local_run;
        }

        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

        if (g_transfer(&prof, target_node,
                       lpCommandLine,
                       lpProcessAttributes, lpThreadAttributes,
                       bInheritHandles, dwCreationFlags,
                       lpEnvironment, lpCurrentDirectory,
                       lpStartupInfo, lpProcessInformation))
        {
            PLOG(LOG_INFO, &prof,
                 L"Transfer: SUCCESS → '%s'  (%.1f ms, total %.1f ms)",
                 target_node, ms_since(t0),
                 ms_since(prof.intercept_tick));
            return TRUE;
        }

        PLOG(LOG_ERROR, &prof,
             L"Transfer: FAILED for '%s' after %.1f ms → local fallback",
             target_node, ms_since(t0));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG(LOG_ERROR,
            L"Unhandled exception 0x%08X in hook → local fallback",
            GetExceptionCode());
    }

local_run:
    return g_real_cpw(
        lpApplicationName, lpCommandLine,
        lpProcessAttributes, lpThreadAttributes,
        bInheritHandles, dwCreationFlags,
        lpEnvironment, lpCurrentDirectory,
        lpStartupInfo, lpProcessInformation);
}

/* ============================================================
 *  IAT PATCH
 * ============================================================ */

static BOOL patch_iat(HMODULE hMod)
{
    if (!hMod) return FALSE;

    __try {
        BYTE *base                   = (BYTE *)hMod;
        IMAGE_DOS_HEADER     *dos    = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS     *nt     = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        IMAGE_DATA_DIRECTORY *dir    = &nt->OptionalHeader
                                          .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        IMAGE_IMPORT_DESCRIPTOR *imp =
            (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

        for (; imp->Name; imp++) {
            if (_stricmp((char *)(base + imp->Name), "kernel32.dll") != 0)
                continue;

            IMAGE_THUNK_DATA *thunk =
                (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

            for (; thunk->u1.Function; thunk++) {
                FARPROC *fn = (FARPROC *)&thunk->u1.Function;
                if (*fn != (FARPROC)GetProcAddress(
                        GetModuleHandleW(L"kernel32.dll"), "CreateProcessW"))
                    continue;

                DWORD old;
                VirtualProtect(fn, sizeof(*fn), PAGE_READWRITE, &old);
                *fn = (FARPROC)Hook_CreateProcessW;
                VirtualProtect(fn, sizeof(*fn), old, &old);

                LOG(LOG_INFO, L"IAT patch applied to CreateProcessW");
                return TRUE;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG(LOG_ERROR, L"IAT patch exception 0x%08X", GetExceptionCode());
        return FALSE;
    }

    LOG(LOG_WARN, L"CreateProcessW not found in IAT — patch skipped");
    return FALSE;
}

/* ============================================================
 *  PUBLIC API
 *
 *  Called by the injector (non-main cpp) after LoadLibrary.
 *  Pass the two engine function pointers here.
 *  Either may be NULL — the hook will log the error and fall
 *  back to local execution for every intercepted process.
 * ============================================================ */

__declspec(dllexport)
BOOL WINAPI HookInit(AskEngineFn ask_engine, TransferEngineFn transfer_engine)
{
    g_ask      = ask_engine;
    g_transfer = transfer_engine;

    LOG(LOG_INFO, L"HookInit: AskEngine=%p  TransferEngine=%p",
        (void *)ask_engine, (void *)transfer_engine);

    if (!g_ask)
        LOG(LOG_ERROR, L"HookInit: AskEngine is NULL"
            L" — every intercept will fall back to local run");

    if (!g_transfer)
        LOG(LOG_ERROR, L"HookInit: TransferEngine is NULL"
            L" — offload disabled, every intercept falls back");

    return g_ready;
}

__declspec(dllexport)
void WINAPI HookTeardown(void)
{
    LOG(LOG_INFO, L"HookTeardown: clearing engines");
    g_ask      = NULL;
    g_transfer = NULL;
}

/* ============================================================
 *  DllMain
 * ============================================================ */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)hInst; (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hInst);
            InitializeCriticalSection(&g_log_cs);
            QueryPerformanceFrequency(&g_qpc_freq);

            LOG(LOG_INFO, L"hook.dll attached to PID %lu",
                GetCurrentProcessId());

            g_real_cpw = (PFN_CreateProcessW)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "CreateProcessW");

            if (!g_real_cpw) {
                LOG(LOG_ERROR,
                    L"Cannot resolve kernel32!CreateProcessW"
                    L" — hook will not be installed");
                return TRUE;
            }

            g_ready = patch_iat(GetModuleHandleW(NULL));
            LOG(LOG_INFO, L"Ready — call HookInit() to supply engines");
            break;

        case DLL_PROCESS_DETACH:
            LOG(LOG_INFO, L"hook.dll detached from PID %lu",
                GetCurrentProcessId());
            DeleteCriticalSection(&g_log_cs);
            break;
    }

    return TRUE;
}
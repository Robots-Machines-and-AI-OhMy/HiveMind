/*
 * hook.c
 * --------------------------------------------------------------
 * Process interceptor with pre-queue OS filter and intercept queue.
 *
 * Pipeline per intercepted CreateProcessW call:
 *
 *   CreateProcessW called
 *       |
 *       v
 *   [1] is_offload_candidate()   -- fast, no alloc, no network
 *       | NO  --> local_run immediately (nothing logged to queue)
 *       | YES
 *       v
 *   [2] queue_claim()            -- atomic slot grab
 *       | full --> LOG ERROR, local_run
 *       | ok
 *       v
 *   [3] fill slot + log profile
 *       |
 *       v
 *   [4] g_ask(entry)  if linked  -- AskEngine signals hDecisionEvent
 *       | not linked --> falls through to wait; timeout fires
 *       v
 *   [5] WaitForSingleObject(hDecisionEvent, DECISION_TIMEOUT_MS)
 *       | TIMEOUT --> LOG WARN, queue_release, local_run
 *       | SIGNALLED
 *       v
 *   [6] read decision
 *       | NO / FALLBACK --> local_run
 *       | YES
 *       v
 *   [7] g_transfer()  if linked
 *       | not linked / FALSE --> LOG ERROR, local_run
 *       | TRUE --> return TRUE (remote execution started)
 *
 * Build (MSVC x64 Developer Command Prompt):
 *   cl /LD /O2 /W4 hook.c /link kernel32.lib psapi.lib /out:hook.dll
 * --------------------------------------------------------------
 */

#include "hook.h"
#include <psapi.h>
#include <stdio.h>

/* ============================================================
 *  LOG PATH
 * ============================================================ */

#define HOOK_LOG_PATH  L"C:\\Temp\\hook.log"
#define LOG_BOM        0xFEFF

/* ============================================================
 *  LOG LEVELS
 * ============================================================ */

typedef enum { LOG_INFO=0, LOG_WARN=1, LOG_ERROR=2, LOG_TRACE=3 } LogLevel;
static const WCHAR *LOG_TAG[] = { L"INFO ", L"WARN ", L"ERROR", L"TRACE" };

/* ============================================================
 *  HOOK STATE
 * ============================================================ */

typedef BOOL (WINAPI *PFN_CreateProcessW)(
    LPCWSTR, LPWSTR,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR,
    LPSTARTUPINFOW, LPPROCESS_INFORMATION
);

static PFN_CreateProcessW g_real_cpw      = NULL;
static AskEngineFn        g_ask           = NULL;
static TransferEngineFn   g_transfer      = NULL;
static BOOL               g_ready         = FALSE;
static CRITICAL_SECTION   g_log_cs;
static LARGE_INTEGER      g_qpc_freq      = {0};

/* Full path to hook.dll — resolved at attach so inject_into_child
 * can pass it to LoadLibraryW in every child process.            */
static WCHAR g_hook_dll_path[MAX_PATH];

/* ============================================================
 *  INTERCEPT QUEUE
 * ============================================================ */

static QueueEntry g_queue[HOOK_QUEUE_CAPACITY];

/* ============================================================
 *  RUNTIME FILTER LIST
 *  Populated at init from HOOK_SYSTEM_NAMES.
 *  Extended at runtime via hook_filter_add_name().
 * ============================================================ */

static WCHAR  g_filter_names[HOOK_FILTER_DYNAMIC_CAP][MAX_PATH];
static int    g_filter_count = 0;
static CRITICAL_SECTION g_filter_cs;

/* System32 and SysWOW64 canonical paths, resolved once at attach. */
static WCHAR g_sys32[MAX_PATH];
static WCHAR g_syswow[MAX_PATH];

/* ============================================================
 *  LOGGING
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
    __except (EXCEPTION_EXECUTE_HANDLER) { }
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

#define LOG(lv, fmt, ...) \
    emit(lv, GetCurrentProcessId(), L"hook.dll", fmt, ##__VA_ARGS__)

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
 *  FILTER INITIALISATION
 *  Loads compile-time names and resolves system directory paths.
 * ============================================================ */

static void filter_init(void)
{
    InitializeCriticalSection(&g_filter_cs);

    /* Compile-time system process names */
    static const WCHAR *builtin[] = HOOK_SYSTEM_NAMES;
    for (int i = 0; builtin[i] && g_filter_count < HOOK_FILTER_DYNAMIC_CAP; i++) {
        wcsncpy_s(g_filter_names[g_filter_count],
                  MAX_PATH, builtin[i], _TRUNCATE);
        g_filter_count++;
    }

    /* Resolve canonical System32 and SysWOW64 paths */
    GetSystemDirectoryW(g_sys32, ARRAYSIZE(g_sys32));
    /* SysWOW64 only exists on 64-bit Windows */
    WCHAR win[MAX_PATH];
    GetWindowsDirectoryW(win, ARRAYSIZE(win));
    _snwprintf_s(g_syswow, ARRAYSIZE(g_syswow), _TRUNCATE,
                 L"%s\\SysWOW64", win);

    LOG(LOG_INFO, L"Filter: %d system names loaded", g_filter_count);
    LOG(LOG_INFO, L"Filter: System32  = %s", g_sys32);
    LOG(LOG_INFO, L"Filter: SysWOW64  = %s", g_syswow);
}

static void filter_teardown(void)
{
    DeleteCriticalSection(&g_filter_cs);
}

/* ============================================================
 *  is_offload_candidate
 *
 *  Returns TRUE  — process should enter the queue.
 *  Returns FALSE — process is definitively OS/pre-GUI; skip.
 *
 *  Checks (in order, cheapest first):
 *    1. Creation flags suggest pure background helper.
 *    2. Basename matches the filter name list.
 *    3. Full path is under System32 or SysWOW64.
 * ============================================================ */

static BOOL is_offload_candidate(const ProcessProfile *p)
{
    /* ── 1. Background-only creation flags ─────────────────────
     * CREATE_NO_WINDOW + DETACHED_PROCESS without CREATE_NEW_CONSOLE
     * is the fingerprint of a headless service helper.  These can
     * never stream output back to a user meaningfully.
     */
    DWORD bg_flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
    if ((p->creation_flags & bg_flags) == bg_flags &&
        !(p->creation_flags & CREATE_NEW_CONSOLE))
    {
        PLOG(LOG_TRACE, p,
             L"Filter: SKIP '%s' — background-only flags 0x%08X",
             p->name, p->creation_flags);
        return FALSE;
    }

    /* ── 2. Name-list match ─────────────────────────────────── */
    EnterCriticalSection(&g_filter_cs);
    for (int i = 0; i < g_filter_count; i++) {
        if (_wcsicmp(p->name, g_filter_names[i]) == 0) {
            LeaveCriticalSection(&g_filter_cs);
            PLOG(LOG_TRACE, p,
                 L"Filter: SKIP '%s' — matches system name list",
                 p->name);
            return FALSE;
        }
    }
    LeaveCriticalSection(&g_filter_cs);

    /* ── 3. Path under System32 / SysWOW64 ─────────────────── */
    if (p->full_path[0]) {
        if (g_sys32[0] &&
            _wcsnicmp(p->full_path, g_sys32, wcslen(g_sys32)) == 0)
        {
            PLOG(LOG_TRACE, p,
                 L"Filter: SKIP '%s' — path under System32",
                 p->name);
            return FALSE;
        }
        if (g_syswow[0] &&
            _wcsnicmp(p->full_path, g_syswow, wcslen(g_syswow)) == 0)
        {
            PLOG(LOG_TRACE, p,
                 L"Filter: SKIP '%s' — path under SysWOW64",
                 p->name);
            return FALSE;
        }
    }

    return TRUE;  /* candidate — let the queue and AskEngine decide */
}

/* ============================================================
 *  EXPORTED: hook_filter_add_name
 *  Called by offload_hook.cpp when the leader permanently marks
 *  a binary as local-only.
 * ============================================================ */

__declspec(dllexport)
BOOL WINAPI hook_filter_add_name(LPCWSTR name)
{
    if (!name || !name[0]) return FALSE;

    EnterCriticalSection(&g_filter_cs);
    if (g_filter_count >= HOOK_FILTER_DYNAMIC_CAP) {
        LeaveCriticalSection(&g_filter_cs);
        LOG(LOG_WARN,
            L"hook_filter_add_name: list full (%d) — cannot add '%s'",
            HOOK_FILTER_DYNAMIC_CAP, name);
        return FALSE;
    }
    wcsncpy_s(g_filter_names[g_filter_count],
              MAX_PATH, name, _TRUNCATE);
    g_filter_count++;
    LeaveCriticalSection(&g_filter_cs);

    LOG(LOG_INFO, L"hook_filter_add_name: added '%s' (%d total)",
        name, g_filter_count);
    return TRUE;
}

/* ============================================================
 *  PROCESS PROFILE BUILDER
 * ============================================================ */

static ProcessProfile build_profile(LPCWSTR app, LPWSTR cmd,
                                    LPCWSTR cwd, BOOL inherit,
                                    DWORD flags)
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
        wcsncpy_s(p.caller_name, ARRAYSIZE(p.caller_name),
                  L"unknown", _TRUNCATE);
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
 * ============================================================ */

static void log_profile(const ProcessProfile *p)
{
    PLOG(LOG_TRACE, p, L"--- Caught Process ---");
    PLOG(LOG_TRACE, p, L"  name           : %s", p->name);
    PLOG(LOG_TRACE, p, L"  full_path      : %s",
         p->full_path[0] ? p->full_path : L"(unresolved)");
    PLOG(LOG_TRACE, p, L"  cmdline        : %s",
         p->cmdline[0]   ? p->cmdline   : L"(none)");
    PLOG(LOG_TRACE, p, L"  cwd            : %s",
         p->cwd[0]       ? p->cwd       : L"(none)");
    PLOG(LOG_TRACE, p, L"  creation_flags : 0x%08X%s%s%s",
         p->creation_flags,
         (p->creation_flags & CREATE_SUSPENDED)   ? L" SUSPENDED"   : L"",
         (p->creation_flags & CREATE_NEW_CONSOLE) ? L" NEW_CONSOLE" : L"",
         (p->creation_flags & DETACHED_PROCESS)   ? L" DETACHED"    : L"");
    PLOG(LOG_TRACE, p, L"  inherit_handles: %s",
         p->inherit_handles ? L"yes" : L"no");
    PLOG(LOG_TRACE, p, L"  has_gui        : %s",
         p->has_gui ? L"yes" : L"no");
    PLOG(LOG_TRACE, p, L"  caller.pid     : %lu", p->caller_pid);
    PLOG(LOG_TRACE, p, L"  caller.name    : %s",  p->caller_name);
    PLOG(LOG_TRACE, p, L"  cpu_count      : %lu", p->local_cpu_count);
    PLOG(LOG_TRACE, p, L"  phys_mem_mb    : %llu", p->local_phys_mem_mb);
    PLOG(LOG_TRACE, p, L"  avail_mem_mb   : %llu", p->local_avail_mem_mb);
    PLOG(LOG_TRACE, p, L"--- End Profile ---");
}

/* ============================================================
 *  QUEUE HELPERS
 * ============================================================ */

static QueueEntry *queue_claim(void)
{
    for (int i = 0; i < HOOK_QUEUE_CAPACITY; i++) {
        if (InterlockedCompareExchange(&g_queue[i].in_use, 1, 0) == 0)
            return &g_queue[i];
    }
    return NULL;
}

static void queue_release(QueueEntry *e)
{
    InterlockedExchange(&e->in_use, 0);
}

/* ============================================================
 *  CHILD PROCESS INJECTION
 *
 *  Propagates hook.dll into a newly spawned child process so
 *  that every process the child launches is also intercepted.
 *
 *  Strategy:
 *    1. Ensure child was created suspended (we add the flag).
 *    2. Allocate a page in the child and write the hook.dll path.
 *    3. CreateRemoteThread(LoadLibraryW, path) — this loads
 *       hook.dll into the child at its earliest opportunity.
 *    4. Wait for the loader thread to finish.
 *    5. Resume the child's main thread.
 *
 *  If anything fails the child is simply resumed without the
 *  hook — it will run locally and untracked, which is safe.
 *
 *  Called from local_run_with_inject for every candidate process
 *  (i.e. processes that passed is_offload_candidate but ran
 *  locally due to timeout / NO decision / missing engines).
 * ============================================================ */

static void inject_into_child(HANDLE hProcess, HANDLE hMainThread,
                               const ProcessProfile *p)
{
    if (!g_hook_dll_path[0]) {
        PLOG(LOG_WARN, p,
             L"Inject child: hook.dll path unknown — skipping '%s'",
             p->name);
        return;
    }

    __try {
        SIZE_T path_bytes = (wcslen(g_hook_dll_path) + 1) * sizeof(WCHAR);

        /* Allocate writable memory in the child for the DLL path */
        LPVOID remote_buf = VirtualAllocEx(
            hProcess, NULL, path_bytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (!remote_buf) {
            PLOG(LOG_ERROR, p,
                 L"Inject child: VirtualAllocEx failed (err %lu) — '%s'"
                 L" runs unhooked",
                 GetLastError(), p->name);
            return;
        }

        /* Write hook.dll path into the child */
        SIZE_T written = 0;
        if (!WriteProcessMemory(hProcess, remote_buf,
                                g_hook_dll_path, path_bytes, &written)
            || written != path_bytes)
        {
            PLOG(LOG_ERROR, p,
                 L"Inject child: WriteProcessMemory failed (err %lu)"
                 L" — '%s' runs unhooked",
                 GetLastError(), p->name);
            VirtualFreeEx(hProcess, remote_buf, 0, MEM_RELEASE);
            return;
        }

        /* Resolve LoadLibraryW in kernel32 — address is the same
         * across processes on the same OS/bitness.               */
        FARPROC load_lib = GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");

        if (!load_lib) {
            PLOG(LOG_ERROR, p,
                 L"Inject child: cannot resolve LoadLibraryW — '%s'"
                 L" runs unhooked", p->name);
            VirtualFreeEx(hProcess, remote_buf, 0, MEM_RELEASE);
            return;
        }

        /* Spawn loader thread inside the child */
        HANDLE hLoader = CreateRemoteThread(
            hProcess, NULL, 0,
            (LPTHREAD_START_ROUTINE)load_lib,
            remote_buf, 0, NULL);

        if (!hLoader) {
            PLOG(LOG_ERROR, p,
                 L"Inject child: CreateRemoteThread failed (err %lu)"
                 L" — '%s' runs unhooked",
                 GetLastError(), p->name);
            VirtualFreeEx(hProcess, remote_buf, 0, MEM_RELEASE);
            return;
        }

        /* Wait for hook.dll to finish loading in the child.
         * Use a generous timeout — DLL_PROCESS_ATTACH is fast. */
        DWORD wait = WaitForSingleObject(hLoader, 2000);
        if (wait == WAIT_TIMEOUT) {
            PLOG(LOG_WARN, p,
                 L"Inject child: loader thread timed out for '%s'"
                 L" — resuming anyway", p->name);
        } else {
            PLOG(LOG_INFO, p,
                 L"Inject child: hook.dll loaded into '%s' (PID %lu)",
                 p->name, GetProcessId(hProcess));
        }

        CloseHandle(hLoader);
        VirtualFreeEx(hProcess, remote_buf, 0, MEM_RELEASE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        PLOG(LOG_ERROR, p,
             L"Inject child: exception 0x%08X for '%s' — resuming unhooked",
             GetExceptionCode(), p->name);
    }
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
        /* ── 1. Build profile ───────────────────────────────── */
        ProcessProfile prof = build_profile(
            lpApplicationName, lpCommandLine,
            lpCurrentDirectory, bInheritHandles, dwCreationFlags);

        /* ── 2. Pre-queue OS filter ─────────────────────────── */
        if (!is_offload_candidate(&prof)) {
            /* Logged at TRACE level inside is_offload_candidate.
             * Pass straight through — no queue slot consumed.   */
            goto local_run;
        }

        /* Candidate: log at INFO now that we know it's interesting */
        PLOG(LOG_INFO, &prof, L"Caught: '%s'", prof.name);
        log_profile(&prof);

        /* ── 3. Claim a queue slot ──────────────────────────── */
        QueueEntry *entry = queue_claim();
        if (!entry) {
            PLOG(LOG_ERROR, &prof,
                 L"Queue full (%d slots) — '%s' runs locally",
                 HOOK_QUEUE_CAPACITY, prof.name);
            goto local_run;
        }

        /* ── 4. Fill the slot ───────────────────────────────── */
        entry->profile              = prof;
        entry->lpApplicationName    = lpApplicationName;
        entry->lpCommandLine        = lpCommandLine;
        entry->lpProcessAttributes  = lpProcessAttributes;
        entry->lpThreadAttributes   = lpThreadAttributes;
        entry->bInheritHandles      = bInheritHandles;
        entry->dwCreationFlags      = dwCreationFlags;
        entry->lpEnvironment        = lpEnvironment;
        entry->lpCurrentDirectory   = lpCurrentDirectory;
        entry->lpStartupInfo        = lpStartupInfo;
        entry->lpProcessInformation = lpProcessInformation;
        entry->decision             = OFFLOAD_FALLBACK;
        entry->target_node[0]       = L'\0';

        PLOG(LOG_INFO, &prof,
             L"Queued: '%s' (slot %d, capacity %d)",
             prof.name, (int)(entry - g_queue), HOOK_QUEUE_CAPACITY);

        /* ── 5. AskEngine ─────────────────────────────────────
         *  LINK: when AskEngine is ready, g_ask will be non-NULL.
         *        It must signal entry->hDecisionEvent when done.
         * ──────────────────────────────────────────────────── */
        if (!g_ask) {
            PLOG(LOG_ERROR, &prof,
                 L"AskEngine not linked — '%s' will time out (%d ms)"
                 L" and run locally",
                 prof.name, DECISION_TIMEOUT_MS);
        } else {
            g_ask(entry);
        }

        /* ── 6. Wait for decision or timeout ────────────────── */
        DWORD wait_res = WaitForSingleObject(
            entry->hDecisionEvent, DECISION_TIMEOUT_MS);

        if (wait_res == WAIT_TIMEOUT) {
            PLOG(LOG_WARN, &prof,
                 L"Timeout (%d ms) — '%s' runs locally",
                 DECISION_TIMEOUT_MS, prof.name);
            queue_release(entry);
            goto local_run;
        }

        /* ── 7. Act on decision ─────────────────────────────── */
        OffloadDecision decision = entry->decision;
        WCHAR target_node[256];
        wcsncpy_s(target_node, ARRAYSIZE(target_node),
                  entry->target_node, _TRUNCATE);

        queue_release(entry);

        if (decision != OFFLOAD_YES) {
            PLOG(LOG_INFO, &prof,
                 L"AskEngine: %s — '%s' runs locally",
                 decision == OFFLOAD_NO ? L"NO" : L"FALLBACK",
                 prof.name);
            goto local_run;
        }

        PLOG(LOG_INFO, &prof,
             L"AskEngine: YES — '%s' -> '%s'",
             prof.name, target_node);

        /* ── 8. TransferEngine ────────────────────────────────
         *  LINK: when TransferEngine is ready, g_transfer will be
         *        non-NULL. Replace stub in offload_hook.cpp.
         * ──────────────────────────────────────────────────── */
        if (!g_transfer) {
            PLOG(LOG_ERROR, &prof,
                 L"TransferEngine not linked — '%s' falls back to local",
                 prof.name);
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
                 L"Transfer: SUCCESS — '%s' -> '%s' (%.1f ms, total %.1f ms)",
                 prof.name, target_node,
                 ms_since(t0), ms_since(prof.intercept_tick));
            return TRUE;
        }

        PLOG(LOG_ERROR, &prof,
             L"Transfer: FAILED — '%s' after %.1f ms — local fallback",
             prof.name, ms_since(t0));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG(LOG_ERROR,
            L"Unhandled exception 0x%08X in hook — local fallback",
            GetExceptionCode());
    }

local_run:
    {
        /* Launch the child suspended so we can inject hook.dll
         * before it runs.  We strip CREATE_SUSPENDED from the
         * caller's flags only if we added it ourselves.        */
        BOOL we_suspended = !(dwCreationFlags & CREATE_SUSPENDED);
        DWORD launch_flags = dwCreationFlags | CREATE_SUSPENDED;

        PROCESS_INFORMATION local_pi;
        BOOL use_local_pi = (lpProcessInformation == NULL);
        if (use_local_pi) lpProcessInformation = &local_pi;

        BOOL ok = g_real_cpw(
            lpApplicationName, lpCommandLine,
            lpProcessAttributes, lpThreadAttributes,
            bInheritHandles, launch_flags,
            lpEnvironment, lpCurrentDirectory,
            lpStartupInfo, lpProcessInformation);

        if (ok && lpProcessInformation->hProcess) {
            /* Only inject into candidate processes — OS processes
             * that were filtered out arrive here via the early
             * goto and should not be injected.                   */
            BOOL candidate = FALSE;
            __try {
                ProcessProfile tmp = build_profile(
                    lpApplicationName, lpCommandLine,
                    lpCurrentDirectory, bInheritHandles,
                    dwCreationFlags);
                candidate = is_offload_candidate(&tmp);
                if (candidate)
                    inject_into_child(
                        lpProcessInformation->hProcess,
                        lpProcessInformation->hThread,
                        &tmp);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }

            /* Resume main thread if we were the ones who suspended */
            if (we_suspended)
                ResumeThread(lpProcessInformation->hThread);
        }

        if (use_local_pi && ok) {
            CloseHandle(lpProcessInformation->hProcess);
            CloseHandle(lpProcessInformation->hThread);
        }

        return ok;
    }
}

/* ============================================================
 *  QUEUE INITIALISATION / TEARDOWN
 * ============================================================ */

static BOOL queue_init(void)
{
    for (int i = 0; i < HOOK_QUEUE_CAPACITY; i++) {
        ZeroMemory(&g_queue[i], sizeof(QueueEntry));
        g_queue[i].hDecisionEvent =
            CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!g_queue[i].hDecisionEvent) {
            LOG(LOG_ERROR,
                L"queue_init: CreateEvent failed slot %d (err %lu)",
                i, GetLastError());
            return FALSE;
        }
    }
    LOG(LOG_INFO, L"Queue initialised (%d slots)", HOOK_QUEUE_CAPACITY);
    return TRUE;
}

static void queue_teardown(void)
{
    for (int i = 0; i < HOOK_QUEUE_CAPACITY; i++) {
        if (g_queue[i].hDecisionEvent) {
            CloseHandle(g_queue[i].hDecisionEvent);
            g_queue[i].hDecisionEvent = NULL;
        }
    }
    LOG(LOG_INFO, L"Queue torn down");
}

/* ============================================================
 *  IAT PATCH — ALL MODULES
 * ============================================================ */

static BOOL patch_one_module(HMODULE hMod, FARPROC real_cpw,
                              HMODULE hSelf)
{
    if (!hMod || hMod == hSelf) return FALSE;

    __try {
        BYTE *base = (BYTE *)hMod;
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

        IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir->VirtualAddress || !dir->Size) return FALSE;

        IMAGE_IMPORT_DESCRIPTOR *imp =
            (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

        BOOL patched = FALSE;
        for (; imp->Name; imp++) {
            if (_stricmp((char *)(base + imp->Name), "kernel32.dll") != 0)
                continue;

            IMAGE_THUNK_DATA *thunk =
                (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

            for (; thunk->u1.Function; thunk++) {
                FARPROC *fn = (FARPROC *)&thunk->u1.Function;
                if (*fn != real_cpw) continue;

                DWORD old;
                VirtualProtect(fn, sizeof(*fn), PAGE_READWRITE, &old);
                *fn = (FARPROC)Hook_CreateProcessW;
                VirtualProtect(fn, sizeof(*fn), old, &old);
                patched = TRUE;
                break;
            }
            if (patched) break;
        }
        return patched;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; }
}

static BOOL patch_all_modules(void)
{
    FARPROC real_cpw = GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "CreateProcessW");
    if (!real_cpw) {
        LOG(LOG_ERROR,
            L"patch_all_modules: cannot resolve CreateProcessW");
        return FALSE;
    }

    HMODULE hSelf = NULL;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)patch_all_modules, &hSelf);

    HMODULE mods[512]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(),
                            mods, sizeof(mods), &needed)) {
        LOG(LOG_ERROR,
            L"patch_all_modules: EnumProcessModules failed (err %lu)"
            L" — EXE-only fallback", GetLastError());
        BOOL ok = patch_one_module(
            GetModuleHandleW(NULL), real_cpw, hSelf);
        if (ok) LOG(LOG_INFO, L"IAT patch applied (EXE only, fallback)");
        return ok;
    }

    DWORD mod_count = needed / sizeof(HMODULE);
    int   patched   = 0;

    for (DWORD i = 0; i < mod_count; i++) {
        if (!patch_one_module(mods[i], real_cpw, hSelf)) continue;
        WCHAR name[MAX_PATH] = L"(unknown)";
        GetModuleFileNameW(mods[i], name, ARRAYSIZE(name));
        WCHAR *leaf = wcsrchr(name, L'\\');
        LOG(LOG_INFO, L"IAT patch applied — %s",
            leaf ? leaf+1 : name);
        patched++;
    }

    if (patched == 0) {
        LOG(LOG_WARN,
            L"patch_all_modules: no modules patched");
        return FALSE;
    }

    LOG(LOG_INFO, L"patch_all_modules: %d module(s) patched", patched);
    return TRUE;
}

/* ============================================================
 *  EXPORTED API
 * ============================================================ */

__declspec(dllexport)
BOOL WINAPI HookInit(AskEngineFn ask_engine,
                     TransferEngineFn transfer_engine)
{
    g_ask      = ask_engine;
    g_transfer = transfer_engine;

    LOG(LOG_INFO, L"HookInit: AskEngine=%p  TransferEngine=%p",
        (void *)ask_engine, (void *)transfer_engine);

    if (!g_ask)
        LOG(LOG_ERROR,
            L"HookInit: AskEngine not linked"
            L" — intercepts time out after %d ms then run locally",
            DECISION_TIMEOUT_MS);

    if (!g_transfer)
        LOG(LOG_ERROR,
            L"HookInit: TransferEngine not linked — offload disabled");

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
            CreateDirectoryW(L"C:\\Temp", NULL);

            LOG(LOG_INFO, L"hook.dll attached to PID %lu",
                GetCurrentProcessId());

            filter_init();

            if (!queue_init()) {
                LOG(LOG_ERROR,
                    L"Queue init failed — hook will not be installed");
                return TRUE;
            }

            g_real_cpw = (PFN_CreateProcessW)GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"), "CreateProcessW");

            if (!g_real_cpw) {
                LOG(LOG_ERROR,
                    L"Cannot resolve kernel32!CreateProcessW"
                    L" — hook will not be installed");
                return TRUE;
            }

            g_ready = patch_all_modules();

            LOG(LOG_INFO,
                L"Ready (queue: %d slots, timeout: %d ms,"
                L" filter: %d names) — call HookInit() to supply engines",
                HOOK_QUEUE_CAPACITY, DECISION_TIMEOUT_MS,
                g_filter_count);
            break;

        case DLL_PROCESS_DETACH:
            queue_teardown();
            filter_teardown();
            LOG(LOG_INFO, L"hook.dll detached from PID %lu",
                GetCurrentProcessId());
            DeleteCriticalSection(&g_log_cs);
            break;
    }

    return TRUE;
}
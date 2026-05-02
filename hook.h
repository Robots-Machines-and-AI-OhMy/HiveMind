/*
 * hook.h
 * --------------------------------------------------------------
 * Public interface for hook.dll.
 *
 * Shared by hook.c, offload_hook.cpp, and any future engine TU
 * that needs ProcessProfile, QueueEntry, or the engine typedefs.
 *
 * Also owns the non-OS filter definitions so both hook.c and
 * offload_hook.cpp agree on what is skippable without a network
 * round-trip to the leader.
 *
 * SYSTEM-WIDE INJECTION (AppInit_DLLs):
 *   When hook.dll is registered under AppInit_DLLs, Windows loads
 *   it into every new process that loads user32.dll — covering all
 *   interactive desktop applications system-wide.
 *
 *   The IAT patch fires at DLL_PROCESS_ATTACH before any app code
 *   runs.  HookInit() must still be called (by offload_hook.dll or
 *   any loader) to supply the engine function pointers.  Until then
 *   every non-OS intercept queues, times out after
 *   DECISION_TIMEOUT_MS, and runs locally — safe by design.
 *
 *   Use install_hook.exe to write/clear the registry keys.
 *   Requires Administrator.
 * --------------------------------------------------------------
 */

#ifndef HOOK_H
#define HOOK_H

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  TUNABLES
 * ============================================================ */

/* Max time Hook_CreateProcessW blocks waiting for AskEngine. */
#define DECISION_TIMEOUT_MS   200

/* Fixed queue depth — slots are claimed atomically.           */
#define HOOK_QUEUE_CAPACITY   64

/* ============================================================
 *  NON-OS FILTER
 *
 *  Processes that are definitively unoffloadable are filtered
 *  out in hook.c before they ever touch the queue.  The filter
 *  is intentionally conservative (false-negatives pass through
 *  to AskEngine; false-positives waste a queue slot but are
 *  safe).
 *
 *  A process is considered an OS/pre-GUI process if ANY of:
 *    1. Its full path is under System32 or SysWOW64.
 *    2. Its basename matches a known Windows system process.
 *    3. It carries no visible-window intent
 *       (CREATE_NO_WINDOW | DETACHED_PROCESS and not
 *        CREATE_NEW_CONSOLE) — these are background service
 *        helpers that have no interactive output to stream.
 *
 *  offload_hook.cpp may extend this list at runtime by calling
 *  hook_filter_add_name() if the leader reports a process as
 *  permanently local.
 * ============================================================ */

/* Compile-time list of basenames that are always local.
 * Lower-case; comparison is case-insensitive.                  */
#define HOOK_SYSTEM_NAMES {         \
    /* Windows core processes */    \
    L"svchost.exe",                 \
    L"csrss.exe",                   \
    L"smss.exe",                    \
    L"wininit.exe",                 \
    L"winlogon.exe",                \
    L"services.exe",                \
    L"lsass.exe",                   \
    L"lsaiso.exe",                  \
    L"fontdrvhost.exe",             \
    L"dwm.exe",                     \
    L"taskhostw.exe",               \
    L"runtimebroker.exe",           \
    L"sihost.exe",                  \
    L"ctfmon.exe",                  \
    L"conhost.exe",                 \
    L"dllhost.exe",                 \
    L"msiexec.exe",                 \
    L"wuauclt.exe",                 \
    L"spoolsv.exe",                 \
    L"searchindexer.exe",           \
    /* Build tools — never offloadable, suppress log noise */  \
    L"cmake.exe",                   \
    L"ninja.exe",                   \
    L"cl.exe",                      \
    L"link.exe",                    \
    L"lib.exe",                     \
    L"rc.exe",                      \
    L"mt.exe",                      \
    L"msbuild.exe",                 \
    L"devenv.exe",                  \
    L"git.exe",                     \
    L"git-remote-https.exe",        \
    L"sh.exe",                      \
    L"bash.exe",                    \
    NULL                            \
}

/* Must be >= len(HOOK_SYSTEM_NAMES) + runtime additions.       */
#define HOOK_FILTER_DYNAMIC_CAP  128

/* ============================================================
 *  PROCESS PROFILE
 * ============================================================ */

typedef struct ProcessProfile {
    WCHAR     name[MAX_PATH];
    WCHAR     full_path[MAX_PATH];
    WCHAR     cmdline[1024];
    WCHAR     cwd[MAX_PATH];
    DWORD     creation_flags;
    BOOL      inherit_handles;
    BOOL      has_gui;
    DWORD     caller_pid;
    WCHAR     caller_name[MAX_PATH];
    DWORD     local_cpu_count;
    DWORDLONG local_phys_mem_mb;
    DWORDLONG local_avail_mem_mb;
    LARGE_INTEGER intercept_tick;
} ProcessProfile;

/* ============================================================
 *  OFFLOAD DECISION
 * ============================================================ */

typedef enum OffloadDecision {
    OFFLOAD_YES      = 0,
    OFFLOAD_NO       = 1,
    OFFLOAD_FALLBACK = 2,
} OffloadDecision;

/* ============================================================
 *  QUEUE ENTRY
 * ============================================================ */

typedef struct QueueEntry {
    ProcessProfile        profile;
    LPCWSTR               lpApplicationName;
    LPWSTR                lpCommandLine;
    LPSECURITY_ATTRIBUTES lpProcessAttributes;
    LPSECURITY_ATTRIBUTES lpThreadAttributes;
    BOOL                  bInheritHandles;
    DWORD                 dwCreationFlags;
    LPVOID                lpEnvironment;
    LPCWSTR               lpCurrentDirectory;
    LPSTARTUPINFOW        lpStartupInfo;
    LPPROCESS_INFORMATION lpProcessInformation;
    volatile OffloadDecision decision;
    WCHAR                    target_node[256];
    HANDLE                   hDecisionEvent;
    volatile LONG            in_use;
} QueueEntry;

/* ============================================================
 *  ENGINE FUNCTION POINTER TYPES
 * ============================================================ */

/*
 * AskEngineFn
 * Must fill entry->decision + entry->target_node (if YES),
 * then signal entry->hDecisionEvent.
 * Must not throw. Must not modify entry->profile.
 * LINK: replace stub in offload_hook.cpp with real fn ptr.
 */
typedef void (WINAPI *AskEngineFn)(QueueEntry *entry);

/*
 * TransferEngineFn
 * Called only when AskEngine sets OFFLOAD_YES.
 * Returns TRUE on success (hook returns TRUE to caller).
 * Must not throw. Must not modify *profile.
 * LINK: replace stub in offload_hook.cpp with real fn ptr.
 */
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
 *  EXPORTED HOOK FUNCTIONS
 * ============================================================ */

typedef BOOL (WINAPI *FnHookInit)(
    AskEngineFn      ask_engine,
    TransferEngineFn transfer_engine
);
typedef void (WINAPI *FnHookTeardown)(void);

/*
 * hook_filter_add_name
 * Exported by hook.dll.  Adds a process basename to the
 * runtime filter list so it is never enqueued again.
 * offload_hook.cpp calls this when the leader permanently
 * classifies a binary as local-only.
 * name must be a null-terminated basename, e.g. L"myapp.exe".
 * Returns TRUE on success, FALSE if the dynamic list is full.
 */
typedef BOOL (WINAPI *FnHookFilterAddName)(LPCWSTR name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HOOK_H */

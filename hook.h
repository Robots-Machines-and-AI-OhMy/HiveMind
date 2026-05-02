//
// Created by jason on 5/1/2026.
//

/*
 * hook.h
 * --------------------------------------------------------------
 * Public interface for hook.dll.
 *
 * Include this in any C or C++ translation unit that needs to
 * load or interact with the hook at runtime.
 *
 * hook.dll exports two functions:
 *   HookInit()     — install engines and activate interception.
 *   HookTeardown() — clear engines (process launches pass through).
 * --------------------------------------------------------------
 */

#ifndef MINDMESH_HOOK_H
#define MINDMESH_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

/* ============================================================
 *  PROCESS PROFILE
 *  Snapshot of a process at the moment of interception.
 *  Built by hook.dll; passed read-only to both engines.
 *  Neither engine may modify this struct.
 * ============================================================ */

typedef struct ProcessProfile {
    /* Target process */
    WCHAR     name[MAX_PATH];         /* basename, e.g. "notepad.exe"   */
    WCHAR     full_path[MAX_PATH];    /* full path if resolvable         */
    WCHAR     cmdline[1024];          /* truncated command line          */
    WCHAR     cwd[MAX_PATH];          /* working directory               */
    DWORD     creation_flags;         /* raw dwCreationFlags             */
    BOOL      inherit_handles;
    BOOL      has_gui;                /* heuristic from creation flags   */

    /* Caller */
    DWORD     caller_pid;
    WCHAR     caller_name[MAX_PATH];  /* basename of calling process     */

    /* Local hardware snapshot (for offload-ability context) */
    DWORD     local_cpu_count;
    DWORDLONG local_phys_mem_mb;
    DWORDLONG local_avail_mem_mb;

    /* Timing — QPC tick at hook entry */
    LARGE_INTEGER intercept_tick;
} ProcessProfile;

/* ============================================================
 *  OFFLOAD DECISION
 *  Returned by AskEngine to tell the hook what to do.
 * ============================================================ */

typedef enum OffloadDecision {
    OFFLOAD_YES      = 0,   /* ship to remote node               */
    OFFLOAD_NO       = 1,   /* run locally, nothing to do        */
    OFFLOAD_FALLBACK = 2,   /* engine error — hook runs locally  */
} OffloadDecision;

/* ============================================================
 *  ENGINE FUNCTION POINTER TYPES
 *
 *  AskEngineFn
 *  -----------
 *  Called by the hook for every intercepted process launch.
 *  Must decide whether the process should be offloaded.
 *
 *  Parameters:
 *    profile          — read-only snapshot of the process.
 *    target_node_out  — caller-allocated buffer [256 wchar_t].
 *                       Engine writes the remote node address
 *                       (e.g. L"192.168.1.5:9000") when returning
 *                       OFFLOAD_YES.  Ignored otherwise.
 *    target_node_len  — length of target_node_out in wchar_t.
 *
 *  Returns:
 *    OFFLOAD_YES      — offload; target_node_out is valid.
 *    OFFLOAD_NO       — run locally.
 *    OFFLOAD_FALLBACK — engine error; hook falls back to local.
 *
 *  Contract:
 *    - Must not throw or raise a structured exception.
 *    - Must return within its own timeout (no blocking forever).
 *    - Must not modify *profile.
 *
 *  Link: /* LINK AskEngine implementation here *
 *
 *
 *  TransferEngineFn
 *  ----------------
 *  Called by the hook when AskEngine returns OFFLOAD_YES.
 *  Responsible for the full remote execution lifecycle:
 *  bundle, transport, surrogate handle, and stream pump.
 *
 *  Parameters:
 *    profile          — read-only snapshot of the process.
 *    target_node      — node address from AskEngine.
 *    remaining args   — original CreateProcessW arguments,
 *                       passed through verbatim so the engine
 *                       can populate lpProcessInformation with
 *                       a surrogate handle for the caller.
 *
 *  Returns:
 *    TRUE   — remote execution started; hook returns TRUE to caller.
 *    FALSE  — transfer failed; hook falls back to local run.
 *
 *  Contract:
 *    - Must not throw or raise a structured exception.
 *    - Must populate lpProcessInformation on TRUE return.
 *    - Must not modify *profile.
 *
 *  Link: /* LINK TransferEngine implementation here *
 *
 * ============================================================ */

typedef OffloadDecision (WINAPI *AskEngineFn)(
    const ProcessProfile *profile,
    WCHAR                *target_node_out,
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
 *  EXPORTED HOOK FUNCTIONS
 *  Resolved at runtime via GetProcAddress after LoadLibrary.
 *  Declare as function pointer types for explicit late-binding.
 * ============================================================ */

/*
 * HookInit
 *
 * Supplies the two engine function pointers to hook.dll.
 * Call once after LoadLibrary("hook.dll").
 *
 * Either engine may be NULL — the hook will log LOG_ERROR
 * and fall back to local execution for every interception.
 *
 * Returns: TRUE if the IAT patch was successfully applied.
 */
typedef BOOL (WINAPI *FnHookInit)(
    AskEngineFn      ask_engine,
    TransferEngineFn transfer_engine
);

/*
 * HookTeardown
 *
 * Clears engine pointers inside hook.dll.
 * After this call all intercepted launches fall through locally.
 * Call before FreeLibrary("hook.dll").
 */
typedef void (WINAPI *FnHookTeardown)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif //MINDMESH_HOOK_H
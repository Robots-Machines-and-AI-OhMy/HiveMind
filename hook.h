//
// Created by jason on 5/1/2026.
//

/*
 * hook.h
 * --------------------------------------------------------------
 * Public interface for hook.dll.
 *
 * Shared by hook.c, injector.cpp, and any future engine TU
 * that needs ProcessProfile or the engine typedefs.
 *
 * hook.dll exports exactly two symbols:
 *   HookInit()     — supply engines, activate interception.
 *   HookTeardown() — clear engines, launches pass through.
 * --------------------------------------------------------------
 */

#ifndef HOOK_H
#define HOOK_H

/* Windows headers must come first, before any other includes. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  PROCESS PROFILE
 *  Snapshot of a process at the moment of interception.
 *  Built entirely by hook.dll from the CreateProcessW args
 *  and local system queries.
 *  Passed read-only to both engines — neither may modify it.
 * ============================================================ */

typedef struct ProcessProfile {
    /* Target process */
    WCHAR     name[MAX_PATH];         /* basename e.g. "notepad.exe"    */
    WCHAR     full_path[MAX_PATH];    /* full path when resolvable       */
    WCHAR     cmdline[1024];          /* command line (truncated)        */
    WCHAR     cwd[MAX_PATH];          /* working directory               */
    DWORD     creation_flags;         /* raw dwCreationFlags             */
    BOOL      inherit_handles;
    BOOL      has_gui;                /* heuristic from creation flags   */

    /* Calling process */
    DWORD     caller_pid;
    WCHAR     caller_name[MAX_PATH];  /* basename of calling process     */

    /* Local hardware snapshot — informs offload-ability */
    DWORD     local_cpu_count;
    DWORDLONG local_phys_mem_mb;
    DWORDLONG local_avail_mem_mb;

    /* QPC tick recorded at hook entry */
    LARGE_INTEGER intercept_tick;
} ProcessProfile;

/* ============================================================
 *  OFFLOAD DECISION
 *  Returned by AskEngine to the hook.
 * ============================================================ */

typedef enum OffloadDecision {
    OFFLOAD_YES      = 0,   /* ship to remote node                */
    OFFLOAD_NO       = 1,   /* run locally, nothing to do         */
    OFFLOAD_FALLBACK = 2,   /* engine error — hook falls back     */
} OffloadDecision;

/* ============================================================
 *  ENGINE FUNCTION POINTER TYPES
 * ============================================================ */

/*
 * AskEngineFn
 * -----------
 * Decides whether a caught process should be offloaded.
 *
 * profile         — read-only; built by hook.dll.
 * target_node_out — [256 wchar_t] buffer; engine writes the
 *                   remote node address (e.g. L"10.0.0.5:9000")
 *                   when returning OFFLOAD_YES.
 * target_node_len — capacity of target_node_out in wchar_t.
 *
 * Returns OFFLOAD_YES / OFFLOAD_NO / OFFLOAD_FALLBACK.
 * Must not throw. Must not modify *profile.
 *
 * LINK: replace stub in injector.cpp with real AskEngine fn ptr.
 */
typedef OffloadDecision (WINAPI *AskEngineFn)(
    const ProcessProfile *profile,
    WCHAR                *target_node_out,
    DWORD                 target_node_len
);

/*
 * TransferEngineFn
 * ----------------
 * Ships the process to the remote node and sets up I/O streams.
 * Called only when AskEngine returns OFFLOAD_YES.
 *
 * profile      — read-only; built by hook.dll.
 * target_node  — address string from AskEngine.
 * remaining    — original CreateProcessW arguments, passed
 *                verbatim so the engine can fill
 *                lpProcessInformation with a surrogate handle.
 *
 * Returns TRUE  → remote execution started; hook returns TRUE.
 *         FALSE → transfer failed; hook falls back to local.
 * Must not throw. Must not modify *profile.
 *
 * LINK: replace stub in injector.cpp with real TransferEngine fn ptr.
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
 *  Resolved at runtime after LoadLibrary("hook.dll").
 *  Typed as function pointers for explicit late-binding.
 * ============================================================ */

/*
 * HookInit — supply engines, complete activation.
 * Call once after LoadLibrary("hook.dll").
 * Either pointer may be NULL; hook logs ERROR and falls back.
 * Returns TRUE if the IAT patch was applied successfully.
 */
typedef BOOL (WINAPI *FnHookInit)(
    AskEngineFn      ask_engine,
    TransferEngineFn transfer_engine
);

/*
 * HookTeardown — clear engine pointers.
 * All subsequent intercepts pass straight through to local.
 * Call before FreeLibrary("hook.dll").
 */
typedef void (WINAPI *FnHookTeardown)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HOOK_H */
//
// Created by jason on 5/1/2026.
//

/*
 * injector.cpp
 * --------------------------------------------------------------
 * Non-main component responsible for loading hook.dll and
 * supplying it with AskEngine and TransferEngine at runtime.
 *
 * This is intentionally not a main() program — it exposes
 * Inject() and Eject() for the host application to call.
 *
 * Both engines are stubs that deliberately return failure
 * values so the hook's fallback path is exercised and logged.
 *
 * Build (MSVC):
 *   cl /LD /O2 /W4 injector.cpp /link kernel32.lib /out:injector.dll
 * --------------------------------------------------------------
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>

/* ============================================================
 *  MIRROR TYPES FROM hook.c
 *  Duplicated here so injector.cpp has no header dependency
 *  on hook internals.  Keep in sync with hook.c.
 * ============================================================ */

struct ProcessProfile {
    wchar_t   name[MAX_PATH];
    wchar_t   full_path[MAX_PATH];
    wchar_t   cmdline[1024];
    wchar_t   cwd[MAX_PATH];
    DWORD     creation_flags;
    BOOL      inherit_handles;
    BOOL      has_gui;
    DWORD     caller_pid;
    wchar_t   caller_name[MAX_PATH];
    DWORD     local_cpu_count;
    DWORDLONG local_phys_mem_mb;
    DWORDLONG local_avail_mem_mb;
    LARGE_INTEGER intercept_tick;
};

enum OffloadDecision {
    OFFLOAD_YES      = 0,
    OFFLOAD_NO       = 1,
    OFFLOAD_FALLBACK = 2,
};

typedef OffloadDecision (WINAPI *AskEngineFn)(
    const ProcessProfile *profile,
    wchar_t              *target_node_out,
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

/* HookInit / HookTeardown signatures exported by hook.dll */
typedef BOOL (WINAPI *FnHookInit)(AskEngineFn, TransferEngineFn);
typedef void (WINAPI *FnHookTeardown)(void);

/* ============================================================
 *  MODULE STATE
 * ============================================================ */

static HMODULE        g_hook_dll      = NULL;
static FnHookTeardown g_teardown_fn   = NULL;

/* ============================================================
 *  AskEngine STUB
 *
 *  Real implementation: open a QUIC stream to the RAFT leader,
 *  send the profile, receive OFFLOAD_YES/NO and a target node.
 *
 *  Stub behaviour: always returns OFFLOAD_FALLBACK so that
 *  hook.c logs the error and falls back to local execution,
 *  proving the fallback path works end-to-end.
 * ============================================================ */

static OffloadDecision WINAPI AskEngine(
    const ProcessProfile *profile,
    wchar_t              *target_node_out,
    DWORD                 /*target_node_len*/)
{
    /*
     * AskEngine — NOT YET IMPLEMENTED
     *
     * When implemented this function will:
     *   1. Serialise profile fields into a capability-request packet.
     *   2. Open (or reuse) a lsQUIC stream to the RAFT leader.
     *   3. Send the packet and await a decision response.
     *   4. Write the chosen node address into target_node_out.
     *   5. Return OFFLOAD_YES, OFFLOAD_NO, or OFFLOAD_FALLBACK.
     */

    (void)profile;
    (void)target_node_out;

    /* Stub: signal that the engine is not available yet.
     * hook.c will log LOG_ERROR and run the process locally. */
    return OFFLOAD_FALLBACK;
}

/* ============================================================
 *  TransferEngine STUB
 *
 *  Real implementation: build a ProcessBundle, open lsQUIC
 *  streams to the target node, ship the bundle, and populate
 *  lpProcessInformation with a surrogate handle.
 *
 *  Stub behaviour: always returns FALSE so that hook.c logs
 *  the transfer failure and falls back to local execution.
 * ============================================================ */

static BOOL WINAPI TransferEngine(
    const ProcessProfile  *profile,
    LPCWSTR                target_node,
    LPWSTR                 /*lpCommandLine*/,
    LPSECURITY_ATTRIBUTES  /*lpProcessAttributes*/,
    LPSECURITY_ATTRIBUTES  /*lpThreadAttributes*/,
    BOOL                   /*bInheritHandles*/,
    DWORD                  /*dwCreationFlags*/,
    LPVOID                 /*lpEnvironment*/,
    LPCWSTR                /*lpCurrentDirectory*/,
    LPSTARTUPINFOW         /*lpStartupInfo*/,
    LPPROCESS_INFORMATION  /*lpProcessInformation*/)
{
    /*
     * TransferEngine — NOT YET IMPLEMENTED
     *
     * When implemented this function will:
     *   1. Walk the PE import table to collect required DLLs.
     *   2. Build a ProcessBundle (binary, deps, argv, env, cwd).
     *   3. Open lsQUIC streams to target_node:
     *        Stream 0 → control  (bundle + exit code)
     *        Stream 1 → stdin
     *        Stream 2 → stdout
     *        Stream 3 → stderr
     *        Stream 4 → framebuffer (if has_gui)
     *   4. Create a local surrogate handle for the caller.
     *   5. Return TRUE on success.
     */

    (void)profile;
    (void)target_node;

    /* Stub: signal that transfer is not available yet.
     * hook.c will log LOG_ERROR and run the process locally. */
    return FALSE;
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

/*
 * Inject()
 *
 * Loads hook.dll into the current process, resolves HookInit,
 * and wires in the two engine stubs.  Call this once from the
 * host application before any process launches you want caught.
 *
 * Returns TRUE on success.  On failure the host process
 * continues normally — hook.dll is either not loaded or the
 * IAT patch was not applied.
 */
extern "C" __declspec(dllexport)
BOOL WINAPI Inject(void)
{
    if (g_hook_dll) return TRUE;   /* already injected */

    g_hook_dll = LoadLibraryW(L"hook.dll");
    if (!g_hook_dll) {
        /* Not a crash — caller continues without interception */
        OutputDebugStringW(L"[injector] LoadLibrary(hook.dll) failed\n");
        return FALSE;
    }

    FnHookInit init_fn = (FnHookInit)
        GetProcAddress(g_hook_dll, "HookInit");

    g_teardown_fn = (FnHookTeardown)
        GetProcAddress(g_hook_dll, "HookTeardown");

    if (!init_fn) {
        OutputDebugStringW(L"[injector] HookInit not found in hook.dll\n");
        FreeLibrary(g_hook_dll);
        g_hook_dll = NULL;
        return FALSE;
    }

    /*
     * Supply the two engine stubs.
     * hook.c will log LOG_ERROR for each NULL/failing engine
     * and fall back to local execution — that IS the expected
     * behaviour at this stage.
     */
    init_fn(AskEngine, TransferEngine);
    return TRUE;
}

/*
 * Eject()
 *
 * Calls HookTeardown to clear engine pointers inside hook.dll,
 * then unloads the DLL.  After this call, CreateProcessW
 * operates normally again (IAT entry still points to hook,
 * but hook immediately falls through with NULL engines).
 *
 * For a complete IAT restore, replace the patch with Detours.
 */

extern "C" __declspec(dllexport)
void WINAPI Eject(void)
{
    if (!g_hook_dll) return;

    if (g_teardown_fn) g_teardown_fn();

    FreeLibrary(g_hook_dll);
    g_hook_dll    = NULL;
    g_teardown_fn = NULL;
}

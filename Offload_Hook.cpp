/*
 * offload_hook.cpp
 * --------------------------------------------------------------
 * Non-main component: loads hook.dll and wires in the engines.
 * (Renamed from injector.cpp — same role, clearer name.)
 *
 * Both engines are stubs that exercise the failure/timeout paths:
 *
 *   AskEngine stub      — does NOT signal hDecisionEvent.
 *                         Interceptor waits DECISION_TIMEOUT_MS,
 *                         logs WARN "Timeout", runs locally.
 *
 *   TransferEngine stub — returns FALSE.
 *                         Would log ERROR "Transfer: FAILED" if
 *                         AskEngine ever returned OFFLOAD_YES.
 *
 * Build (MSVC x64 Developer Command Prompt):
 *   cl /LD /O2 /W4 offload_hook.cpp /link kernel32.lib /out:offload_hook.dll
 * --------------------------------------------------------------
 */

#include "offload_hook.hpp"

/* ============================================================
 *  MODULE STATE
 * ============================================================ */

static HMODULE           g_hook_dll       = NULL;
static FnHookTeardown    g_teardown_fn    = NULL;
static FnHookFilterAddName g_filter_add_fn = NULL;

/* ============================================================
 *  AskEngine STUB
 *
 *  Queue-based contract:
 *    - Receives a live QueueEntry* from the intercept queue.
 *    - Must write entry->decision (and entry->target_node if YES).
 *    - Must signal entry->hDecisionEvent before returning.
 *
 *  This stub does NOT signal — the interceptor's 200 ms timeout
 *  fires, logs a WARN, and runs the process locally.
 *  This proves the timeout fallback path end-to-end.
 *
 *  LINK: replace AskEngine below in Inject() with the real fn ptr.
 * ============================================================ */

static void WINAPI AskEngine(QueueEntry *entry)
{
    /*
     * AskEngine — NOT YET IMPLEMENTED
     *
     * When implemented:
     *   1. Post entry to a dedicated worker thread.
     *   2. Worker serialises entry->profile into a request packet.
     *   3. Send packet to RAFT leader over lsQUIC.
     *   4. On leader response: write entry->decision + target_node.
     *   5. Signal entry->hDecisionEvent to unblock the interceptor.
     *
     * If the response arrives after DECISION_TIMEOUT_MS the
     * interceptor has already run locally and released the slot —
     * the engine must check entry->in_use before signalling.
     */
    (void)entry;

    /* Stub: do not signal — let the 200 ms timeout fire.
     * hook.c will log:
     *   [WARN ] Timeout (200 ms) — '<name>' runs locally        */
}

/* ============================================================
 *  TransferEngine STUB
 *
 *  LINK: replace TransferEngine below in Inject() with real fn ptr.
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
     * When implemented:
     *   1. Walk PE import table, collect required DLLs.
     *   2. Build ProcessBundle (binary, deps, argv, env, cwd).
     *   3. Open lsQUIC streams to target_node:
     *        Stream 0 -> control  (bundle + exit code)
     *        Stream 1 -> stdin
     *        Stream 2 -> stdout
     *        Stream 3 -> stderr
     *        Stream 4 -> framebuffer (if profile->has_gui)
     *   4. Create local surrogate handle for lpProcessInformation.
     *   5. Return TRUE on success.
     */
    (void)profile;
    (void)target_node;

    /* Stub: return FALSE.
     * hook.c will log:
     *   [ERROR] Transfer: FAILED — '<name>' after X ms — local fallback */
    return FALSE;
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

extern "C" {

/*
 * Inject()
 *
 * Loads hook.dll, wires in the two engine stubs.
 *
 * Expected log with stubs active:
 *   [INFO ] hook.dll attached to PID ...
 *   [INFO ] Filter: N system names loaded
 *   [INFO ] Filter: System32  = C:\Windows\System32
 *   [INFO ] Filter: SysWOW64  = C:\Windows\SysWOW64
 *   [INFO ] Queue initialised (64 slots)
 *   [INFO ] IAT patch applied — hook_test.exe
 *   [INFO ] Ready (queue: 64 slots, timeout: 200 ms, filter: N names)
 *   [ERROR] HookInit: AskEngine not linked — ...
 *   [ERROR] HookInit: TransferEngine not linked — ...
 *
 * Then per non-OS process launch:
 *   [TRACE] Filter: SKIP 'svchost.exe' — matches system name list
 *         (or silently passes for user processes)
 *   [INFO ] Caught: 'myapp.exe'
 *   [TRACE] --- Caught Process --- ... fields ...
 *   [INFO ] Queued: 'myapp.exe' (slot N, capacity 64)
 *   [ERROR] AskEngine not linked — 'myapp.exe' will time out ...
 *   [WARN ] Timeout (200 ms) — 'myapp.exe' runs locally
 */
__declspec(dllexport)
BOOL WINAPI Inject(void)
{
    if (g_hook_dll) return TRUE;

    g_hook_dll = LoadLibraryW(L"hook.dll");
    if (!g_hook_dll) {
        OutputDebugStringW(L"[offload_hook] LoadLibrary(hook.dll) failed\n");
        return FALSE;
    }

    FnHookInit init_fn = (FnHookInit)
        GetProcAddress(g_hook_dll, "HookInit");
    g_teardown_fn   = (FnHookTeardown)
        GetProcAddress(g_hook_dll, "HookTeardown");
    g_filter_add_fn = (FnHookFilterAddName)
        GetProcAddress(g_hook_dll, "hook_filter_add_name");

    if (!init_fn) {
        OutputDebugStringW(
            L"[offload_hook] HookInit not found in hook.dll\n");
        FreeLibrary(g_hook_dll);
        g_hook_dll = NULL;
        return FALSE;
    }

    /*
     * LINK AskEngine:      replace AskEngine with real fn ptr here.
     * LINK TransferEngine: replace TransferEngine with real fn ptr here.
     */
    init_fn(AskEngine, TransferEngine);
    return TRUE;
}

/*
 * Eject()
 */
__declspec(dllexport)
void WINAPI Eject(void)
{
    if (!g_hook_dll) return;
    if (g_teardown_fn) g_teardown_fn();
    FreeLibrary(g_hook_dll);
    g_hook_dll       = NULL;
    g_teardown_fn    = NULL;
    g_filter_add_fn  = NULL;
}

/*
 * OffloadFilterAdd()
 *
 * Forwards to hook.dll's hook_filter_add_name().
 * Safe to call after Inject(); no-op if hook.dll is not loaded.
 */
__declspec(dllexport)
BOOL WINAPI OffloadFilterAdd(LPCWSTR name)
{
    if (!g_filter_add_fn) return FALSE;
    return g_filter_add_fn(name);
}

} /* extern "C" */
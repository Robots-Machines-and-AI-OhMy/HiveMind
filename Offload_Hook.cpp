/*
 * Offload_Hook.cpp
 * --------------------------------------------------------------
 * Non-main component: loads hook.dll and wires in the engines.
 *
 * AskEngine      — stub (timeout path). Replace with real impl
 *                  when teammate delivers leader comms layer.
 * TransferEngine — stub for demo (leader/follower not ready).
 *                  LINK: swap NULL -> TransferEngineImpl in
 *                  Inject() once leader-follower is working.
 * --------------------------------------------------------------
 */

#include "Offload_Hook.hpp"

/* ============================================================
 *  MODULE STATE
 * ============================================================ */

static HMODULE             g_hook_dll      = NULL;
static FnHookTeardown      g_teardown_fn   = NULL;
static FnHookFilterAddName g_filter_add_fn = NULL;

/* ============================================================
 *  AskEngine STUB
 *
 *  Does NOT signal hDecisionEvent — the 200 ms timeout fires
 *  and the process runs locally.  This is correct, expected
 *  behaviour until the leader comms layer is wired in.
 *
 *  LINK: when ready, replace AskEngine in Inject() below with
 *        the real function pointer.  Implementation steps:
 *
 *    1. Post entry to a dedicated worker thread.
 *    2. Call fill_capability_request(&req, &entry->profile)
 *       (from leader_protocol.hpp) to build the wire packet.
 *    3. Send CapabilityRequest to the RAFT leader over lsQUIC
 *       on port LEADER_PORT within LEADER_RESPONSE_TIMEOUT_MS.
 *    4. Read OffloadResponse from the leader.
 *    5. Set entry->decision = response.decision.
 *       If YES: wcsncpy response.target_node -> entry->target_node.
 *    6. Signal entry->hDecisionEvent to unblock the interceptor.
 *       Check entry->in_use first — if 0 the slot already timed
 *       out and was released; do not signal.
 * ============================================================ */

static void WINAPI AskEngine(QueueEntry *entry)
{
    (void)entry;
    /* Stub: fall through to timeout in hook.c */
}

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

extern "C" {

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
     * LINK AskEngine:      replace AskEngine with real fn ptr
     *                      when leader comms layer is ready.
     * LINK TransferEngine: replace NULL with TransferEngineImpl
     *                      once leader-follower is working.
     */
    init_fn(AskEngine, NULL);  /* TransferEngine stubbed for demo */
    return TRUE;
}

__declspec(dllexport)
void WINAPI Eject(void)
{
    if (!g_hook_dll) return;
    if (g_teardown_fn) g_teardown_fn();
    FreeLibrary(g_hook_dll);
    g_hook_dll      = NULL;
    g_teardown_fn   = NULL;
    g_filter_add_fn = NULL;
}

__declspec(dllexport)
BOOL WINAPI OffloadFilterAdd(LPCWSTR name)
{
    if (!g_filter_add_fn) return FALSE;
    return g_filter_add_fn(name);
}

} /* extern "C" */
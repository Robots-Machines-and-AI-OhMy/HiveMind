/*
 * Offload_Hook.cpp
 * --------------------------------------------------------------
 * Wires AskEngine and TransferEngine into hook.dll.
 *
 * AskEngine flow:
 *   1. Build a CapabilityRequest from the intercepted ProcessProfile.
 *   2. Ask NetworkManager::requestOffload() — this goes to the Raft
 *      leader which picks the best node and returns its IP.
 *   3. If the leader returns a target IP:
 *        - Set entry->decision = OFFLOAD_YES
 *        - Write "ip:port" into entry->target_node
 *   4. If no target (local cluster, degraded, etc.):
 *        - Set entry->decision = OFFLOAD_NO
 *   5. Signal entry->hDecisionEvent — unblocks hook.c.
 *
 * TransferEngine:
 *   Passed directly to HookInit() as TransferEngineImpl from
 *   transfer_engine.cpp.  Called only when AskEngine sets YES.
 * --------------------------------------------------------------
 */

#include "Offload_Hook.hpp"
#include "Network.hpp"          // NetworkManager singleton
#include "global.hpp"

#include <string>
#include <chrono>

/* ============================================================
 *  MODULE STATE
 * ============================================================ */

static HMODULE             g_hook_dll      = NULL;
static FnHookTeardown      g_teardown_fn   = NULL;
static FnHookFilterAddName g_filter_add_fn = NULL;

/* ============================================================
 *  AskEngine
 *
 *  Runs on the hook's internal worker thread.  Must complete
 *  within DECISION_TIMEOUT_MS (200 ms) or the hook falls back
 *  to local execution automatically.
 *
 *  We budget LEADER_RESPONSE_TIMEOUT_MS (150 ms) for the network
 *  round-trip, leaving 50 ms headroom for local overhead.
 * ============================================================ */

static void WINAPI AskEngine(QueueEntry *entry)
{
    if (!entry) return;

    try {
        // Build an opaque process_id for the Raft log.
        // Format: "exe_basename:caller_pid:tick"
        char pid_buf[MAX_PATH + 64] = {};
        char name_utf8[MAX_PATH]    = {};
        WideCharToMultiByte(CP_UTF8, 0,
            entry->profile.name, -1,
            name_utf8, sizeof(name_utf8) - 1,
            NULL, NULL);
        sprintf_s(pid_buf, sizeof(pid_buf), "%s:%lu:%lld",
                  name_utf8,
                  entry->profile.caller_pid,
                  entry->profile.intercept_tick.QuadPart);
        std::string process_id(pid_buf);

        NetworkManager& net = NetworkManager::getInstance();

        if (!net.isConnected()) {
            // Not in a cluster — run locally.
            entry->decision = OFFLOAD_NO;
            SetEvent(entry->hDecisionEvent);
            return;
        }

        // Ask the Raft leader to place this process.
        // requestOffload() blocks up to LEADER_RESPONSE_TIMEOUT_MS.
        std::string target_ip = net.requestOffload(process_id);

        if (target_ip.empty()) {
            entry->decision = OFFLOAD_NO;
        } else {
            entry->decision = OFFLOAD_YES;

            // Write "ip:port" into entry->target_node (wide string).
            std::string target_node = target_ip + ":"
                + std::to_string(REMOTE_AGENT_PORT);
            MultiByteToWideChar(CP_UTF8, 0,
                target_node.c_str(), -1,
                entry->target_node,
                _countof(entry->target_node) - 1);
        }
    } catch (...) {
        entry->decision = OFFLOAD_FALLBACK;
    }

    // Only signal if the hook hasn't already timed out and
    // reclaimed the slot (in_use goes to 0 on timeout).
    if (InterlockedCompareExchange(&entry->in_use, 1, 1) == 1)
        SetEvent(entry->hDecisionEvent);
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

    // Wire both engines.
    // AskEngine  : asks the Raft leader for a target node.
    // TransferEngineImpl: builds and ships the process bundle.
    init_fn(AskEngine, TransferEngineImpl);
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
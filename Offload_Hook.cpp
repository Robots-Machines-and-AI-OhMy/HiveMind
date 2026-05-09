/*
 * Offload_Hook.cpp
 * --------------------------------------------------------------
 * Non-main component: loads hook.dll and wires in the engines.
 *
 * AskEngine — calls NetworkManager::requestOffload() if the
 *             network layer is connected.  Falls back to
 *             OFFLOAD_NO (local run) if not connected or if
 *             the leader returns no target.
 *
 * TransferEngine — stub (NULL) for demo.
 *                  LINK: swap NULL -> TransferEngineImpl once
 *                  end-to-end bundle transfer is validated.
 * --------------------------------------------------------------
 */

#include "Offload_Hook.hpp"
#include "Network.hpp"   /* NetworkManager::requestOffload() */

#include <string>

/* ============================================================
 *  MODULE STATE
 * ============================================================ */

static HMODULE             g_hook_dll      = NULL;
static FnHookTeardown      g_teardown_fn   = NULL;
static FnHookFilterAddName g_filter_add_fn = NULL;

/* ============================================================
 *  AskEngine
 *
 *  Called on the intercepting thread inside Hook_CreateProcessW.
 *  Must signal entry->hDecisionEvent before returning.
 *  The outer wait in hook.c allows DECISION_TIMEOUT_MS total
 *  (200 ms) — keep this function fast.
 * ============================================================ */

static void WINAPI AskEngine(QueueEntry* entry)
{
    if (!entry) return;

    // Use C++ try/catch — __try is incompatible with C++ objects
    // (std::string, NetworkManager) under /EHsc. The effect is the same:
    // any unexpected exception falls back to running locally.
    try {
        char pid_buf[MAX_PATH + 32] = {};
        sprintf_s(pid_buf, sizeof(pid_buf), "proc_%lu",
                  entry->profile.caller_pid);
        std::string process_id(pid_buf);

        NetworkManager& net = NetworkManager::getInstance();

        if (net.isConnected()) {
            std::string target = net.requestOffload(process_id);
            if (!target.empty()) {
                entry->decision = OFFLOAD_YES;
                wcsncpy_s(entry->target_node, 256,
                          std::wstring(target.begin(), target.end()).c_str(),
                          _TRUNCATE);
            } else {
                entry->decision = OFFLOAD_NO;
            }
        } else {
            entry->decision = OFFLOAD_NO;
        }
    } catch (...) {
        entry->decision = OFFLOAD_FALLBACK;
    }

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

    FnHookInit init_fn  = (FnHookInit)
        GetProcAddress(g_hook_dll, "HookInit");
    g_teardown_fn       = (FnHookTeardown)
        GetProcAddress(g_hook_dll, "HookTeardown");
    g_filter_add_fn     = (FnHookFilterAddName)
        GetProcAddress(g_hook_dll, "hook_filter_add_name");

    if (!init_fn) {
        OutputDebugStringW(
            L"[offload_hook] HookInit not found in hook.dll\n");
        FreeLibrary(g_hook_dll);
        g_hook_dll = NULL;
        return FALSE;
    }

    /*
     * AskEngine      — live, calls NetworkManager::requestOffload().
     * TransferEngine — LINK: replace NULL with TransferEngineImpl
     *                  when bundle transfer is ready.
     */
    init_fn(AskEngine, NULL);
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
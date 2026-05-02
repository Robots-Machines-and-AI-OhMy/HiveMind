/*
 * offload_hook.hpp
 * --------------------------------------------------------------
 * Public C++ interface for offload_hook.dll.
 * (Renamed from injector.hpp — same role, clearer name.)
 *
 * Exposes:
 *   Inject()  — load hook.dll, wire in engine stubs, activate.
 *   Eject()   — teardown hook.dll and unload.
 *
 * Engine linkage points are marked with LINK comments.
 * Replace stubs in offload_hook.cpp when engines are ready.
 *
 * Also re-exports FnHookFilterAddName so callers can add names
 * to hook.dll's runtime filter without including hook.h directly.
 * --------------------------------------------------------------
 */

#ifndef OFFLOAD_HOOK_HPP
#define OFFLOAD_HOOK_HPP

#include "hook.h"   /* ProcessProfile, QueueEntry, engine typedefs,
                       DECISION_TIMEOUT_MS, HOOK_QUEUE_CAPACITY,
                       FnHookFilterAddName                          */

/* ============================================================
 *  ENGINE LINKAGE DECLARATIONS
 *
 *  When AskEngine and TransferEngine are implemented:
 *    1. Include their headers here.
 *    2. At the LINK comments in offload_hook.cpp, replace the
 *       stub function pointers with the real ones.
 *
 *  Example:
 *    #include "ask_engine.h"       // AskEngineImpl()
 *    #include "transfer_engine.h"  // TransferEngineImpl()
 * ============================================================ */

/* LINK AskEngine header here      */
/* LINK TransferEngine header here */

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

extern "C" {

/*
 * Inject()
 *
 * Loads hook.dll, resolves HookInit, passes engine stubs.
 * Until real engines are linked:
 *   - Every non-OS process is caught and queued.
 *   - AskEngine stub does not signal — timeout fires at 200 ms.
 *   - Process runs locally after timeout.
 *   - All activity is logged to C:\Temp\hook.log.
 *
 * Returns TRUE if hook.dll loaded and HookInit was called.
 */
__declspec(dllexport) BOOL WINAPI Inject(void);

/*
 * Eject()
 *
 * Clears engines in hook.dll, then unloads it.
 * After this call every intercepted launch passes through
 * immediately (NULL engine guard -> local_run).
 */
__declspec(dllexport) void WINAPI Eject(void);

/*
 * OffloadFilterAdd()
 *
 * Thin wrapper around hook.dll's hook_filter_add_name().
 * Adds a process basename to the permanent local-only list.
 * Call when the leader determines a binary is never offloadable.
 *
 * name — basename only, e.g. L"myapp.exe"
 * Returns TRUE on success.
 */
__declspec(dllexport) BOOL WINAPI OffloadFilterAdd(LPCWSTR name);

} /* extern "C" */

#endif /* OFFLOAD_HOOK_HPP */
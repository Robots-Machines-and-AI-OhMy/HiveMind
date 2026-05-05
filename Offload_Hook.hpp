/*
 * Offload_Hook.hpp
 * --------------------------------------------------------------
 * Public C++ interface for offload_hook.dll.
 *
 * Exposes:
 *   Inject()           — load hook.dll, wire in engines, activate.
 *   Eject()            — teardown hook.dll and unload.
 *   OffloadFilterAdd() — add a basename to the permanent skip list.
 *
 * Engine linkage:
 *   TransferEngine — implemented in transfer_engine.cpp.
 *                    Wired automatically in Inject().
 *   AskEngine      — stub until leader comms layer is ready.
 *                    LINK comment in Offload_Hook.cpp marks the
 *                    replacement point.
 * --------------------------------------------------------------
 */

#ifndef OFFLOAD_HOOK_HPP
#define OFFLOAD_HOOK_HPP

#include "hook.h"   /* ProcessProfile, QueueEntry, engine typedefs,
                       DECISION_TIMEOUT_MS, HOOK_QUEUE_CAPACITY,
                       FnHookFilterAddName                          */

/* LINK: uncomment these when leader-follower is working:
 * #include "transfer_engine.hpp"  // TransferEngineImpl()
 * #include "leader_protocol.hpp"  // CapabilityRequest, OffloadResponse */

/* ============================================================
 *  ENGINE LINKAGE DECLARATIONS
 *
 *  TransferEngine: stubbed NULL for demo — leader-follower not
 *                   ready yet.  Files transfer_engine.cpp/.hpp
 *                   and leader_protocol.hpp are complete and
 *                   waiting. To re-enable:
 *                     1. Uncomment the includes above.
 *                     2. Change NULL -> TransferEngineImpl in
 *                        Offload_Hook.cpp Inject().
 *                     3. Add transfer_engine.cpp to the
 *                        offload_hook CMake target.
 *
 *  AskEngine: pending teammate's leader comms layer.
 *    When ready:
 *      1. Include the ask engine header here.
 *      2. Replace AskEngine stub in Offload_Hook.cpp Inject()
 *         with the real function pointer.
 *    Wire format is defined in leader_protocol.hpp.
 * ============================================================ */

/* LINK AskEngine header here when ready */

/* ============================================================
 *  PUBLIC API
 * ============================================================ */

extern "C" {

__declspec(dllexport) BOOL WINAPI Inject(void);
__declspec(dllexport) void WINAPI Eject(void);
__declspec(dllexport) BOOL WINAPI OffloadFilterAdd(LPCWSTR name);

} /* extern "C" */

#endif /* OFFLOAD_HOOK_HPP */
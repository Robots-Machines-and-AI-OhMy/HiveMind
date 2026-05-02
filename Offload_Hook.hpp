//
// Created by jason on 5/1/2026.
//

/*
 * injector.hpp
 * --------------------------------------------------------------
 * Public C++ interface for injector.dll.
 *
 * Exposes two free functions:
 *   Inject()  — load hook.dll and wire in the two engine stubs.
 *   Eject()   — teardown and unload hook.dll.
 *
 * Engine linkage points are marked clearly below.
 * Replace each stub with the real engine function pointer
 * when AskEngine and TransferEngine are implemented.
 * --------------------------------------------------------------
 */

#ifndef INJECTOR_HPP
#define INJECTOR_HPP

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include "hook.h"

/* ============================================================
 *  ENGINE LINKAGE DECLARATIONS
 *
 *  When AskEngine and TransferEngine are ready, declare them
 *  here and include their respective headers.
 *
 *  Example:
 *    #include "ask_engine.h"      // provides AskEngineImpl()
 *    #include "transfer_engine.h" // provides TransferEngineImpl()
 *
 *  Then update the engine linkage points in injector.cpp
 *  (marked with /* LINK ... * comments) to pass the real
 *  function pointers to HookInit instead of the stubs.
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
 * Loads hook.dll into the current process, resolves HookInit,
 * and passes the engine function pointers to it.
 *
 * Currently passes stub engines that return failure so the
 * fallback path in hook.dll is exercised and logged.
 *
 * Returns TRUE if hook.dll loaded and HookInit was called.
 * Returns FALSE if loading failed — the host process continues
 * normally without any interception.
 */
__declspec(dllimport) BOOL WINAPI Inject(void);

/*
 * Eject()
 *
 * Calls HookTeardown inside hook.dll to clear engine pointers,
 * then unloads hook.dll.
 *
 * Note: the IAT entry still points to Hook_CreateProcessW after
 * this call.  Hook_CreateProcessW detects NULL engines and falls
 * through to the real CreateProcessW immediately.  For a full
 * IAT restore, rebuild with Microsoft Detours and call
 * DetourDetach in HookTeardown.
 */
__declspec(dllimport) void WINAPI Eject(void);

} /* extern "C" */

#endif /* INJECTOR_HPP */

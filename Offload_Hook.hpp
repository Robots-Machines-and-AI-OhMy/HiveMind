/*
* Offload_Hook.hpp
 * --------------------------------------------------------------
 * Public C++ interface for offload_hook.dll.
 *
 * Exposes:
 *   Inject()           — load hook.dll, wire in engines, activate.
 *   Eject()            — teardown hook.dll and unload.
 *   OffloadFilterAdd() — add a basename to the permanent skip list.
 * --------------------------------------------------------------
 */

#ifndef OFFLOAD_HOOK_HPP
#define OFFLOAD_HOOK_HPP

#include "hook.h"
#include "transfer_engine.hpp"
#include "leader_protocol.hpp"

extern "C" {

	__declspec(dllexport) BOOL WINAPI Inject(void);
	__declspec(dllexport) void WINAPI Eject(void);
	__declspec(dllexport) BOOL WINAPI OffloadFilterAdd(LPCWSTR name);

} /* extern "C" */

#endif /* OFFLOAD_HOOK_HPP */
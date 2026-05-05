/*
 * transfer_engine.hpp
 * --------------------------------------------------------------
 * Public interface for TransferEngine.
 *
 * TransferEngine is called by offload_hook.cpp when AskEngine
 * returns OFFLOAD_YES.  It is responsible for:
 *
 *   1. Building a ProcessBundle from the ProcessProfile.
 *   2. Resolving and packaging PE dependencies.
 *   3. Opening lsQUIC streams to the target node.
 *   4. Shipping the bundle and pumping I/O streams back.
 *   5. Creating a surrogate PROCESS_INFORMATION for the caller.
 *
 * Stream layout (one QUIC connection per offloaded process):
 *   Stream 0 — control   : bundle send, exit code receive
 *   Stream 1 — stdin     : client → remote
 *   Stream 2 — stdout    : remote → client
 *   Stream 3 — stderr    : remote → client
 *   Stream 4 — (reserved): framebuffer / video, future use
 *
 * The function signature matches TransferEngineFn in hook.h so
 * it can be passed directly to HookInit().
 * --------------------------------------------------------------
 */

#ifndef TRANSFER_ENGINE_HPP
#define TRANSFER_ENGINE_HPP

#include "hook.h"

/* ============================================================
 *  WIRE PROTOCOL CONSTANTS
 *  Shared between transfer_engine.cpp and remote_agent.cpp.
 *  Both sides must agree on these values.
 * ============================================================ */

/* Magic number at the start of every bundle header */
#define BUNDLE_MAGIC          0x4D4D4248u   /* "MMBH" MindMesh Bundle Header */

/* Current bundle format version */
#define BUNDLE_VERSION        1u

/* Default port remote_agent listens on */
#define REMOTE_AGENT_PORT     19876

/* Maximum size of a single PE binary we will transfer (256 MB) */
#define BUNDLE_MAX_BINARY_MB  256u

/* Maximum number of DLL dependencies bundled per process */
#define BUNDLE_MAX_DEPS       128u

/* ============================================================
 *  BUNDLE HEADER  (sent on Stream 0 before binary payload)
 *
 *  Layout on wire (all fields little-endian):
 *    [4]  magic
 *    [4]  version
 *    [4]  flags
 *    [4]  dep_count          number of DLL entries following
 *    [8]  binary_size        bytes of the main .exe
 *    [2]  name_len           wchar_t count of exe basename
 *    [name_len*2] name       exe basename (UTF-16 LE, no null)
 *    [2]  cmdline_len        wchar_t count
 *    [cmdline_len*2] cmdline (UTF-16 LE, no null)
 *    [2]  cwd_len
 *    [cwd_len*2]  cwd
 *    [2]  env_len            wchar_t count of env block
 *    [env_len*2]  env        double-null-terminated env block
 *    --- per dep (dep_count times) ---
 *    [2]  dep_name_len
 *    [dep_name_len*2] dep_name
 *    [8]  dep_size
 *    --- binary payload ---
 *    [binary_size] raw PE bytes of main exe
 *    --- per dep ---
 *    [dep_size]    raw PE bytes of each dep in order
 * ============================================================ */

#pragma pack(push, 1)
typedef struct BundleHeader {
    unsigned int  magic;
    unsigned int  version;
    unsigned int  flags;          /* reserved, set to 0      */
    unsigned int  dep_count;
    unsigned long long binary_size;
} BundleHeader;

typedef struct DepEntry {
    unsigned short name_len;      /* wchar_t count, no null  */
    unsigned long long dep_size;  /* bytes                   */
    /* followed by name_len*2 bytes of name, then dep_size bytes */
} DepEntry;
#pragma pack(pop)

/* BundleHeader flags */
#define BUNDLE_FLAG_HAS_GUI    (1u << 0)  /* process needs display    */
#define BUNDLE_FLAG_SUSPENDED  (1u << 1)  /* start in suspended state */

/* ============================================================
 *  EXIT PACKET  (sent by remote_agent on Stream 0 on exit)
 *
 *  [4] exit_magic   (BUNDLE_MAGIC ^ 0xFFFFFFFF)
 *  [4] exit_code
 * ============================================================ */

#define EXIT_MAGIC  (BUNDLE_MAGIC ^ 0xFFFFFFFFu)

/* ============================================================
 *  TransferEngine entry point
 *
 *  Matches TransferEngineFn typedef in hook.h exactly.
 *  Pass this function pointer to HookInit() via offload_hook.cpp.
 *
 *  Returns TRUE  — remote execution started, surrogate handle
 *                  written to lpProcessInformation.
 *  Returns FALSE — any failure; hook falls back to local run.
 * ============================================================ */

BOOL WINAPI TransferEngineImpl(
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

#endif /* TRANSFER_ENGINE_HPP */
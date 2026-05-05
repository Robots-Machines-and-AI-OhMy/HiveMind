/*
 * transfer_engine.cpp
 * --------------------------------------------------------------
 * TransferEngine implementation.
 *
 * Current state: skeleton with all structural pieces in place.
 * lsQUIC transport is stubbed as a blocking TCP connection so
 * the bundle format and surrogate handle logic can be tested
 * end-to-end before the QUIC layer is ready.
 *
 * IMPLEMENT markers show exactly where lsQUIC replaces the TCP
 * stub once your teammate's transport layer is available.
 *
 * Build: add transfer_engine.cpp to offload_hook target in
 *        CMakeLists.txt and link ws2_32.lib.
 * --------------------------------------------------------------
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>

#include "transfer_engine.hpp"

#pragma comment(lib, "ws2_32.lib")

/* ============================================================
 *  INTERNAL LOGGING
 *  Mirrors hook.c's log format so both appear consistently
 *  in C:\Temp\hook.log.
 * ============================================================ */

#define TE_LOG_PATH  L"C:\\Temp\\hook.log"
#define TE_LOG_BOM   0xFEFF

static CRITICAL_SECTION g_te_log_cs;
static bool             g_te_log_cs_init = false;

static void te_log(const wchar_t *level, const wchar_t *fmt, ...)
{
    __try {
        if (!g_te_log_cs_init) return;

        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t msg[1024];
        va_list ap; va_start(ap, fmt);
        _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
        va_end(ap);

        wchar_t line[1280];
        int n = _snwprintf_s(line, _countof(line), _TRUNCATE,
            L"[%02u:%02u:%02u.%03u] [%s] [transfer_engine:%lu] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level, GetCurrentProcessId(), msg);
        if (n <= 0) return;

        EnterCriticalSection(&g_te_log_cs);
        HANDLE hf = CreateFileW(TE_LOG_PATH,
            FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
            OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            if (GetFileSize(hf, NULL) == 0) {
                DWORD bw; wchar_t bom = TE_LOG_BOM;
                WriteFile(hf, &bom, sizeof(bom), &bw, NULL);
            }
            DWORD bw;
            WriteFile(hf, line, (DWORD)(n * sizeof(wchar_t)), &bw, NULL);
            CloseHandle(hf);
        }
        LeaveCriticalSection(&g_te_log_cs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#define TE_INFO(fmt,...)  te_log(L"INFO ", fmt, ##__VA_ARGS__)
#define TE_WARN(fmt,...)  te_log(L"WARN ", fmt, ##__VA_ARGS__)
#define TE_ERROR(fmt,...) te_log(L"ERROR", fmt, ##__VA_ARGS__)

/* ============================================================
 *  DEPENDENCY WALKER
 *  Collects the set of non-system DLLs the binary imports.
 *  Returns a list of full paths to ship with the bundle.
 *  Recursive (handles indirect deps up to max_depth).
 * ============================================================ */

struct DepList {
    wchar_t paths[BUNDLE_MAX_DEPS][MAX_PATH];
    int     count;
};

static void collect_deps(const wchar_t *binary_path,
                         DepList *deps, int depth)
{
    if (depth > 3) return;  /* cap recursion */

    __try {
        HANDLE hf = CreateFileW(binary_path, GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf == INVALID_HANDLE_VALUE) return;

        /* Map the PE into memory for inspection */
        HANDLE hm = CreateFileMappingW(hf, NULL, PAGE_READONLY, 0, 0, NULL);
        CloseHandle(hf);
        if (!hm) return;

        BYTE *base = (BYTE *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(hm);
        if (!base) return;

        __try {
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) goto unmap;

            IMAGE_NT_HEADERS *nt =
                (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) goto unmap;

            IMAGE_DATA_DIRECTORY *dir =
                &nt->OptionalHeader
                    .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir->VirtualAddress) goto unmap;

            IMAGE_IMPORT_DESCRIPTOR *imp =
                (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

            for (; imp->Name; imp++) {
                char *dll_name = (char *)(base + imp->Name);

                /* Resolve to full path via SearchPath */
                wchar_t w_name[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, dll_name, -1,
                                    w_name, MAX_PATH);

                wchar_t full[MAX_PATH];
                if (!SearchPathW(NULL, w_name, NULL, MAX_PATH,
                                 full, NULL))
                    continue;

                /* Skip system DLLs — target is Windows 10/11 x86 so
                 * system32/SysWOW64 contents match and need not be shipped. */
                wchar_t sys32[MAX_PATH], syswow[MAX_PATH], win[MAX_PATH];
                wchar_t prog[MAX_PATH], prog86[MAX_PATH];
                GetSystemDirectoryW(sys32, MAX_PATH);
                GetWindowsDirectoryW(win, MAX_PATH);
                _snwprintf_s(syswow, MAX_PATH, _TRUNCATE,
                             L"%s\\SysWOW64", win);
                /* Also skip DLLs from Program Files — runtime redistributables
                 * (MSVC CRT, DirectX, etc.) that ship with Windows or are
                 * installed system-wide.  App-local DLLs (same dir as .exe)
                 * are always bundled regardless of location.               */
                GetEnvironmentVariableW(L"ProgramFiles",
                                        prog, MAX_PATH);
                GetEnvironmentVariableW(L"ProgramFiles(x86)",
                                        prog86, MAX_PATH);

                /* Resolve binary's own directory for same-dir check */
                wchar_t bin_dir[MAX_PATH];
                wcsncpy_s(bin_dir, MAX_PATH, binary_path, _TRUNCATE);
                wchar_t *last_sep = wcsrchr(bin_dir, L'\\');
                if (last_sep) *last_sep = L'\0';

                bool is_same_dir = (_wcsnicmp(full, bin_dir,
                                              wcslen(bin_dir)) == 0);

                /* Always bundle same-dir DLLs; skip everything else
                 * that lives under a system or program-files path.  */
                if (!is_same_dir) {
                    if (_wcsnicmp(full, sys32,  wcslen(sys32))  == 0) continue;
                    if (_wcsnicmp(full, syswow, wcslen(syswow)) == 0) continue;
                    if (prog[0]   &&
                        _wcsnicmp(full, prog,   wcslen(prog))   == 0) continue;
                    if (prog86[0] &&
                        _wcsnicmp(full, prog86, wcslen(prog86)) == 0) continue;
                }

                /* Deduplicate */
                bool found = false;
                for (int i = 0; i < deps->count; i++) {
                    if (_wcsicmp(deps->paths[i], full) == 0) {
                        found = true; break;
                    }
                }
                if (found) continue;
                if (deps->count >= BUNDLE_MAX_DEPS) break;

                wcsncpy_s(deps->paths[deps->count], MAX_PATH,
                          full, _TRUNCATE);
                deps->count++;

                /* Recurse into this dep */
                collect_deps(full, deps, depth + 1);
            }
        unmap:;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        UnmapViewOfFile(base);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* ============================================================
 *  WIRE HELPERS  (write to socket)
 * ============================================================ */

static bool send_all(SOCKET s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int sent = send(s, p, (int)len, 0);
        if (sent <= 0) return false;
        p   += sent;
        len -= sent;
    }
    return true;
}

static bool send_u16(SOCKET s, uint16_t v)
{
    return send_all(s, &v, sizeof(v));
}

static bool send_u32(SOCKET s, uint32_t v)
{
    return send_all(s, &v, sizeof(v));
}

static bool send_u64(SOCKET s, uint64_t v)
{
    return send_all(s, &v, sizeof(v));
}

static bool send_wstr(SOCKET s, const wchar_t *str, size_t wchars)
{
    /* length prefix then raw UTF-16 LE bytes */
    if (!send_u16(s, (uint16_t)wchars)) return false;
    return send_all(s, str, wchars * sizeof(wchar_t));
}

static bool send_file(SOCKET s, const wchar_t *path, uint64_t *size_out)
{
    HANDLE hf = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {0};
    GetFileSizeEx(hf, &sz);
    *size_out = (uint64_t)sz.QuadPart;

    if (!send_u64(s, *size_out)) {
        CloseHandle(hf); return false;
    }

    char buf[64 * 1024];
    DWORD rd;
    bool ok = true;
    while (ok && ReadFile(hf, buf, sizeof(buf), &rd, NULL) && rd > 0)
        ok = send_all(s, buf, rd);

    CloseHandle(hf);
    return ok;
}

static bool recv_all(SOCKET s, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int r = recv(s, p, (int)len, 0);
        if (r <= 0) return false;
        p   += r;
        len -= r;
    }
    return true;
}

/* ============================================================
 *  SURROGATE PROCESS
 *
 *  The hook caller receives a PROCESS_INFORMATION with a real
 *  process handle and thread handle.  We create a minimal
 *  suspended helper process (cmd /c pause) as the surrogate.
 *  A pump thread waits for the remote exit code and sets the
 *  surrogate's exit code to match before terminating it, so
 *  WaitForSingleObject and GetExitCodeProcess behave correctly.
 * ============================================================ */

struct PumpContext {
    SOCKET   ctrl_socket;   /* Stream 0 — receives exit packet */
    SOCKET   stdout_socket; /* Stream 2 — remote stdout        */
    SOCKET   stderr_socket; /* Stream 3 — remote stderr        */
    HANDLE   hSurrogate;    /* surrogate process handle        */
    HANDLE   hStdout;       /* caller's inherited stdout       */
    HANDLE   hStderr;       /* caller's inherited stderr       */
};

static DWORD WINAPI pump_thread(LPVOID param)
{
    PumpContext *ctx = (PumpContext *)param;

    /* Pump stdout and stderr in parallel using select() */
    char buf[4096];
    bool running = true;
    while (running) {
        fd_set rd;
        FD_ZERO(&rd);

        bool has_ctrl   = ctx->ctrl_socket   != INVALID_SOCKET;
        bool has_stdout = ctx->stdout_socket != INVALID_SOCKET;
        bool has_stderr = ctx->stderr_socket != INVALID_SOCKET;

        if (!has_ctrl && !has_stdout && !has_stderr) break;

        if (has_ctrl)   FD_SET(ctx->ctrl_socket,   &rd);
        if (has_stdout) FD_SET(ctx->stdout_socket, &rd);
        if (has_stderr) FD_SET(ctx->stderr_socket, &rd);

        struct timeval tv = { 1, 0 };
        int r = select(0, &rd, NULL, NULL, &tv);
        if (r <= 0) continue;

        /* stdout */
        if (has_stdout && FD_ISSET(ctx->stdout_socket, &rd)) {
            int n = recv(ctx->stdout_socket, buf, sizeof(buf), 0);
            if (n <= 0) {
                closesocket(ctx->stdout_socket);
                ctx->stdout_socket = INVALID_SOCKET;
            } else if (ctx->hStdout != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(ctx->hStdout, buf, n, &written, NULL);
            }
        }

        /* stderr */
        if (has_stderr && FD_ISSET(ctx->stderr_socket, &rd)) {
            int n = recv(ctx->stderr_socket, buf, sizeof(buf), 0);
            if (n <= 0) {
                closesocket(ctx->stderr_socket);
                ctx->stderr_socket = INVALID_SOCKET;
            } else if (ctx->hStderr != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteFile(ctx->hStderr, buf, n, &written, NULL);
            }
        }

        /* control — exit packet */
        if (has_ctrl && FD_ISSET(ctx->ctrl_socket, &rd)) {
            uint32_t magic = 0, exit_code = 0;
            if (recv_all(ctx->ctrl_socket, &magic, 4) &&
                recv_all(ctx->ctrl_socket, &exit_code, 4) &&
                magic == EXIT_MAGIC)
            {
                TE_INFO(L"Remote process exited with code %lu", exit_code);
                /* Terminate surrogate with matching exit code */
                TerminateProcess(ctx->hSurrogate, exit_code);
                running = false;
            }
            closesocket(ctx->ctrl_socket);
            ctx->ctrl_socket = INVALID_SOCKET;
        }
    }

    CloseHandle(ctx->hSurrogate);
    delete ctx;
    return 0;
}

/* ============================================================
 *  CONNECT TO TARGET NODE
 *
 *  target_node format: "ip:port" e.g. "192.168.1.5:19876"
 *
 *  IMPLEMENT: replace connect_tcp with lsQUIC stream open.
 *  The socket returned here maps to QUIC Stream 0 (control).
 *  Additional streams (stdout, stderr) are opened by the caller.
 * ============================================================ */

static SOCKET connect_tcp(const wchar_t *target_node)
{
    /* Parse "host:port" */
    wchar_t host[256] = {0};
    int     port      = REMOTE_AGENT_PORT;

    const wchar_t *colon = wcsrchr(target_node, L':');
    if (colon) {
        size_t hlen = colon - target_node;
        wcsncpy_s(host, _countof(host), target_node, hlen);
        port = _wtoi(colon + 1);
    } else {
        wcsncpy_s(host, _countof(host), target_node, _TRUNCATE);
    }

    char host_a[256];
    WideCharToMultiByte(CP_ACP, 0, host, -1, host_a, sizeof(host_a),
                        NULL, NULL);

    struct addrinfo hints = {}, *res = NULL;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_s[16];
    _snprintf_s(port_s, sizeof(port_s), _TRUNCATE, "%d", port);

    if (getaddrinfo(host_a, port_s, &hints, &res) != 0)
        return INVALID_SOCKET;

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(s); freeaddrinfo(res); return INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return s;

    /*
     * IMPLEMENT: replace the TCP connect above with:
     *
     *   lsquic_conn_t *conn = quic_open_connection(target_node);
     *   lsquic_stream_t *ctrl = lsquic_conn_get_stream(conn, 0);
     *   return (SOCKET)(uintptr_t)ctrl;
     *
     * Then open streams 2 and 3 separately for stdout/stderr.
     */
}

/* ============================================================
 *  TransferEngineImpl
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
    LPPROCESS_INFORMATION  lpProcessInformation)
{
    TE_INFO(L"TransferEngine: starting offload of '%s' to '%s'",
            profile->name, target_node);

    /* ── 1. Initialise Winsock (idempotent) ─────────────────── */
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    /* ── 2. Collect dependencies ─────────────────────────────── */
    DepList deps = {};
    collect_deps(profile->full_path, &deps, 0);
    TE_INFO(L"TransferEngine: '%s' has %d non-system dep(s)",
            profile->name, deps.count);

    /* ── 3. Connect to remote agent ──────────────────────────── */
    /*
     * IMPLEMENT: replace connect_tcp with lsQUIC stream open.
     * See connect_tcp() comment above.
     */
    SOCKET ctrl = connect_tcp(target_node);
    if (ctrl == INVALID_SOCKET) {
        TE_ERROR(L"TransferEngine: cannot connect to '%s' (err %d)",
                 target_node, WSAGetLastError());
        return FALSE;
    }
    TE_INFO(L"TransferEngine: connected to '%s'", target_node);

    /* Open separate connections for stdout and stderr streams.
     * With lsQUIC these would be additional streams on the same
     * connection rather than separate TCP sockets.
     *
     * IMPLEMENT: replace with lsquic_conn_get_stream(conn, 2/3)
     */
    SOCKET stdout_sock = connect_tcp(target_node);
    SOCKET stderr_sock = connect_tcp(target_node);

    /* ── 4. Send bundle header ───────────────────────────────── */
    BundleHeader hdr = {};
    hdr.magic   = BUNDLE_MAGIC;
    hdr.version = BUNDLE_VERSION;
    hdr.flags   = 0;
    if (profile->has_gui)
        hdr.flags |= BUNDLE_FLAG_HAS_GUI;
    if (dwCreationFlags & CREATE_SUSPENDED)
        hdr.flags |= BUNDLE_FLAG_SUSPENDED;
    hdr.dep_count   = (unsigned int)deps.count;
    hdr.binary_size = 0; /* filled after file send */

    if (!send_all(ctrl, &hdr, sizeof(hdr))) {
        TE_ERROR(L"TransferEngine: failed sending bundle header");
        closesocket(ctrl); return FALSE;
    }

    /* ── 5. Send string fields ───────────────────────────────── */
    const wchar_t *cmdline = lpCommandLine
        ? lpCommandLine : profile->cmdline;
    const wchar_t *cwd = lpCurrentDirectory
        ? lpCurrentDirectory : profile->cwd;

    bool ok =
        send_wstr(ctrl, profile->name,
                  wcslen(profile->name))             &&
        send_wstr(ctrl, cmdline, wcslen(cmdline))    &&
        send_wstr(ctrl, cwd,    wcslen(cwd));

    /* Environment block — double-null terminated */
    if (ok) {
        const wchar_t *env = lpEnvironment
            ? (const wchar_t *)lpEnvironment : L"\0";
        size_t env_len = 0;
        while (env[env_len] || env[env_len + 1]) env_len++;
        env_len += 2; /* include double null */
        ok = send_wstr(ctrl, env, env_len);
    }

    if (!ok) {
        TE_ERROR(L"TransferEngine: failed sending string fields");
        closesocket(ctrl); return FALSE;
    }

    /* ── 6. Send dependency entries (name + size + bytes) ───── */
    for (int i = 0; i < deps.count && ok; i++) {
        wchar_t *dep_name = wcsrchr(deps.paths[i], L'\\');
        dep_name = dep_name ? dep_name + 1 : deps.paths[i];

        DepEntry de = {};
        de.name_len = (unsigned short)wcslen(dep_name);

        ok = send_all(ctrl, &de.name_len, sizeof(de.name_len)) &&
             send_all(ctrl, dep_name,
                      de.name_len * sizeof(wchar_t));
        if (!ok) break;

        uint64_t dep_sz = 0;
        ok = send_file(ctrl, deps.paths[i], &dep_sz);
        if (ok)
            TE_INFO(L"TransferEngine: sent dep '%s' (%llu bytes)",
                    dep_name, dep_sz);
    }

    /* ── 7. Send main binary ─────────────────────────────────── */
    if (ok) {
        uint64_t bin_sz = 0;
        ok = send_file(ctrl, profile->full_path, &bin_sz);
        if (ok)
            TE_INFO(L"TransferEngine: sent '%s' (%llu bytes)",
                    profile->name, bin_sz);
    }

    if (!ok) {
        TE_ERROR(L"TransferEngine: bundle send failed");
        closesocket(ctrl);
        closesocket(stdout_sock);
        closesocket(stderr_sock);
        return FALSE;
    }

    TE_INFO(L"TransferEngine: bundle complete, creating surrogate");

    /* ── 8. Create surrogate process ─────────────────────────── */
    /*
     * The surrogate is a minimal suspended process the caller
     * can WaitForSingleObject on.  The pump thread terminates
     * it with the remote exit code when the remote process exits.
     */
    STARTUPINFOW si_surr = { sizeof(si_surr) };
    PROCESS_INFORMATION pi_surr = {};

    wchar_t surr_cmd[] = L"cmd.exe /c pause";
    BOOL surr_ok = CreateProcessW(
        NULL, surr_cmd, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        NULL, NULL, &si_surr, &pi_surr);

    if (!surr_ok) {
        TE_ERROR(L"TransferEngine: surrogate creation failed (err %lu)",
                 GetLastError());
        closesocket(ctrl);
        closesocket(stdout_sock);
        closesocket(stderr_sock);
        return FALSE;
    }

    /* Return surrogate handles to the caller */
    *lpProcessInformation = pi_surr;

    /* ── 9. Start pump thread ─────────────────────────────────── */
    PumpContext *ctx = new PumpContext();
    ctx->ctrl_socket   = ctrl;
    ctx->stdout_socket = stdout_sock;
    ctx->stderr_socket = stderr_sock;
    ctx->hSurrogate    = pi_surr.hProcess;
    ctx->hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    ctx->hStderr = GetStdHandle(STD_ERROR_HANDLE);

    HANDLE hPump = CreateThread(NULL, 0, pump_thread, ctx, 0, NULL);
    if (hPump) {
        CloseHandle(hPump); /* detach — pump owns its lifetime */
    } else {
        TE_WARN(L"TransferEngine: pump thread failed (err %lu)"
                L" — stdout/stderr will not be streamed",
                GetLastError());
    }

    /* Resume surrogate so the caller's WaitForSingleObject works */
    ResumeThread(pi_surr.hThread);
    CloseHandle(pi_surr.hThread);
    /* hProcess stays in lpProcessInformation for the caller */

    TE_INFO(L"TransferEngine: '%s' offloaded successfully"
            L" (surrogate PID %lu)",
            profile->name, pi_surr.dwProcessId);
    return TRUE;
}

/* ============================================================
 *  DLL initialisation
 *  Called when offload_hook.dll loads this translation unit.
 * ============================================================ */

struct TEInit {
    TEInit()  {
        InitializeCriticalSection(&g_te_log_cs);
        g_te_log_cs_init = true;
    }
    ~TEInit() {
        g_te_log_cs_init = false;
        DeleteCriticalSection(&g_te_log_cs);
    }
} g_te_init;
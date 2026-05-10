/*
 * remote_agent.cpp
 * --------------------------------------------------------------
 * Remote node agent.
 *
 * Runs as a standalone process on the target machine.
 * Listens for incoming ProcessBundle connections from
 * TransferEngine, reconstructs and launches the process
 * locally, pumps stdout/stderr back, and sends the exit code.
 *
 * Current state: TCP transport (same stub as transfer_engine).
 * IMPLEMENT markers show where lsQUIC replaces TCP.
 *
 * Build (MSVC x64 Developer Command Prompt):
 *   cl /O2 /W4 /EHsc remote_agent.cpp ^
 *       /link ws2_32.lib kernel32.lib /out:remote_agent.exe
 *
 * Usage:
 *   remote_agent.exe [port]        (default port: 19876)
 * --------------------------------------------------------------
 */

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "transfer_engine.hpp"  /* wire constants shared with sender */

#pragma comment(lib, "ws2_32.lib")

/* ============================================================
 *  LOGGING
 * ============================================================ */

static void ra_log(const wchar_t *level, const wchar_t *fmt, ...)
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wprintf(L"[%02u:%02u:%02u.%03u] [%s] [remote_agent] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level, msg);
    fflush(stdout);
}

#define RA_INFO(fmt,...)  ra_log(L"INFO ", fmt, ##__VA_ARGS__)
#define RA_WARN(fmt,...)  ra_log(L"WARN ", fmt, ##__VA_ARGS__)
#define RA_ERROR(fmt,...) ra_log(L"ERROR", fmt, ##__VA_ARGS__)

/* ============================================================
 *  WIRE HELPERS  (receive from socket)
 * ============================================================ */

static bool recv_all(SOCKET s, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int r = recv(s, p, (int)len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}

static bool send_all(SOCKET s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int r = send(s, p, (int)len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}

static bool recv_u16(SOCKET s, uint16_t &v)
{
    return recv_all(s, &v, sizeof(v));
}

static bool recv_u32(SOCKET s, uint32_t &v)
{
    return recv_all(s, &v, sizeof(v));
}

static bool recv_u64(SOCKET s, uint64_t &v)
{
    return recv_all(s, &v, sizeof(v));
}

/* Receives a length-prefixed UTF-16 string into a std::wstring */
static bool recv_wstr(SOCKET s, std::wstring &out)
{
    uint16_t len = 0;
    if (!recv_u16(s, len)) return false;
    if (len == 0) { out.clear(); return true; }
    out.resize(len);
    return recv_all(s, &out[0], len * sizeof(wchar_t));
}

/* Receives size-prefixed bytes and writes them to a file */
static bool recv_file(SOCKET s, const wchar_t *dest_path,
                      uint64_t *size_out)
{
    uint64_t sz = 0;
    if (!recv_u64(s, sz)) return false;
    *size_out = sz;

    HANDLE hf = CreateFileW(dest_path, GENERIC_WRITE,
        0, NULL, CREATE_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    char buf[64 * 1024];
    uint64_t remaining = sz;
    bool ok = true;
    while (ok && remaining > 0) {
        DWORD chunk = (remaining < (uint64_t)sizeof(buf))
                    ? (DWORD)remaining : (DWORD)sizeof(buf);
        if (!recv_all(s, buf, chunk)) { ok = false; break; }
        DWORD written;
        if (!WriteFile(hf, buf, chunk, &written, NULL)
            || written != chunk) { ok = false; break; }
        remaining -= chunk;
    }

    CloseHandle(hf);
    if (!ok) DeleteFileW(dest_path);
    return ok;
}

/* ============================================================
 *  STDOUT / STDERR PUMP
 *  Reads from the process handles and forwards to the caller.
 * ============================================================ */

struct PumpCtx {
    HANDLE   hRead;     /* read end of pipe from child       */
    SOCKET   dest;      /* socket stream to forward bytes to */
};

static DWORD WINAPI pipe_pump(LPVOID param)
{
    PumpCtx *ctx = (PumpCtx *)param;
    char buf[4096];
    DWORD rd;
    while (ReadFile(ctx->hRead, buf, sizeof(buf), &rd, NULL) && rd > 0)
        send_all(ctx->dest, buf, rd);
    CloseHandle(ctx->hRead);
    closesocket(ctx->dest);
    delete ctx;
    return 0;
}

/* ============================================================
 *  HANDLE ONE CLIENT CONNECTION
 * ============================================================ */

struct ClientSockets {
    SOCKET ctrl;    /* Stream 0 — receives bundle, sends exit */
    SOCKET out;     /* Stream 2 — stdout forward              */
    SOCKET err;     /* Stream 3 — stderr forward              */
};

static void handle_client(ClientSockets socks)
{
    SOCKET ctrl = socks.ctrl;

    /* ── Receive bundle header ─────────────────────────────── */
    BundleHeader hdr = {};
    if (!recv_all(ctrl, &hdr, sizeof(hdr))) {
        RA_ERROR(L"Failed to receive bundle header"); return;
    }
    if (hdr.magic != BUNDLE_MAGIC || hdr.version != BUNDLE_VERSION) {
        RA_ERROR(L"Bad bundle magic/version: 0x%08X / %u",
                 hdr.magic, hdr.version);
        return;
    }

    RA_INFO(L"Bundle header OK — %u dep(s), flags=0x%08X",
            hdr.dep_count, hdr.flags);

    /* ── Receive string fields ─────────────────────────────── */
    std::wstring name, cmdline, cwd, env_block;
    if (!recv_wstr(ctrl, name)    ||
        !recv_wstr(ctrl, cmdline) ||
        !recv_wstr(ctrl, cwd)     ||
        !recv_wstr(ctrl, env_block)) {
        RA_ERROR(L"Failed to receive string fields"); return;
    }

    RA_INFO(L"Receiving '%s'", name.c_str());

    /* ── Create staging directory ──────────────────────────── */
    wchar_t stage[MAX_PATH];
    wchar_t tmp_base[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_base);
    _snwprintf_s(stage, MAX_PATH, _TRUNCATE,
                 L"%smm_%lu_%lu",
                 tmp_base,
                 GetCurrentProcessId(),
                 GetCurrentThreadId());
    CreateDirectoryW(stage, NULL);

    RA_INFO(L"Staging directory: %s", stage);

    /* ── Receive and write dep files ───────────────────────── */
    for (unsigned i = 0; i < hdr.dep_count; i++) {
        uint16_t name_len = 0;
        if (!recv_u16(ctrl, name_len)) {
            RA_ERROR(L"Failed to receive dep name length"); return;
        }
        std::wstring dep_name(name_len, L'\0');
        if (!recv_all(ctrl, &dep_name[0], name_len * sizeof(wchar_t))) {
            RA_ERROR(L"Failed to receive dep name"); return;
        }

        wchar_t dep_path[MAX_PATH];
        _snwprintf_s(dep_path, MAX_PATH, _TRUNCATE,
                     L"%s\\%s", stage, dep_name.c_str());

        uint64_t dep_sz = 0;
        if (!recv_file(ctrl, dep_path, &dep_sz)) {
            RA_ERROR(L"Failed to receive dep '%s'", dep_name.c_str());
            return;
        }
        RA_INFO(L"  dep '%s' written (%llu bytes)",
                dep_name.c_str(), dep_sz);
    }

    /* ── Receive and write main binary ─────────────────────── */
    wchar_t bin_path[MAX_PATH];
    _snwprintf_s(bin_path, MAX_PATH, _TRUNCATE,
                 L"%s\\%s", stage, name.c_str());

    uint64_t bin_sz = 0;
    if (!recv_file(ctrl, bin_path, &bin_sz)) {
        RA_ERROR(L"Failed to receive binary '%s'", name.c_str());
        return;
    }
    RA_INFO(L"Binary '%s' written (%llu bytes)", name.c_str(), bin_sz);

    /* ── Set up stdout / stderr pipes ─────────────────────── */
    HANDLE hStdoutRd, hStdoutWr, hStderrRd, hStderrWr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    bool pipes_ok =
        CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0) &&
        SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0) &&
        CreatePipe(&hStderrRd, &hStderrWr, &sa, 0) &&
        SetHandleInformation(hStderrRd, HANDLE_FLAG_INHERIT, 0);

    if (!pipes_ok) {
        RA_ERROR(L"Failed to create pipes (err %lu)", GetLastError());
        return;
    }

    /* ── Launch the process ─────────────────────────────────── */
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = hStdoutWr;
    si.hStdError   = hStderrWr;

    PROCESS_INFORMATION pi = {};

    /* Build mutable cmdline copy */
    std::wstring cmd_copy = cmdline;

    DWORD flags = CREATE_NO_WINDOW;
    if (hdr.flags & BUNDLE_FLAG_HAS_GUI)
        flags = 0; /* allow GUI */

    // Prepend staging directory to PATH in the environment block
    // so the launched process finds its bundled DLLs first.
    std::wstring patched_env;
    {
        // Get current PATH
        wchar_t cur_path[32768] = {};
        GetEnvironmentVariableW(L"PATH", cur_path, _countof(cur_path));
        std::wstring new_path = std::wstring(stage) + L";" + cur_path;

        // Build a new environment block: prepend PATH=new_path
        // then copy the caller's env block (or current environment).
        const wchar_t *src_env = env_block.empty()
            ? nullptr : env_block.data();

        // Collect existing env vars from src_env or current process
        // into a map, overriding PATH.
        wchar_t *proc_env = src_env ? nullptr : GetEnvironmentStringsW();
        const wchar_t *scan = src_env ? src_env
                            : (proc_env ? proc_env : L"\0");

        patched_env = L"PATH=" + new_path + L"\0";
        while (scan && *scan) {
            std::wstring var(scan);
            scan += var.size() + 1;
            // Skip any existing PATH= entry
            if (_wcsnicmp(var.c_str(), L"PATH=", 5) == 0) continue;
            patched_env += var + L"\0";
        }
        patched_env += L"\0"; // double-null terminator
        if (proc_env) FreeEnvironmentStringsW(proc_env);
    }

    BOOL launched = CreateProcessW(
        bin_path,
        &cmd_copy[0],
        NULL, NULL, TRUE,
        flags,
        (LPVOID)patched_env.data(),
        cwd.empty()        ? stage : cwd.c_str(),
        &si, &pi);

    /* Close write ends — child has its own copies */
    CloseHandle(hStdoutWr);
    CloseHandle(hStderrWr);

    if (!launched) {
        RA_ERROR(L"CreateProcessW failed (err %lu)", GetLastError());
        CloseHandle(hStdoutRd);
        CloseHandle(hStderrRd);
        return;
    }

    RA_INFO(L"'%s' launched (PID %lu)", name.c_str(), pi.dwProcessId);

    /* ── Start pipe pump threads ─────────────────────────────── */
    PumpCtx *ctx_out = new PumpCtx{ hStdoutRd, socks.out };
    PumpCtx *ctx_err = new PumpCtx{ hStderrRd, socks.err };

    HANDLE hPump1 = CreateThread(NULL, 0, pipe_pump, ctx_out, 0, NULL);
    HANDLE hPump2 = CreateThread(NULL, 0, pipe_pump, ctx_err, 0, NULL);
    if (hPump1) CloseHandle(hPump1);
    if (hPump2) CloseHandle(hPump2);

    /* ── Wait for process and send exit code ─────────────────── */
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    RA_INFO(L"'%s' exited with code %lu", name.c_str(), exit_code);

    /* Send exit packet on control stream */
    uint32_t exit_magic = EXIT_MAGIC;
    send_all(ctrl, &exit_magic, sizeof(exit_magic));
    send_all(ctrl, &exit_code,  sizeof(exit_code));

    /* ── Clean up staging directory ─────────────────────────────
     * Recursively delete all files then the directory itself.
     * We do this after WaitForSingleObject so the process has
     * fully exited and its file handles are closed.
     * ──────────────────────────────────────────────────────── */
    RA_INFO(L"Cleaning up staging directory: %s", stage);
    {
        wchar_t search[MAX_PATH];
        _snwprintf_s(search, MAX_PATH, _TRUNCATE, L"%s\\*", stage);
        WIN32_FIND_DATAW fd;
        HANDLE hf = FindFirstFileW(search, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 ||
                    wcscmp(fd.cFileName, L"..") == 0) continue;
                wchar_t full[MAX_PATH];
                _snwprintf_s(full, MAX_PATH, _TRUNCATE,
                             L"%s\\%s", stage, fd.cFileName);
                // Mark non-read-only so DeleteFileW succeeds
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    RemoveDirectoryW(full);
                else
                    DeleteFileW(full);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        SetFileAttributesW(stage, FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(stage))
            RA_INFO(L"Staging directory cleaned up.");
        else
            RA_WARN(L"Could not fully remove staging dir (err %lu). "
                    L"Some files may still be in use.", GetLastError());
    }

    closesocket(ctrl);
}

/* ============================================================
 *  MAIN — listen loop
 * ============================================================ */

int wmain(int argc, wchar_t *argv[])
{
    int port = REMOTE_AGENT_PORT;
    if (argc >= 2) port = _wtoi(argv[1]);

    WSADATA wsd;
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        RA_ERROR(L"WSAStartup failed"); return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        RA_ERROR(L"socket() failed"); return 1;
    }

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               (char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((u_short)port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        RA_ERROR(L"bind() failed (err %d)", WSAGetLastError());
        return 1;
    }

    if (listen(listener, SOMAXCONN) != 0) {
        RA_ERROR(L"listen() failed"); return 1;
    }

    RA_INFO(L"remote_agent listening on port %d", port);
    RA_INFO(L"Waiting for bundles from TransferEngine...");

    /*
     * IMPLEMENT: replace the TCP accept loop below with lsQUIC
     * server initialisation.  Each incoming QUIC connection maps
     * to one offloaded process.  Streams 0/2/3 map to ctrl/out/err.
     *
     * The handle_client() function does not need to change —
     * only the socket values in ClientSockets need to be replaced
     * with QUIC stream handles (cast to SOCKET for now, or
     * refactor ClientSockets to hold lsquic_stream_t*).
     */

    while (true) {
        /* Accept control connection (Stream 0) */
        SOCKET ctrl = accept(listener, NULL, NULL);
        if (ctrl == INVALID_SOCKET) continue;

        /* Accept stdout and stderr connections (Streams 2 & 3).
         * With TCP these are separate accepted sockets; with QUIC
         * they would be additional streams on the same connection. */
        SOCKET out = accept(listener, NULL, NULL);
        SOCKET err = accept(listener, NULL, NULL);

        RA_INFO(L"Incoming bundle connection accepted");

        /* Handle each bundle on a new thread */
        ClientSockets *socks = new ClientSockets{ ctrl, out, err };

        CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
            ClientSockets s = *(ClientSockets *)p;
            delete (ClientSockets *)p;
            handle_client(s);
            return 0;
        }, socks, 0, NULL);
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}
/*
 * install_hook.cpp
 * --------------------------------------------------------------
 * Installs and uninstalls hook.dll as a system-wide AppInit DLL.
 *
 * AppInit_DLLs causes Windows to load the named DLL into every
 * process that loads user32.dll — which includes every interactive
 * desktop application a user launches.
 *
 * Two registry hives are written on 64-bit Windows:
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows
 *     (covers 64-bit processes)
 *   HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Windows
 *     (covers 32-bit processes running under WOW64)
 *
 * REQUIREMENTS:
 *   - Must be run as Administrator (registry write to HKLM).
 *   - hook.dll must be in a stable, permanent path before install.
 *   - If Secure Boot / RequireSignedAppInit_DLLs is enforced,
 *     hook.dll must be signed, or that policy must be relaxed in
 *     a test environment.
 *   - A reboot or log-off/log-on is NOT required — the setting
 *     takes effect for every new process launched after the keys
 *     are written.  Already-running processes are unaffected.
 *
 * Build (MSVC x64 Developer Command Prompt):
 *   cl /O2 /W4 install_hook.cpp /link advapi32.lib /out:install_hook.exe
 *
 * Usage:
 *   install_hook.exe install   C:\full\path\to\hook.dll
 *   install_hook.exe uninstall
 * --------------------------------------------------------------
 */

/* UAC elevation is requested via a manifest file (install_hook.manifest).
 * CMakeLists.txt embeds it at link time via target properties.
 * No pragma needed here. */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

/* ============================================================
 *  REGISTRY KEY PATHS
 * ============================================================ */

static const WCHAR *REG_KEYS[] = {
    /* 64-bit processes (and all processes on 32-bit Windows) */
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
    /* 32-bit processes on 64-bit Windows (WOW64 node) */
    L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
    NULL
};

static const WCHAR *VAL_DLL      = L"AppInit_DLLs";
static const WCHAR *VAL_ENABLE   = L"LoadAppInit_DLLs";
static const WCHAR *VAL_UNSIGNED = L"RequireSignedAppInit_DLLs";

/* ============================================================
 *  HELPERS
 * ============================================================ */

static void print_last_error(const WCHAR *context, LONG err)
{
    WCHAR buf[256] = L"(no message)";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)err, 0, buf, ARRAYSIZE(buf), NULL);
    /* strip trailing newline */
    size_t len = wcslen(buf);
    while (len > 0 && (buf[len-1] == L'\r' || buf[len-1] == L'\n'))
        buf[--len] = L'\0';
    wprintf(L"  [ERROR] %s: %s (%ld)\n", context, buf, err);
}

/*
 * set_appinitdll_key
 * Writes or clears AppInit_DLLs in one registry key path.
 * dll_path == NULL means uninstall (clear and disable).
 */
static BOOL set_appinitdll_key(const WCHAR *key_path, const WCHAR *dll_path)
{
    HKEY hk = NULL;
    LONG rc  = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path,
                              0, KEY_SET_VALUE, &hk);
    if (rc != ERROR_SUCCESS) {
        print_last_error(key_path, rc);
        return FALSE;
    }

    BOOL ok = TRUE;

    if (dll_path) {
        /* ── Install ─────────────────────────────────────────── */

        /* Write the DLL path */
        rc = RegSetValueExW(hk, VAL_DLL, 0, REG_SZ,
                            (const BYTE *)dll_path,
                            (DWORD)((wcslen(dll_path) + 1) * sizeof(WCHAR)));
        if (rc != ERROR_SUCCESS) {
            print_last_error(L"Set AppInit_DLLs", rc);
            ok = FALSE;
            goto done;
        }

        /* Enable loading */
        DWORD one = 1;
        rc = RegSetValueExW(hk, VAL_ENABLE, 0, REG_DWORD,
                            (const BYTE *)&one, sizeof(one));
        if (rc != ERROR_SUCCESS) {
            print_last_error(L"Set LoadAppInit_DLLs", rc);
            ok = FALSE;
            goto done;
        }

        /* Disable signature requirement (test environments only).
         * On production machines this value should remain 1 and
         * hook.dll must carry a valid Authenticode signature.     */
        DWORD zero = 0;
        rc = RegSetValueExW(hk, VAL_UNSIGNED, 0, REG_DWORD,
                            (const BYTE *)&zero, sizeof(zero));
        if (rc != ERROR_SUCCESS) {
            /* Non-fatal — key may not exist on all Windows versions */
            wprintf(L"  [WARN ] RequireSignedAppInit_DLLs not set (%ld)"
                    L" — may need signed DLL\n", rc);
        }

        wprintf(L"  [OK   ] %s\n", key_path);

    } else {
        /* ── Uninstall ───────────────────────────────────────── */

        /* Clear the path */
        rc = RegSetValueExW(hk, VAL_DLL, 0, REG_SZ,
                            (const BYTE *)L"",
                            (DWORD)sizeof(WCHAR));
        if (rc != ERROR_SUCCESS) {
            print_last_error(L"Clear AppInit_DLLs", rc);
            ok = FALSE;
            goto done;
        }

        /* Disable loading */
        DWORD zero = 0;
        rc = RegSetValueExW(hk, VAL_ENABLE, 0, REG_DWORD,
                            (const BYTE *)&zero, sizeof(zero));
        if (rc != ERROR_SUCCESS) {
            print_last_error(L"Clear LoadAppInit_DLLs", rc);
            ok = FALSE;
            goto done;
        }

        wprintf(L"  [OK   ] %s\n", key_path);
    }

done:
    RegCloseKey(hk);
    return ok;
}

/* ============================================================
 *  INSTALL
 * ============================================================ */

static int do_install(const WCHAR *dll_path)
{
    /* Verify the file actually exists before writing the registry */
    DWORD attr = GetFileAttributesW(dll_path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"[ERROR] hook.dll not found at: %s\n", dll_path);
        wprintf(L"        Copy hook.dll to a stable path first.\n");
        return 1;
    }

    wprintf(L"[install] Writing AppInit_DLLs = %s\n\n", dll_path);

    BOOL all_ok = TRUE;
    for (int i = 0; REG_KEYS[i]; i++)
        if (!set_appinitdll_key(REG_KEYS[i], dll_path))
            all_ok = FALSE;

    if (all_ok) {
        wprintf(L"\n[install] Done.\n");
        wprintf(L"          hook.dll will be loaded into every new GUI\n");
        wprintf(L"          process (user32.dll consumer) from this point.\n");
        wprintf(L"          Check C:\\Temp\\hook.log after launching any app.\n");
    } else {
        wprintf(L"\n[install] Completed with errors — see above.\n");
        wprintf(L"          Are you running as Administrator?\n");
    }

    return all_ok ? 0 : 1;
}

/* ============================================================
 *  UNINSTALL
 * ============================================================ */

static int do_uninstall(void)
{
    wprintf(L"[uninstall] Clearing AppInit_DLLs ...\n\n");

    BOOL all_ok = TRUE;
    for (int i = 0; REG_KEYS[i]; i++)
        if (!set_appinitdll_key(REG_KEYS[i], NULL))
            all_ok = FALSE;

    if (all_ok) {
        wprintf(L"\n[uninstall] Done.\n");
        wprintf(L"            hook.dll will no longer be injected into\n");
        wprintf(L"            new processes. Already-running processes\n");
        wprintf(L"            still have it loaded until they exit.\n");
    } else {
        wprintf(L"\n[uninstall] Completed with errors — see above.\n");
        wprintf(L"            Are you running as Administrator?\n");
    }

    return all_ok ? 0 : 1;
}

/* ============================================================
 *  QUERY — show current state
 * ============================================================ */

static int do_query(void)
{
    wprintf(L"[query] Current AppInit_DLLs state:\n\n");

    for (int i = 0; REG_KEYS[i]; i++) {
        HKEY hk = NULL;
        LONG rc  = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEYS[i],
                                  0, KEY_QUERY_VALUE, &hk);
        if (rc != ERROR_SUCCESS) {
            wprintf(L"  %s\n    (could not open key: %ld)\n\n",
                    REG_KEYS[i], rc);
            continue;
        }

        WCHAR path[MAX_PATH] = L"(empty)";
        DWORD path_sz        = sizeof(path);
        DWORD enabled        = 0;
        DWORD enabled_sz     = sizeof(enabled);

        RegQueryValueExW(hk, VAL_DLL, NULL, NULL,
                         (BYTE *)path, &path_sz);
        RegQueryValueExW(hk, VAL_ENABLE, NULL, NULL,
                         (BYTE *)&enabled, &enabled_sz);

        wprintf(L"  %s\n"
                L"    LoadAppInit_DLLs : %lu\n"
                L"    AppInit_DLLs     : %s\n\n",
                REG_KEYS[i], enabled,
                path[0] ? path : L"(empty)");

        RegCloseKey(hk);
    }

    return 0;
}

/* ============================================================
 *  MAIN
 * ============================================================ */

int wmain(int argc, WCHAR *argv[])
{
    if (argc < 2) {
        wprintf(
            L"Usage:\n"
            L"  install_hook.exe install   <full\\path\\to\\hook.dll>\n"
            L"  install_hook.exe uninstall\n"
            L"  install_hook.exe query\n"
            L"\n"
            L"Must be run as Administrator.\n");
        return 1;
    }

    if (_wcsicmp(argv[1], L"install") == 0) {
        if (argc < 3) {
            wprintf(L"[ERROR] install requires the full path to hook.dll\n");
            return 1;
        }
        return do_install(argv[2]);
    }

    if (_wcsicmp(argv[1], L"uninstall") == 0)
        return do_uninstall();

    if (_wcsicmp(argv[1], L"query") == 0)
        return do_query();

    wprintf(L"[ERROR] Unknown command '%s'\n", argv[1]);
    return 1;
}
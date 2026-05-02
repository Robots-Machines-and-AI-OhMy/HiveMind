//
// Hook_Test.cpp
// -------------------------------------------------------------
// Proof-of-life test runner for the hook + offload pipeline.
//
// What this does:
//   1. Loads offload_hook.dll and calls Inject() to supply the
//      engine stubs to hook.dll.
//
//   2. If AppInit_DLLs is already installed (install_hook.exe
//      was run as Administrator beforehand), hook.dll is already
//      loaded system-wide and Inject() just wires in the engines.
//      If not, hook.dll is loaded only into this process — useful
//      for isolated testing without admin rights.
//
//   3. Launches one or more processes.  Each non-OS process is:
//        - Caught and profiled by hook.dll
//        - Queued for up to 200 ms waiting for AskEngine
//        - Run locally after timeout (engines are stubs)
//        - Logged in full to C:\Temp\hook.log
//
//   4. Waits for all launched processes to exit, then Ejects.
//
// To add more test cases: add CreateProcessW calls in the
// "Launch test processes" section below.
// -------------------------------------------------------------

#include <windows.h>
#include <iostream>
#include <vector>

typedef BOOL (WINAPI *InjectFn)(void);
typedef void (WINAPI *EjectFn)(void);

int main()
{
    // ── 1. Load offload_hook.dll ─────────────────────────────
    HMODULE hOffload = LoadLibraryW(L"offload_hook.dll");
    if (!hOffload) {
        std::wcerr << L"[test_runner] Failed to load offload_hook.dll"
                      L" (err " << GetLastError() << L")\n";
        return 1;
    }

    InjectFn Inject = (InjectFn)GetProcAddress(hOffload, "Inject");
    EjectFn  Eject  = (EjectFn) GetProcAddress(hOffload, "Eject");

    if (!Inject || !Eject) {
        std::wcerr << L"[test_runner] Inject/Eject not found in"
                      L" offload_hook.dll\n";
        FreeLibrary(hOffload);
        return 1;
    }

    // ── 2. Inject — wires engine stubs into hook.dll ─────────
    if (!Inject()) {
        std::wcerr << L"[test_runner] Inject() failed\n";
        FreeLibrary(hOffload);
        return 1;
    }

    std::wcout << L"[test_runner] Hook active.\n"
               << L"             Non-OS processes will be caught, queued,\n"
               << L"             timed out (200 ms), and run locally.\n"
               << L"             Log: C:\\Temp\\hook.log\n\n";

    // ── 3. Launch test processes ─────────────────────────────
    // Add or remove entries here to test different binaries.
    // System32 paths will be filtered out by hook.dll (SKIP in log).
    // Desktop / user-installed apps will be caught (Caught: in log).
    const wchar_t *targets[] = {
        L"C:\\Windows\\System32\\notepad.exe",   // expect: SKIP (System32)
        L"C:\\Windows\\System32\\calc.exe",      // expect: SKIP (System32)
        // Add a non-OS binary here to see a full queue+timeout cycle:
        // L"C:\\Users\\Public\\Desktop\\SomeApp.exe",
        nullptr
    };

    std::vector<PROCESS_INFORMATION> procs;

    for (int i = 0; targets[i]; i++) {
        STARTUPINFOW        si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};

        // Pass a mutable copy of the path as lpCommandLine
        // (CreateProcessW may write to it internally).
        wchar_t cmd[MAX_PATH];
        wcsncpy_s(cmd, targets[i], _TRUNCATE);

        BOOL ok = CreateProcessW(
            targets[i],         // lpApplicationName
            cmd,                // lpCommandLine (mutable copy)
            nullptr, nullptr,
            FALSE, 0,
            nullptr, nullptr,
            &si, &pi);

        if (ok) {
            std::wcout << L"[test_runner] Launched: " << targets[i]
                       << L" (PID " << pi.dwProcessId << L")\n";
            procs.push_back(pi);
        } else {
            std::wcerr << L"[test_runner] Failed to launch: " << targets[i]
                       << L" (err " << GetLastError() << L")\n";
        }
    }

    // ── 4. Wait for all launched processes to exit ───────────
    std::wcout << L"\n[test_runner] Waiting for processes to exit...\n";

    for (auto &pi : procs) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        std::wcout << L"[test_runner] PID " << pi.dwProcessId
                   << L" exited (code " << exit_code << L")\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // ── 5. Eject and clean up ────────────────────────────────
    Eject();
    FreeLibrary(hOffload);

    std::wcout << L"\n[test_runner] Done. Check C:\\Temp\\hook.log\n";
    return 0;
}
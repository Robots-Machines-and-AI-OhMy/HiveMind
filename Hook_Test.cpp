//
// Created by jason on 5/1/2026.
//

#include <windows.h>
#include <iostream>

typedef BOOL (WINAPI *InjectFn)(void);

int main() {
    HMODULE inj = LoadLibraryW(L"injector.dll");
    if (!inj) {
        std::cout << "Failed to load injector.dll\n";
        return 1;
    }

    InjectFn Inject = (InjectFn)GetProcAddress(inj, "Inject");
    if (!Inject || !Inject()) {
        std::cout << "Inject failed\n";
        return 1;
    }

    std::cout << "Injection active. Launching process...\n";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    CreateProcessW(
        L"C:\\Windows\\System32\\notepad.exe",
        NULL, NULL, NULL, FALSE, 0,
        NULL, NULL, &si, &pi
    );

    WaitForSingleObject(pi.hProcess, INFINITE);

    return 0;
}

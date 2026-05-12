/*
 * HiveMindCLI.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Interactive command-line interface for the HiveMind distributed offloading
 * system.
 *
 * On startup:
 *   1. Runs the benchmark and prints the local metric score.
 *   2. Loads offload_hook.dll and calls Inject() to wire the engines into
 *      hook.dll (if hook.dll is registered system-wide via AppInit_DLLs,
 *      Inject supplies the engines; if not, it loads hook.dll in-process only).
 *
 * On exit (any path — 'exit' command, Ctrl-C, or unhandled exception):
 *   3. Ejects the hook (clears engine pointers).
 *   4. Disconnects from the HiveMind network if connected.
 *   5. Calls install_hook.exe uninstall (elevated) to clear AppInit_DLLs so
 *      hook.dll is no longer injected into new processes after we exit.
 *
 * Commands:
 *   help        print this list
 *   create      create a new HiveMind network (you become leader)
 *   scan        discover nearby HiveMind networks
 *   join        join an existing network
 *   hook        install hook.dll system-wide (requires admin, prompts UAC)
 *   unhook      uninstall hook.dll system-wide
 *   status      show connection and node info
 *   leader      show current leader IP
 *   disconnect  leave the network
 *   exit        clean shutdown
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "Network.hpp"
#include "compute_check.hpp"
#include "global.hpp"
#include "global_strings.hpp"
#include <shellapi.h>

#include <iostream>
#include <string>
#include <sstream>

// ── Hook DLL management ───────────────────────────────────────────────────────

typedef BOOL (WINAPI *FnInject)(void);
typedef void (WINAPI *FnEject)(void);

static HMODULE  g_offload_dll  = NULL;
static FnInject g_inject_fn    = NULL;
static FnEject  g_eject_fn     = NULL;

static void hook_load()
{
    if (g_offload_dll) return;
    g_offload_dll = LoadLibraryW(L"offload_hook.dll");
    if (!g_offload_dll) {
        std::cout << "[hook] offload_hook.dll not found in PATH — "
                     "offloading disabled.\n";
        return;
    }
    g_inject_fn = (FnInject)GetProcAddress(g_offload_dll, "Inject");
    g_eject_fn  = (FnEject) GetProcAddress(g_offload_dll, "Eject");

    if (g_inject_fn && g_inject_fn()) {
        std::cout << "[hook] Engine wired into hook.dll.\n";
    } else {
        std::cout << "[hook] Inject() failed — hook.dll may not be active.\n";
    }
}

static void hook_unload()
{
    if (!g_offload_dll) return;
    if (g_eject_fn) g_eject_fn();
    FreeLibrary(g_offload_dll);
    g_offload_dll = NULL;
    g_inject_fn   = NULL;
    g_eject_fn    = NULL;
}

// ── System-wide hook install / uninstall via install_hook.exe ─────────────────

// Locate install_hook.exe relative to our own binary.
static std::wstring find_install_hook()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *last = wcsrchr(self, L'\\');
    if (last) *(last + 1) = L'\0';
    std::wstring path = std::wstring(self) + L"install_hook.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"";
    return path;
}

// Run install_hook.exe with the given args; returns exit code or -1 on failure.
// ShellExecuteExW is used so Windows can prompt UAC if needed.
static int run_install_hook(const std::wstring& args)
{
    std::wstring exe = find_install_hook();
    if (exe.empty()) {
        std::cout << "[hook] install_hook.exe not found next to HiveMind.exe\n";
        return -1;
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask       = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb      = L"runas";          // request elevation
    sei.lpFile      = exe.c_str();
    sei.lpParameters= args.c_str();
    sei.nShow       = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            std::cout << "[hook] UAC prompt cancelled.\n";
        else
            std::cout << "[hook] ShellExecuteExW failed (err " << err << ")\n";
        return -1;
    }

    WaitForSingleObject(sei.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static void cmd_hook_install()
{
    // Get full path to hook.dll next to us
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t *last = wcsrchr(self, L'\\');
    if (last) *(last + 1) = L'\0';
    std::wstring hook_path = std::wstring(self) + L"hook.dll";

    if (GetFileAttributesW(hook_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[hook] hook.dll not found at: ";
        std::wcout << hook_path << L"\n";
        return;
    }

    std::wstring args = L"install \"" + hook_path + L"\"";
    std::cout << "[hook] Installing hook.dll system-wide (UAC prompt may appear)...\n";
    int rc = run_install_hook(args);
    if (rc == 0)
        std::cout << "[hook] Installed. New processes will be intercepted.\n";
    else
        std::cout << "[hook] Install failed (code " << rc << ").\n";
}

static void cmd_hook_uninstall()
{
    std::cout << "[hook] Removing hook.dll from AppInit_DLLs "
                 "(UAC prompt may appear)...\n";
    int rc = run_install_hook(L"uninstall");
    if (rc == 0)
        std::cout << "[hook] Uninstalled. New processes will not be intercepted.\n";
    else
        std::cout << "[hook] Uninstall failed (code " << rc << ").\n";
}

// ── Clean shutdown ─────────────────────────────────────────────────────────────

static void shutdown()
{
    NetworkManager& net = NetworkManager::getInstance();

    if (net.isConnected()) {
        std::cout << "[exit] Disconnecting from network...\n";
        net.disconnect();
    }
    net.cleanup();

    hook_unload();

    // Always attempt to clear AppInit_DLLs on exit — silent (SW_HIDE),
    // best-effort.  If the user never ran 'hook', this is a no-op.
    std::wstring exe = find_install_hook();
    if (!exe.empty()) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb       = L"runas";
        sei.lpFile       = exe.c_str();
        sei.lpParameters = L"uninstall";
        sei.nShow        = SW_HIDE;
        if (ShellExecuteExW(&sei)) {
            WaitForSingleObject(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
        }
    }
}

// Ctrl-C / console close handler
static BOOL WINAPI console_handler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT)
    {
        std::cout << "\n[exit] Signal received — shutting down...\n";
        shutdown();
        return FALSE;   // let Windows terminate the process
    }
    return FALSE;
}

// ── Command helpers ────────────────────────────────────────────────────────────

static void cmd_help()
{
    std::cout <<
        "\nAvailable commands:\n"
        "  help        Show this list\n"
        "  create      Create a new HiveMind network (you become leader)\n"
        "  scan        Discover nearby HiveMind networks\n"
        "  join        Join an existing network\n"
        "  hook        Install hook.dll system-wide (intercept processes)\n"
        "  unhook      Remove hook.dll from AppInit_DLLs\n"
        "  status      Show connection and node info\n"
        "  leader      Show current leader IP\n"
        "  disconnect  Leave the current network\n"
        "  exit        Clean shutdown\n\n";
}

static void cmd_create()
{
	NetworkManager& net = NetworkManager::getInstance();
    if (net.isConnected()) {
        std::cout << "Already connected. Run 'disconnect' first.\n\n";
        return;
    }
	
    std::string name, password;
    std::cout << "Network name: ";
    std::cin >> name;
    std::cout << "Password (or NA for none): ";
    std::cin >> password;
    if (password == "NA") password = "";

    
    if (net.createNetwork(name, password))
        std::cout << "Network '" << name << "' created. You are the leader.\n\n";
    else
        std::cout << "Failed to create network.\n\n";
}

static void cmd_scan()
{
    std::cout << "Scanning...\n";
    NetworkManager& net = NetworkManager::getInstance();
    if (!net.init()) {
        std::cout << "Network init failed.\n\n";
        return;
    }
    auto results = net.scan();
    if (results.empty()) {
        std::cout << "No HiveMind networks found.\n\n";
        return;
    }
    std::cout << "Found " << results.size() << " network(s):\n";
    int i = 1;
    for (auto& r : results) {
        std::cout << "  " << i++ << ". " << r.name
                  << " | Leader: " << r.leader_ip
                  << " | Password: " << (r.has_password ? "Yes" : "No")
                  << "\n";
    }
    std::cout << "\n";
}

static void cmd_join()
{
	NetworkManager& net = NetworkManager::getInstance();
    if (net.isConnected()) {
        std::cout << "Already connected. Run 'disconnect' first.\n\n";
        return;
    }
	
    std::string leader_ip, name, password;
    std::cout << "Leader IP: ";
    std::cin >> leader_ip;
    std::cout << "Network name: ";
    std::cin >> name;
    std::cout << "Password (or NA for none): ";
    std::cin >> password;
    if (password == "NA") password = "";

    if (net.joinNetwork(leader_ip, name, password))
        std::cout << "Joined network '" << name << "' via " << leader_ip << ".\n\n";
    else
        std::cout << "Failed to join network (wrong password or leader unreachable).\n\n";
}

static void cmd_status()
{
    NetworkManager& net = NetworkManager::getInstance();
    if (!net.isConnected()) {
        std::cout << "Not connected to any network.\n\n";
        return;
    }
    std::cout << "\n" << net.statusString() << "\n";
}

static void cmd_leader()
{
    NetworkManager& net = NetworkManager::getInstance();
    if (!net.isConnected()) {
        std::cout << "Not connected.\n\n";
        return;
    }
    std::string lip = net.leaderIp();
    std::cout << "Leader: " << (lip.empty() ? "election in progress" : lip)
              << "\n\n";
}

static void cmd_disconnect()
{
    NetworkManager& net = NetworkManager::getInstance();
    if (!net.isConnected()) {
        std::cout << "Not connected.\n\n";
        return;
    }
    net.disconnect();
    std::cout << "Disconnected.\n\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    // Register Ctrl-C / close handler for clean teardown
    SetConsoleCtrlHandler(console_handler, TRUE);

    // Benchmark
    metric = metric_calculation();
    std::cout << "==============================================\n";
    std::cout << "     HiveMind  -  Distributed Offloading\n";
    std::cout << "==============================================\n";
    std::cout << "Benchmark score : " << metric << "\n\n";

    // Initialise network layer
    NetworkManager& net = NetworkManager::getInstance();
    net.init();

    // Load offload engines (non-fatal if DLL missing)
    hook_load();

    std::cout << "Type 'help' to see commands.\n\n";

    std::string input;
    while (true) {
        std::cout << "HiveMind> ";
        if (!(std::cin >> input)) break;   // EOF / pipe closed

        if      (input == "help")       cmd_help();
        else if (input == "create")     cmd_create();
        else if (input == "scan")       cmd_scan();
        else if (input == "join")       cmd_join();
        else if (input == "hook")       cmd_hook_install();
        else if (input == "unhook")     cmd_hook_uninstall();
        else if (input == "status")     cmd_status();
        else if (input == "leader")     cmd_leader();
        else if (input == "disconnect") cmd_disconnect();
        else if (input == "exit")       break;
        else
            std::cout << "Unknown command '" << input
                      << "'. Type 'help'.\n\n";
    }

    std::cout << "Shutting down...\n";
    shutdown();
    std::cout << "Closed.\n";
    return 0;
}
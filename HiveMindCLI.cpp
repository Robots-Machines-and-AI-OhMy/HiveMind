#include <iostream>
#include <string>
#include <vector>
#include "Network.hpp"
#include "compute_check.hpp"
#include "global.hpp"
#include "global_strings.hpp"

using namespace std;

enum Command {
    HELP,
    CREATE,
    SCAN,
    JOIN,
    STATUS,
    GETLEADER,
    DISCONNECT,
    EXIT,
    UNKNOWN
};

Command getCommand(const string& cmd) {
    if (cmd == "help") return HELP;
    if (cmd == "create") return CREATE;
    if (cmd == "scan") return SCAN;
    if (cmd == "join") return JOIN;
    if (cmd == "status") return STATUS;
    if (cmd == "leader") return GETLEADER;
    if (cmd == "disconnect") return DISCONNECT;
    if (cmd == "exit") return EXIT;
    return UNKNOWN;
}

int main() {
    metric = metric_calculation();
    string input;
    string networkName;
    string password;

    bool running = true;

    NetworkManager& NetManager = NetworkManager::getInstance();
    NetManager.init();

    cout << "Metric Score: " << metric << endl;
    cout << "=============================\n";
    cout << "     HiveMind CLI System\n";
    cout << "=============================\n";
    cout << "Type 'help' to see commands.\n\n";

    while (running) {
        cout << "HiveMind> ";
        cin >> input;

        switch (getCommand(input)) {

            case HELP:
                cout << "Available Commands:\n";
                cout << "create      - Create a HiveMind network\n";
                cout << "scan        - Search for HiveMind networks\n";
                cout << "join        - Join a network\n";
                cout << "status      - View current network status\n";
                cout << "leader      - Show current leader node\n";
                cout << "disconnect  - Leave the network\n";
                cout << "exit        - Close HiveMind\n\n";
                break;

            case CREATE:
                cout << "Create your network\n";
                cout << "Enter the name of your network: ";
                cin >> networkName;

                cout << "Enter the password for your network or NA for no password: ";
                cin >> password;

                if (password == "NA") password = "";
                if (NetManager.createNetwork(networkName, password))
                    cout << "Network '" << networkName << "' created. You are the leader.\n\n";
                else
                    cout << "Error: Could not create network.\n\n";
                break;

            case SCAN: {
                cout << "Scanning local network...\n";
                vector<ScanResult> results = NetManager.scan();
                if (results.empty()) {
                    cout << "No HiveMind networks found.\n\n";
                } else {
                    cout << "Found networks:\n";
                    for (size_t i = 0; i < results.size(); ++i) {
                        cout << (i + 1) << ". "
                             << results[i].name
                             << " | Leader: " << results[i].leader_ip
                             << " | Password: " << (results[i].has_password ? "Yes" : "No")
                             << "\n";
                    }
                    cout << "\n";
                }
                break;
            }

            case JOIN: {
                cout << "Scanning for networks...\n";
                vector<ScanResult> results = NetManager.scan();
                if (results.empty()) {
                    cout << "No networks found. Use 'scan' first.\n\n";
                    break;
                }

                for (size_t i = 0; i < results.size(); ++i) {
                    cout << (i + 1) << ". "
                         << results[i].name
                         << " (" << results[i].leader_ip << ")"
                         << (results[i].has_password ? " [password required]" : "")
                         << "\n";
                }

                cout << "Enter network name: ";
                cin >> networkName;

                cout << "Enter password: ";
                cin >> password;

                string leader_ip;
                for (auto& r : results)
                    if (r.name == networkName) { leader_ip = r.leader_ip; break; }

                if (leader_ip.empty()) {
                    cout << "Error: Network '" << networkName << "' not found in scan results.\n\n";
                    break;
                }

                if (NetManager.joinNetwork(leader_ip, networkName, password))
                    cout << "Connected to " << networkName << " successfully.\n\n";
                else
                    cout << "Error: Connection attempt failed.\n\n";
                break;
            }

            case STATUS:
                if (NetManager.isConnected()) {
                    cout << "Network Status:\n";
                    cout << NetManager.statusString() << "\n\n";
                } else {
                    cout << "Error: Not connected to a HiveMind network.\n\n";
                }
                break;

            case GETLEADER:
                if (NetManager.isConnected()) {
                    string lip = NetManager.leaderIp();
                    cout << "Current Leader Node: "
                         << (lip.empty() ? "election in progress" : lip)
                         << "\n\n";
                } else {
                    cout << "Error: Not connected to a HiveMind network.\n\n";
                }
                break;

            case DISCONNECT:
                if (NetManager.isConnected()) {
                    cout << "Leaving HiveMind network...\n";
                    NetManager.disconnect();
                    cout << "Safely disconnected from network.\n\n";
                } else {
                    cout << "Error: No active network connection.\n\n";
                }
                break;

            case EXIT:
                cout << "Closing HiveMind CLI...\n";
                running = false;
                break;

            case UNKNOWN:
            default:
                cout << "Error: Unknown command. Type 'help' for command list.\n\n";
                break;
        }
    }

    NetManager.cleanup();
    cout << "Closed";
    return 0;
}
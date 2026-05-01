#include <iostream>
#include <string>
#include "network.hpp"
using namespace std;

enum Command {
    HELP,
    CREATE,
    SCAN,
    CONNECT,
    STATUS,
    LEADER,
    DISCONNECT,
    EXIT,
    UNKNOWN
};

Command getCommand(const string& cmd) {
    if (cmd == "help") return HELP;
    if (cmd == "Create") return CREATE;
    if (cmd == "scan") return SCAN;
    if (cmd == "connect") return CONNECT;
    if (cmd == "status") return STATUS;
    if (cmd == "leader") return LEADER;
    if (cmd == "disconnect") return DISCONNECT;
    if (cmd == "exit") return EXIT;
    return UNKNOWN;
}

int main() {
    string input;
    bool connected = false;
    bool running = true;
    NetworkManager* NetManager = NetworkManager::getNetworkManager(running);

    cout << "=============================\n";
    cout << "     HiveMind CLI System\n";
    cout << "=============================\n";
    cout << "Type 'help' to see commands.\n\n";

    while (running) {
        cout << "HiveMind> ";
        cin >> input;

        switch (getCommand(input)) {
            case HELP:
                cout << "\nAvailable Commands:\n";
                cout << "scan        - Search for HiveMind networks\n";
                cout << "connect     - Connect to a network\n";
                cout << "status      - View current network status\n";
                cout << "leader      - Show current leader node\n";
                cout << "disconnect  - Leave the network\n";
                cout << "exit        - Close HiveMind\n\n";
                break;
            case CREATE:
                string networkName, password;
                cout << "\nCreate your network\n";
                cout << "Enter the name of your network\n";
                cin >> networkName;
                cout << "Enter the password for your network or press enter for no password\n";
                cin >> password;
                NetManager->createNetwork(networkName, password);
                break;

            case SCAN:
                cout << "\nScanning local network...\n";
                cout << "Found networks:\n";
                cout << "1. " + networkName + "password yes\n";
                cout << "2. HiveMind-Lab | Password: Yes\n";
                cout << "3. HiveMind-Test | Password: No\n\n";
                break;

            case CONNECT:

                cout << "Enter network name: ";
                cin >> networkName;

                if (networkName == "HiveMind-Lab") {
                    cout << "Enter password: ";
                    cin >> password;

                    if (password == "1234") {
                        connected = true;
                        cout << "Connected to HiveMind-Lab successfully.\n\n";
                    } else {
                        cout << "Error: Connection attempt failed.\n\n";
                    }
                }
                else if (networkName == "HiveMind-Test") {
                    connected = true;
                    cout << "Connected to HiveMind-Test successfully.\n\n";
                }
                else {
                    cout << "Error: Connection attempt failed.\n\n";
                }
                break;
            }

            case STATUS:
                if (connected) {
                    cout << "\nNetwork Status:\n";
                    cout << "Connected: Yes\n";
                    cout << "Node Role: Follower\n";
                    cout << "CPU Usage: 34%\n";
                    cout << "Memory Usage: 58%\n";
                    cout << "Connected Nodes: 4\n\n";
                } else {
                    cout << "Error: Not connected to a HiveMind network.\n\n";
                }
                break;

            case LEADER:
                if (connected) {
                    cout << "Current Leader Node: 192.168.1.10\n\n";
                } else {
                    cout << "Error: Not connected to a HiveMind network.\n\n";
                }
                break;

            case DISCONNECT:
                if (connected) {
                    cout << "Leaving HiveMind network...\n";
                    connected = false;
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

    return 0;
}
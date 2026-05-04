#include <iostream>
#include <string>
//#include "network.hpp"
#include "compute_check.hpp"
#include "global.hpp"

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
    metric=metric_calculation();
    string input;
    string networkName;
    string password;

    bool isConnected = false;
    bool running = true;

    //NetworkManager* NetManager = NetworkManager::getNetworkManager(running);

    cout << metric;
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
                cout << "create      - Create a HiveMind network\n";
                cout << "scan        - Search for HiveMind networks\n";
                cout << "join        - Join a network\n";
                cout << "status      - View current network status\n";
                cout << "leader      - Show current leader node\n";
                cout << "disconnect  - Leave the network\n";
                cout << "exit        - Close HiveMind\n\n";
                break;

            case CREATE:
                cout << "\nCreate your network\n";
                cout << "Enter the name of your network: ";
                cin >> networkName;

                cout << "Enter the password for your network: ";
                cin >> password;

                //NetManager->createNetwork(networkName, password);
                break;

            case SCAN:
                cout << "\nScanning local network...\n";
                cout << "Found networks:\n";

                // Later you can replace this with NetManager->getNetworkInfo()
                cout << "1. HiveMind-Lab | Password: Yes\n";
                cout << "2. HiveMind-Test | Password: No\n\n";
                break;

            case JOIN:
                cout << "Enter network name: ";
                cin >> networkName;

                cout << "Enter password: ";
                cin >> password;

                if (password == "1234") {
                    isConnected = true;
                    cout << "Connected to " << networkName << " successfully.\n\n";
                } else {
                    cout << "Error: Connection attempt failed.\n\n";
                }
                break;

            case STATUS:
                if (isConnected) {
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

            case GETLEADER:
                if (isConnected) {
                    cout << "Current Leader Node: 192.168.1.10\n\n";
                } else {
                    cout << "Error: Not connected to a HiveMind network.\n\n";
                }
                break;

            case DISCONNECT:
                if (isConnected) {
                    cout << "Leaving HiveMind network...\n";
                    isConnected = false;
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
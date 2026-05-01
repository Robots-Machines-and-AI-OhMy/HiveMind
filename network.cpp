/*
 * handles networking functions
 */

//#include <Lsquic.h> // Litespeed Quic library
#include <iostream>
#include <string>
#include <vector> // basically array lists
#include <chrono> // time keeper
#include <winsock2.h> // networking package
#include <thread> // for async operations
#include <mutex> // locking mechanisms

#include "network.hpp"
#include "containerization.hpp" // handles process containerization logic

using namespace std;

/*
class NetworkManager {

private:
    class Network {

    private:
        string name; //name of network
        string netUID; //UID of network
        string leadIP; //leader's IP address
        string password; //hash of password, may be null

    public:
        // network constructor, password may be NULL
        Network(string netName, string leader, string pass) {
            name = netName;
            password = pass;
            leadIP = leader;
        }

        string getName() { return name; }
        string getUID() { return netUID; }
        string getLeader() { return leadIP; }
        // password cannot get directly retrieved, for security reasons

        // accepts a password hash and compares it to this network
        // returns true if the hashes match
        bool validatePassword(string inputPassHash) {
            if (password == inputPassHash)
                return true;
            else
                return false;
        }
    }; // end Network class

    // NetworkManager attributes

    const bool test; // for testing mode

    status nodeState = None;
    double perfScore; // this device's performance score
    char* hostname = (char*)malloc(256 * sizeof(char));
    winsock2::gethostname(hostname, 256);

    Network currentNet = NULL; // this device's current network
    const int tickTime = 200; // time between heartbeats (in ms)
    const int heartbeatTimeout = 500; //time in ms to wait for heartbeat

    // holds NetInfo structs, used for reporting scan results to UI
    vector<struct NetInfo> netInfo = new vector<struct NetInfo>();

    WSADATA wsdata;

    // stops async operations
    // set when leaving network, reset when joining
    bool halting = false;

    NetworkManager* netmgr;
    static mutex mtx; // lock object

    // constructor, specifies testing mode
    NetworkManager(bool testing) {
        perfScore = calculateMetrics();
        test = testing;
        //startup winsock, this is mandatory so kill if error
        if (WSAStartup(MAKEWORD(2,2), &wsdata) != 0) {
            printf("Issue with WS startup: %d\n", WSAGetLastError());
            exit();
        }
        //Lsquic startup here
    }

    // internal async method, ran by leaders
    void listenForScan() {
        // IPv4 UDP socket
        SOCKET scanListener = new socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in listenSpec;
        listenSpec.sin_family = AF_INET;
        listenSpec.sin_addr.s_addr = INADDR_ANY;
        listenSpec.sin_port = htons(56713);

        while (!halting && nodeState == LEADER) {
            // wait here
        }
        // close UDP socket
        closesocket(scanListener);
    }

    // internal async method
    void sendHeartbeat() {
        while (!halting) {
            if (nodeState == LEADER) {
                //send heartbeat logic
            }
            else {
                //send metrics to leader
            }
        }
    }

public:

    // structure for network info, to be exposed to UI
    struct NetInfo {
        string name;
        string UID;
        string leadIP;
        bool password;
    };

    NetworkManager(const NetworkManager& obj) = delete; //delete copy constructor

    // creates a network with specified name and password
    // password may be null
    // returns true if network successfully created
    bool createNetwork(string name, string password) {
        currentNet = Network(name, hostname, password);

        //perform rest of setup logic here

        return true; //success
    }

    // disconnects this device from network
    // returns true when complete
    // returns false if an error was encountered
    bool leaveNetwork() {
        try {
            //perform disconnection logic
            halting = true;

            //wipe network
            Network currentNet = NULL;
            return true;
        }
        catch (Exception e) {
            return false;
        }
    }

    // attempts to join network of specified name and password
    // password may be null
    // returns true if success, false if there was an error
    bool joinNetwork(string name, string UID, string password) {
        return false; //replace with join logic later
    }

    //scans for networks by broadcasting UDP port 56713
    void scan() {
        // initialize UDP socket(s)

        // send broadcast message

        // collect responses, populate netInfo vector

    }

    // runs metric calculation algorithm
    double calculateMetrics() {
        return -1.0; //replace with algorithm
    }

    // fully dismantles network operations for proper shutdown
    // intended to be called when application terminating
    void cleanup() {
        // disconnect from current net if applicable
        bool discSuccess = false;
        if (currentNet != NULL) {
            discSuccess = leaveNetwork();
        }
        else {
            discSuccess = true;
        }

        // cleanup networks
        int success = winsock2::WSACleanup();

        if (test) {
            // print captured issues to terminal
            if (!discSuccess)
                cout << "issues encountered disconnecting from network" << endl;
            if (success != 0)
                cout << "issues encountered cleaning up winsock.dll" << endl;
        }
    }

    // used to initialize NetworkManager as a singleton
    // testing parameter used to put the network manager in testing mode
    // after initialization this cannot be changed.
    static NetworkManager* getNetworkManager(bool testing) {
        if (netmgr == nullptr) {
            lock_guard<mutex> lock(mtx);
            if (netmgr == nullptr) {
                netmgr = new NetworkManager(testing);
            }
        }
        return netmgr;
    }

    vector<struct NetInfo> getNetworkInfo() { return netInfo; }

};
*/

// network constructor, password may be NULL
NetworkManager::Network::Network(string netName, string leader, string pass) {
    name = netName;
    password = pass;
    leadIP = leader;
}

string NetworkManager::Network::getName() { return name; }
string NetworkManager::Network::getLeader() { return leadIP; }

// accepts a password hash and compares it to this network
// returns true if the hashes match
bool NetworkManager::Network::validatePassword(string inputPassHash) {
        if (password == inputPassHash)
            return true;
        else
            return false;
}

// constructor, specifies testing mode
NetworkManager::NetworkManager(bool testing) {
    perfScore = calculateMetrics();
    test = testing;

    nodeState = NONE;
    hostname = (char*)malloc(256 * sizeof(char));
    gethostname(hostname, 256);

    currentNet = new Network::Network("null", "none", "na"); //random inapplicable values

    netInfo = new vector<struct NetInfo>();
    halting = false;

    NetworkManager(const NetworkManager& obj) = delete; //delete copy constructor

    //startup winsock, this is mandatory so kill if error
    if (WSAStartup(MAKEWORD(2,2), &wsdata) != 0) {
        printf("Issue with WS startup: %d\n", WSAGetLastError());
        exit();
    }
    //Lsquic startup here
}

// internal async method, ran by leaders
void NetworkManager::listenForScan() {
    // IPv4 UDP socket
    SOCKET scanListener = new socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in listenSpec;
    listenSpec.sin_family = AF_INET;
    listenSpec.sin_addr.s_addr = INADDR_ANY;
    listenSpec.sin_port = htons(56713);

    while (!halting && nodeState == LEADER) {
        // wait here
    }
    // close UDP socket
    closesocket(scanListener);
}

// internal async method
void NetworkManager::sendHeartbeat() {
    while (!halting) {
        if (nodeState == LEADER) {
            //send heartbeat logic
        }
        else {
            //send metrics to leader
        }
    }
}


// creates a network with specified name and password
// password may be null
// returns true if network successfully created
bool NetworkManager::createNetwork(string name, string password) {
    currentNet = Network(name, hostname, password);

    //perform rest of setup logic here

    return true; //success
}

// disconnects this device from network
// returns true when complete
// returns false if an error was encountered
bool NetworkManager::leaveNetwork() {
    try {
        //perform disconnection logic
        halting = true;

        //wipe network
        Network currentNet = NULL;
        return true;
    }
    catch (...) {
        return false;
    }
}

// attempts to join network of specified name and password
// password may be null
// returns true if success, false if there was an error
bool NetworkManager::joinNetwork(string name, string UID, string password) {
    return false; //replace with join logic later
}

//scans for networks by broadcasting UDP port 56713
void NetworkManager::scan() {
    // initialize UDP socket(s)

    // send broadcast message

    // collect responses, populate netInfo vector

}

// runs metric calculation algorithm
double NetworkManager::calculateMetrics() {
    return -1.0; //replace with algorithm
}

// fully dismantles network operations for proper shutdown
// intended to be called when application terminating
void NetworkManager::cleanup() {
    // disconnect from current net if applicable
    bool discSuccess = false;
    if (currentNet != NULL) {
        discSuccess = leaveNetwork();
    }
    else {
        discSuccess = true;
    }

    // cleanup networks
    int success = WSACleanup();

    if (test) {
        // print captured issues to terminal
        if (!discSuccess)
            cout << "issues encountered disconnecting from network" << endl;
        if (success != 0)
            cout << "issues encountered cleaning up winsock.dll" << endl;
    }
}

// used to initialize NetworkManager as a singleton
// testing parameter used to put the network manager in testing mode
// after initialization this cannot be changed.
static NetworkManager* NetworkManager::getNetworkManager(bool testing) {
    if (netmgr == nullptr) {
        lock_guard<mutex> lock(mtx);
        if (netmgr == nullptr) {
            netmgr = new NetworkManager(testing);
        }
    }
    return netmgr;
}

vector<struct NetInfo> NetworkManager::getNetworkInfo() { return netInfo; }

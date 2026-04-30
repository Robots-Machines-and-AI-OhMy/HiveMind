/*
 * handles networking functions
 */

#include <Lsquic.h> // Litespeed Quic library
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

// node statuses
enum status {
    Leader,
    Follower,
    Candidate,
    None
};

class NetworkManager {

private:
    class Network {

    private:
        string name; //name of network
        string netUID; //UID of network
        string leadIP; //leader's IP address
        string password; //hash of password, may be null

    public:
        //network with a name, no password
        Network(string netName, string leader) {
            name = netName;
            leadIP = leader;
            password = NULL;
        }

        //network with a name and password
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

    // bind network names to IP addresses
    // network join relies on this being up to date, collected from broadcast
    vector routingTable;

    bool WSAinit = false; // if winsock.dll is initialized
    bool lsquicInit = false; //if lsquic is initialized

    // stops async operations
    // set when leaving network, reset when joining
    bool halting = false;

    NetworkManager* netmgr;
    static mutex mtx; // lock object

    // constructor, specifies testing mode
    NetworkManager(bool testing) {
        perfScore = calculateMetrics();
        test = testing;
    }

public:
    NetworkManager(const NetworkManager& obj) = delete; //delete copy constructor

    // creates a network with specified name and password
    // password may be null
    // returns true if network successfully created
    bool createNetwork(string name, string password) {
        if (password == NULL)
            currentNet = new Network(name, hostname);
        else
            currentNet = new Network(name, hostname, password);

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

    // internal async method
    void broadcastNetInfo() {
        // initialize UDP socket here
        while (!halting) {
            // broadcast info here
        }
    }

    // internal async method
    void sendHeartbeat() {
        while (!halting) {
            if (nodeState == Leader) {
                //send heartbeat logic
            }
            else {
                //send metrics to leader
            }
        }
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

};

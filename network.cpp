/*
 * handles networking functions
 */

#include <Lsquic.h> // Lightspeed Quic library
#include <iostream>
#include <string>
#include <vector> //basically array lists
#include <chrono> //time keeper
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
        const bool test; // for testing mode
        status state = None;
        double perfScore; // this device's performance score
        Network currentNet = NULL; //this device's current network

    public:
        // creates a network with specified name and password
        // password may be null
        // returns true if network successfully created
        bool createNetwork(string name, string password) {
            //
        }

        // runs metric calculation algorithm
        double calculateMetrics() {
            return -1.0; //replace with algorithm later
        }

        // constructor, specifies testing mode
        NetworkManager(bool testing) {
            perfScore = calculateMetrics();
            test = testing;
        }

        // default constructor, assumes testing mode false
        NetworkManager() {
            perfScore = calculateMetrics();
            test = false;
        }

};

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
        Network(string netName, string pass, string leader) {
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
        bool validatePassword(string inputPass) {
            if (password == inputPass)
                return true;
            else
                return false;
        }
};

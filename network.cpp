/*
 * handles networking functions
 */

#include <Lsquic.h> // Lightspeed Quic library
#include <iostream>
#include <string>
#include <vector> //basically array lists
#include <chrono> //time keeper
#include <winsock2.h> //networking package
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
        bool validatePassword(string inputPass) {
            if (password == inputPass)
                return true;
            else
                return false;
        }
};

class NetworkManager {

    private:
        const bool test; // for testing mode
        status state = None;
        double perfScore; // this device's performance score
        char* hostname = malloc(256 * sizeof(char));
        winsock2::gethostname(hostname, 256);
        Network currentNet = NULL; // this device's current network
        // bind network names to IP addresses
        // network join relies on this being up to date, collected from broadcast
        vector routingTable;

    public:
        // creates a network with specified name and password
        // password may be null
        // returns true if network successfully created
        bool createNetwork(string name, string password) {
            if (password == NULL)
                currentNet = new Network(name, hostname);
            else
                currentNet = new Network(name, hostname, password);

            return true; //success
        }

        // disconnects this device from network
        // returns true when complete
        // returns false if an error was encountered
        bool leaveNetwork() {
            try {
                //perform disconnection logic

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

        // runs metric calculation algorithm
        double calculateMetrics() {
            return -1.0; //replace with algorithm
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

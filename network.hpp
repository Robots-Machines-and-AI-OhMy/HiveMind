#ifndef NETWORK
#define NETWORK

#include <string>
#include <vector>
#include <mutex>
#include <winsock2.h>

// define method headers for network.cpp

using namespace std;

// node statuses
enum status {
    LEADER,
    FOLLOWER,
    CANDIDATE,
    NONE
};

// singleton class (only one instance can exist at a time)
// responsible for managing network connections between peers
class NetworkManager {
private:
    class Network {
    private:
        string name; //name of network
        string netUID; //UID of network
        string leadIP; //leader's IP address
        string password; //hash of password, may be null
    public:
        Network(string netName, string leader, string pass);

        string getName();
        string getUID();
        string getLeader();
        bool isPass();

		void setName(string newName);
		void setUID(string newUID);
		void setPassword(string newHash);

        bool validatePassword(string inputPassHash);
    };

    bool test; //testing mode

    status nodeState; //current state, see enum above
    double perfScore; //performance score
    hostent* hostname; //the device's hostname
	char* localIP; //the device's IP address

    Network currentNet; //current network device is member of, may be null
    const int tickTime = 200; //heartbeat timing interval in ms
    const int heartbeatTimeout = 500; //timeout for getting heartbeat in ms

    // holds NetInfo structs, used for reporting scan results to UI
    vector<struct NetInfo> netInfo;

    WSADATA wsdata;

    bool halting; //used to halt async ops
    NetworkManager* netmgr; // pointer to singleton obj
    static mutex mtx; // lock object
    NetworkManager(bool testing);

    void listenForScan(); //async leader method, responds to scan msgs
    void sendHeartbeat(); //async p2p member method, manages heartbeat logic
	void memberInit(); // launches threads for async member operations e.g. heartbeat

public:

    // structure for network info, to be exposed to UI
    struct P2PNetInfo {
        string name;
        string UID;
        string leadIP;
        bool password;
    };

    static NetworkManager* getNetworkManager(bool testing);

    double calculateMetrics();

    bool createNetwork(string name, string password);
    bool leaveNetwork();
    bool joinNetwork(string name, string UID, string passHash);
    void scan(); //scans for network (populates networkInfo vector)

    bool isConnected(); //returns true if currently connected to a network

    void cleanup();

    vector<struct NetInfo> getNetworkInfo();
};

#endif

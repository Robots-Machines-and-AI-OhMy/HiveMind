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
        uint8_t netUID[SHA256_DIGEST_SIZE]; //UID of network
        string leadIP; //leader's IP address
        uint8_t passHash[SHA256_DIGEST_SIZE]; //hash of password, may be empty string
		bool passValid; 
    public:
        Network(string netName, string leader, string pass);

        string getName();
        uint8_t* getUID();
        string getLeader();
        bool isPass();

		void setName(string newName);
		void setUID(); //generates new UID
		void setPassword(string newPassword); //hashes argument for password set

        bool validatePassword(string inputPassHash);
    };

    bool test; //testing mode

    status nodeState; //current state, see enum above
    struct SystemHealth dynPerfScore; //performance score
    hostent* hostname; //the device's hostname
	char* localIP; //the device's IP address

    Network currentNet; //current network device is member of, may be null
    const int tickTime = 200; //heartbeat timing interval in ms
    const int heartbeatTimeout = 500; //timeout for getting heartbeat in ms
	vector<struct SystemHealth> memberScores; //track member's performance scores

    // holds NetInfo structs, used for reporting scan results to UI
    vector<struct P2PNetInfo> netInfo;

    WSADATA wsdata;

    bool halting; //used to halt async ops
    NetworkManager* netmgr; // pointer to singleton obj
    static mutex mtx; // lock object
    NetworkManager(bool testing);
	
	vector<thread> asyncOps; //track launched threads
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

    vector<struct P2PNetInfo> getNetworkInfo();
};

#endif

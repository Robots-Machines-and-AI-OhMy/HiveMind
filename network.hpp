#ifndef NETWORK
#define NETWORK

// define method headers for network.cpp

// node statuses
enum status {
    Leader,
    Follower,
    Candidate,
    None
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
        Network(string netName, string leader);
        Network(string netName, string leader, string pass);

        string getName();
        string getUID();
        string getLeader();

        bool validatePassword(string inputPassHash);
    };

    const bool test; //testing mode

    status nodeState; //current state, see enum above
    double perfScore; //performance score
    char* hostname; //the device's hostname

    Network currentNet; //current network device is member of, may be null
    vector routingTable; //collecting broadcast data
    const int tickTime; //heartbeat timing interval

    bool WSAinit; //true if winsock.dll is initialized
    bool lsquicInit; //true if lsquic is initalized

    bool halting; //used to halt async ops
    NetworkManager* netmgr; // pointer to singleton obj
    static mutex mtx; // lock object
    NetworkManager(bool testing);

public:

    NetworkManager* getNetworkManager(bool testing);

    bool createNetwork(string name, string password);
    bool leaveNetwork();
    bool joinNetwork(string name, string UID, string password);

    void cleanup();
};

#endif

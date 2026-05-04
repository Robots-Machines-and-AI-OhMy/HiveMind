/*
 * handles networking functions
 */

#include "msquic\src\inc\msquic.hpp" //QUIC library
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
            exit(0);
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
NetworkManager::Network::Network(string netName, string leader, string passHash) {
    name = netName;
	//UID init
    password = passHash;
    leadIP = leader;
}

string NetworkManager::Network::getName() { return name; }
//string NetworkManager::Network::getUID() { return UID; }
string NetworkManager::Network::getLeader() { return leadIP; }
bool NetworkManager::Network::isPass() {
	if (password == "")
		return false; 
	else
		return true;
}

// accepts a password hash and compares it to this network
// returns true if the hashes match
bool NetworkManager::Network::validatePassword(string inputPassHash) {
        if (password == inputPassHash)
            return true;
        else
            return false;
}

// constructor, specifies testing mode
NetworkManager::NetworkManager(bool testing) : currentNet("null", "none", "na"){
    perfScore = calculateMetrics();
    test = testing;

    nodeState = NONE;
    hostname = gethostbyname(""); 
	localIP = inet_ntoa(*(struct in_addr *)*localHost->h_addr_list); // get this device's IP
    //currentNet = new this->Network::Network("null", "none", "na"); //random inapplicable values

	/*vector<NetInfo> networks;

	networks = vector<NetInfo>();*/
    netInfo = new vector<struct NetInfo>();
    halting = false;

    NetworkManager(const NetworkManager& obj) = delete; //delete copy constructor

    //startup winsock, this is mandatory so kill if error
    if (WSAStartup(MAKEWORD(2,2), &wsdata) != 0) {
        wprintf(L"Issue with WS startup: %d\n", WSAGetLastError());
        exit(0);
    }
    //Lsquic startup here
}

// internal async method, ran by leaders
// listens for UDP messages on port 56713; responds with their network info
// format:  <network-name>|<network-UID>|<leader-ip>|<passFlag>
// passFlag is either "t" or "f"
// pipe "|" is used as delimiter
void NetworkManager::listenForScan() {
    // IPv4 UDP socket
    SOCKET scanListener = new socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (scanListener == INVALID_SOCKET) {
		wprinf(L"socket failed with error: %ld\n", WSAGetLastError());
		WSACleanup();
		exit(0);
	}
    struct sockaddr_in listenSpec;
    listenSpec.sin_family = AF_INET;
    listenSpec.sin_addr.s_addr = INADDR_ANY; //accept any IP
    listenSpec.sin_port = htons(56713); //listen on port 56713
	
	int reqbuflen = 32; // length of recv buffer
	char reqbuf[reqbuflen]; // holds request message
	sockaddr requestAddr; // holds request source address
	int recvResult; // observing how successful receive is
	
	int sendResult; // observing how successful send operation is
	int sendbuflen = 128; // length of send buffer
	char sendbuf[sendbuflen]; // holds message to send
	
	//network info format: <network-name>|<network-UID>|<leader-IP>|<passFlag>
	//network-name: name of the network
	//network-UID: UID of the network
	//passFlag: whether the network has a password, either "t" for yes or "f" for no
	//pipe character "|" used as a delimiter; it follows that the delimiter cannot be a part of delimited members
	string networkInfo = currentNet.getName() + "|" + currentNet.getUID() + "|" + currentNet.getLeader() + "|";
	if (currentNet.isPass())
		networkInfo += "t";
	else
		networkInfo += "f";
	
	strcpy(sendbuf, networkInfo.c_str());
	
	if ((int bindCode = bind(scanListener, listenSpec&, sizeof(listenSpec))) != 0) {
		printf("Error binding scan listener socket: %d", bindCode);
		exit(0); // there may be smarter alternatives to this
	}

    while (!halting && nodeState == LEADER) {
        // thread will spin around in here, perform one response per iteration
		
		// receive packet (broadcast from scan function)
		// note: recvfrom is a blocking method
		recvResult = recvfrom(scanListener, reqbuf&, reqbuflen, 0, requestAddr&, sizeof(requestAddr));
		
		// send response 
		sendResult = sendto(scanListener, sendbuf&, sendbuflen, 0, requestAddr&, sizeof(requestAddr)); 
		
    }
    // close UDP socket
    closesocket(scanListener);
}

// internal async method
// leaders will send heartbeat to their followers
// followers will send metrics to the leader
// performed over QUIC
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

// launches threads for async member methods
// does not set node state
// CANDIDATE state only exists when the async methods are functioning
void NetworkManager::memberInit() {
	switch (nodeState) {
		case LEADER:
			// launch scan listener
		case FOLLOWER: 
			// launch heartbeat
		default:
			// does nothing
			break;
	}
}


// creates a network with specified name and password
// password may be null
// returns true if network successfully created
bool NetworkManager::createNetwork(string name, string passHash) {
    currentNet.setName(name);
	currentNet.setUID(); // generate UID
	currentNet.setPassword(passHash);
	
	nodeState = LEADER; // creating node is the leader of network by default
	memberInit(); // initialize async methods

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
    	currentNet.setName("");
    	currentNet.setUID(""); // generate UID
    	currentNet.setPassword("");
        return true;
    }
    catch (...) {
        return false;
    }
}

// attempts to join network of specified name and password
// password may be null
// returns true if success, false if there was an error
bool NetworkManager::joinNetwork(string name, string UID, string passHash) {
    return false; //replace with join logic later
}

//scans for networks by broadcasting UDP port 56713
void NetworkManager::scan() {
    // initialize UDP socket
	SOCKET scanner = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	
	if (scanner == INVALID_SOCKET) {
		wprintf(L"socket failed with error: %ld\n", WSAGetLastError());
		WSACleanup();
		exit(0);
	}
    // specify socket
	BOOL bOpt = TRUE;
	int bOptLen = sizeof (BOOL);
	
	// set socket to broadcast
	int broadcastSetCode = setsockopt(scanner, SOL_SOCKET, SO_BROADCAST, (char*) &bOpt, bOptLen);
	if (broadcastSetCode != 0) {
		wprintf(L"setsockopt for SO_BROADCAST failed with error %u\n", WSAGetLastError());
		WSACleanup();
		exit(0);
	}
	
	// send broadcast message
	struct sockaddr_in scanSpec; 
	scanSpec.sin_family = AF_INET;
	scanSpec.sin_port = htons(56713);
	scanSpec.sin_addr.s_addr = inet_addr("255.255.255.255"); // broadcast address
	int sendResult = sendto(scanner, "", 0, 0, scanSpec&, sizeof(scanSpec)); // send empty datagram
	if (sendResult == SOCKET_ERROR) {
		wprintf(L"send error: %d\n", WSAGetLastError());
	}
	
	int recBufLen = 128; 
	char* recBuf[recBufLen];
	
	this_thread::sleep_for(chrono::seconds(1));  ; //brief pause to get network responses


    // collect responses, populate netInfo vector
	int bytesReceived;
	struct NetInfo receivedNet;
	// this loop technically cannot break since recv is blocking
	// implement timing mechanism here to force the loop to exit after specified time
	int maxTimeMS = 2000; // time to pick up networks
	netInfo = new vector<struct NetInfo>(); // wipe old network list; avoids stale data
	while ((bytesReceived = recv(scanner, recBuf, recBufLen, 0)) != 0) {
		netInfo.push_back(new struct NetInfo);
		//initialize
		receivedNet = netInfo.get(netInfo.length - 1);
		receivedNet.name = "";
		receivedNet.UID = "";
		receivedNet.leadIP = "";
		int i = 0; // buffer iterator
		//name
		while (recBuf[i] != '|' && recBuf[i] != '\0') {
			receivedNet.name += recBuf[i];
		}
		i++; //skip pipe char
		//UID
		while (recBuf[i] != '|' && recBuf[i] != '\0') {
			receivedNet.UID += recBuf[i];
		}
		i++; //skip pipe char
		//Leader IP
		while (recBuf[i] != '|' && recBuf[i] != '\0') {
			receivedNet.leadIP += recBuf[i];
		}
		i++;
		//pass flag
		if (recBuf[i] == 't')
			receivedNet.passFlag = true;
		else
			receivedNet.passFlag = false;
		
		// check time; make sure maxTimeMS has not fully elapsed
	}

}

// runs dynamic metric calculation algorithm
// metrics sent to leader during heartbeat
double NetworkManager::calculateMetrics() {
    return -1.0; //replace with algorithm
}

// fully dismantles network operations for proper shutdown
// intended to be called when application terminating
void NetworkManager::cleanup() {
    // disconnect from current net if applicable
    bool discSuccess = false;
    if (currentNet.getName()=="") {
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
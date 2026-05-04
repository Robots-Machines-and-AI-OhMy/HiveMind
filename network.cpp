/*
 * handles networking functions
 */

#include "msquic\src\inc\msquic.hpp" //QUIC library
#include "tiny-sha\src\tiny_sha.h" //SHA library
#include <iostream>
#include <string>
#include <vector> // basically array lists
#include <chrono> // timekeeper
#include <winsock2.h> // networking package
#include <thread> // for async operations
#include <mutex> // locking mechanisms
#include <unordered_map> // hashmaps

#include "global.hpp"
#include "network.hpp"
#include "containerization.hpp" // handles process containerization logic
#include "Calculate_Performance.hpp" //dynamic performance metric calculation

using namespace std;

// network constructor, password may be NULL
NetworkManager::Network::Network(string netName, string leader, string password) {
    name = netName;
	long long currentTime = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now()
        ).time_since_epoch().count();
	if (!(SHA256((const uint8_t*)currentTime, sizeof(currentTime), &netUID)))
		cout << "Hash of current time (for UID) failed" << endl;
	
    if (password == "")
		passValid = false;
	else {
		if (!(SHA256((const uint8_t*)(c_str(password)), password.length, &passHash)))
			cout << "Hash of new network's password failed!" << endl; 
		passValid = true; 
	}
    leadIP = leader;
}

string NetworkManager::Network::getName() { return name; }
uint8_t NetworkManager::Network::getUID() { return UID; }
string NetworkManager::Network::getLeader() { return leadIP; }

void NetworkManager::Network::setName(string newName) { name = newName; }
void NetworkManager::Network::setLeader(string leader) { leadIP = leader; }
void NetworkManager::Network::setUID() {
	long long currentTime = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now()
        ).time_since_epoch().count();
	if (!(SHA256((const uint8_t*)currentTime, sizeof(currentTime), &netUID)))
		cout << "Hash of current time (for UID) failed" << endl;
}
void NetworkManager::Network::setPassword(string newPassword) {
	if (password == "")
		passValid = false;
	else {
		if (!(SHA256((const uint8_t*)(c_str(password)), password.length, &passHash)))
			cout << "Hash of new network's password failed!" << endl; 
		passValid = true; 
	}
}


bool NetworkManager::Network::isPass() {
	if (!passValid)
		return false;
	else
		return true;
}

// accepts a password hash and compares it to this network
// returns true if the hashes match
bool NetworkManager::Network::validatePassword(uint8_t* inputPassHash) {
	int cmp = SHA256CompareOrder(inputPassHash, passHash);
	if (cmp == 0)
		return true; 
	else 
		return false;
}

// constructor, specifies testing mode
NetworkManager::NetworkManager(bool testing) : currentNet("", "", "") {
	
    test = testing;

    nodeState = NONE;
    hostname = gethostbyname("");
	localIP = inet_ntoa(*(struct in_addr *)*localHost->h_addr_list); // get this device's IP
    netInfo = new vector<struct P2PNetInfo>();
    halting = false;

    NetworkManager(const NetworkManager& obj) = delete; //delete copy constructor

    //startup winsock, this is mandatory so kill if error
    if (WSAStartup(MAKEWORD(2,2), &wsdata) != 0) {
        wprintf(L"Issue with WS startup: %d\n", WSAGetLastError());
        exit(0);
    }
	//startup msquic
	QUIC_STATUS quicStatus = MsQuicOpen2(); //use msquic version 2
	if (quicStatus == QUIC_FAILED) {
		cout << "msquic failed to start" << endl;
		exit(0);
	}
	
}

// internal async method, ran by leaders
// listens for UDP messages on port 56713; responds with their network info
// format:  <network-name>|<network-UID>|<leader-ip>|<passFlag>
// passFlag is either "t" or "f"
// pipe "|" is used as delimiter
void NetworkManager::listenForScan() {
    // IPv4 UDP socket
    SOCKET scanListener = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (scanListener == INVALID_SOCKET) {
		wprintf(L"socket failed with error: %ld\n", WSAGetLastError());
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

	int bindCode = bind(scanListener, (struct sockaddr*)&listenSpec, sizeof(listenSpec));

	if (bindCode == SOCKET_ERROR) {
		printf("Error binding scan listener socket: %d\n", WSAGetLastError());
		exit(1);
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
			thread scan(listenForScan);
			asyncOps.push_back(scan);
		case FOLLOWER:
			// launch heartbeat
			thread beatSend(sendHeartbeat);
			asyncOps.push_back(beatSend);
		default:
			// does nothing
			break;
	}
}


// creates a network with specified name and password
// password may be null
// returns true if network successfully created
bool NetworkManager::createNetwork(string name, string password) {
	
	currentNet.setName(name);
	currentNet.setUID();
	currentNet.setPassword(password);
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
bool NetworkManager::joinNetwork(string name, string UID, string password) {
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
	struct P2PNetInfo receivedNet;
	// this loop technically cannot break since recv is blocking
	// implement timing mechanism here to force the loop to exit after specified time
	int maxTimeMS = 2000; // time to pick up networks
	netInfo = new vector<struct P2PNetInfo>(); // wipe old network list; avoids stale data
	while ((bytesReceived = recv(scanner, recBuf, recBufLen, 0)) != 0) {
		netInfo.push_back(new struct P2PNetInfo);
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
struct SystemHealth NetworkManager::calculateMetrics() {
    return getSystemHealth();
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
	MsQuicClose();

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

vector<struct P2PNetInfo> NetworkManager::getNetworkInfo() { return netInfo; }

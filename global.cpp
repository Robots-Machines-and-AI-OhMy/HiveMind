// global.cpp
#include <string>
#include "global.hpp"

double metric               = 0.0;
int    LOCAL_RAFT_NODE_ID   = 1;
double NODE_CAPACITY_WEIGHT = 1.0;

// C string storage (updated alongside std::string versions by NetworkManager)
static char s_raft_ep[256] = "";
static char s_ip[64]       = "";
const char* LOCAL_RAFT_ENDPOINT_C = s_raft_ep;
const char* LOCAL_NODE_IP_C       = s_ip;

// std::string versions — used by NetworkManager and RaftDistribution
// Declared here so they have a single definition; included via global_strings.hpp
std::string LOCAL_RAFT_ENDPOINT;
std::string LOCAL_NODE_IP;
#pragma once
// global_strings.hpp
// Include ONLY after <string> and Windows headers are already included.
// Provides std::string versions of the global IP/endpoint strings.
// For use in .cpp files; never include from a header.
#include <string>
#include "global.hpp"
extern std::string LOCAL_RAFT_ENDPOINT;
extern std::string LOCAL_NODE_IP;
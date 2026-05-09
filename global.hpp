#pragma once
// global.hpp
// NO #include directives of any kind.
// Safe to include before any Windows, MSQuic, or NuRaft header.
// Definitions in global.cpp.

// Benchmark metric (set at startup by metric_calculation())
extern double metric;

// Node identity (set before RaftDistribution is constructed)
extern int LOCAL_RAFT_NODE_ID;

// Capacity weight derived from benchmark
extern double NODE_CAPACITY_WEIGHT;

// IP strings as C strings (no std::string dependency)
extern const char* LOCAL_RAFT_ENDPOINT_C;
extern const char* LOCAL_NODE_IP_C;

// Port constants as plain macros (no type, no header needed)
#define RAFT_PORT       19875
#define QUIC_DATA_PORT  19876
#define SCAN_UDP_PORT   56713
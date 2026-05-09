#pragma once
// Raft_Engine.hpp
// ─────────────────────────────────────────────────────────────────────────────
// Public interface for RaftDistribution.
//
// IMPORTANT: This header intentionally does NOT include <libnuraft/nuraft.hxx>.
// All NuRaft types are hidden behind a pImpl pointer in the .cpp so that
// Network.cpp, HiveMindCLI.cpp, and Offload_Hook.cpp can include this header
// without needing NuRaft on their include path.
//
// Only Raft_Engine.cpp includes <libnuraft/nuraft.hxx>.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "global.hpp"   // LOCAL_RAFT_NODE_ID, LOCAL_NODE_IP, metric

// Forward declarations — no NuRaft header needed here.
class QuicTransport;
namespace nuraft_detail { struct RaftImpl; }  // opaque pImpl

namespace dist {

// ─────────────────────────────────────────────────────────────────────────────
// Threshold constant
// ─────────────────────────────────────────────────────────────────────────────
static constexpr double OFFLOAD_THRESHOLD_PCT = 0.10;  // 10%

// ─────────────────────────────────────────────────────────────────────────────
// Wire types — plain structs, no NuRaft dependency
// ─────────────────────────────────────────────────────────────────────────────
struct OffloadRequest {
    int         requester_node_id = -1;
    std::string process_id;
};

struct OffloadResponse {
    std::string target_ip;   // empty = run locally
    bool        success = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// NodeInfo
// Per-peer state maintained by every cluster member.
// Keyed by node_id in the nodes_ table inside RaftDistribution.
// ─────────────────────────────────────────────────────────────────────────────
struct NodeInfo {
    int         id               = -1;
    std::string ip;                      // plain IP returned to requesters
    double      benchmark_metric  = 0.0; // static compute_check score (startup)
    double      heartbeat_score   = 0.0; // live: benchmark_metric * remaining_perf

    // Placement score — higher is better.
    double score() const { return heartbeat_score; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RaftDistribution
// ─────────────────────────────────────────────────────────────────────────────
class RaftDistribution {
public:
    // peer_endpoints: { node_id -> "ip:port" } for Raft RPC (all nodes incl. self)
    // peer_ips:       { node_id -> "plain_ip" } returned to requesters
    RaftDistribution(
        QuicTransport&                              quic,
        const std::unordered_map<int,std::string>&  peer_endpoints,
        const std::unordered_map<int,std::string>&  peer_ips
    );

    ~RaftDistribution();

    // Non-copyable.
    RaftDistribution(const RaftDistribution&)            = delete;
    RaftDistribution& operator=(const RaftDistribution&) = delete;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void start();
    void stop();

    // ── Heartbeat API ─────────────────────────────────────────────────────────

    // Called by NetworkManager::wireHeartbeatReceiver() on every incoming packet.
    // Updates the peer's NodeInfo with both live score and static benchmark.
    void on_heartbeat_received(int     from_node_id,
                               double  heartbeat_score,
                               double  benchmark_metric,
                               int64_t seq);

    // Update our own heartbeat_score entry (called every 200 ms).
    void update_local_metric(double heartbeat_score);

    // Read current local score.
    double local_metric() const { return local_metric_.load(); }

    // ── Offload API ───────────────────────────────────────────────────────────

    // Blocks until the leader commits a placement decision, or timeout expires.
    // Returns target node IP, or "" if the process should run locally.
    std::string request_offload(
        const std::string&      process_id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(180));
    //  ^^ 180 ms — inside hook.c's 200 ms hard timeout with margin

    // ── Introspection ─────────────────────────────────────────────────────────
    bool is_leader() const;
    int  leader_id() const;
    int  my_node_id() const;  // returns LOCAL_RAFT_NODE_ID; defined in .cpp

    std::unordered_map<int, NodeInfo> node_snapshot() const;

private:
    // ── Placement (leader-only) ───────────────────────────────────────────────
    // Returns the IP of the best candidate that beats requestor_score by >10%,
    // or "" if no such candidate exists (process should run locally).
    std::string elect_target(double requestor_score) const;

    // ── pImpl — hides all NuRaft types from this header ───────────────────────
    std::unique_ptr<nuraft_detail::RaftImpl> impl_;

    // ── Peer state (no NuRaft types) ─────────────────────────────────────────
    mutable std::mutex                        nodes_mtx_;
    std::unordered_map<int, NodeInfo>         nodes_;

    std::atomic<double>                       local_metric_{0.0};

    QuicTransport&                            quic_;
    std::unordered_map<int,std::string>       peer_endpoints_;

    bool running_ = false;
};

} // namespace dist
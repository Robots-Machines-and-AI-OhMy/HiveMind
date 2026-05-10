#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>
#include <optional>
#include <cstdint>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
class QuicTransport;   // Provided by your QUIC layer – see notes in .cpp

// ─────────────────────────────────────────────────────────────────────────────
// global.hpp contract
//   Defines:  double NODE_CAPACITY_WEIGHT   (per-machine compile-time constant)
//             int    LOCAL_RAFT_NODE_ID      (unique integer id for this node)
//             std::string LOCAL_RAFT_ENDPOINT  ("ip:port" for Raft RPC)
// ─────────────────────────────────────────────────────────────────────────────
#include "global.hpp"

namespace dist {

// ─────────────────────────────────────────────────────────────────────────────
// HeartbeatMetric
//   A [0, 1] load metric piggybacked on the Raft heartbeat stream.
//   0.0 = fully loaded / unavailable, 1.0 = completely idle / best candidate.
// ─────────────────────────────────────────────────────────────────────────────
struct HeartbeatMetric {
    int    node_id  = -1;
    double metric   = 1.0;   // [0, 1]
    int64_t seq     = 0;     // monotonic sequence for staleness detection
};

// ─────────────────────────────────────────────────────────────────────────────
// OffloadRequest  –  sent by any member to the leader via Raft log entry
// OffloadResponse –  returned by the leader (committed log reply)
// ─────────────────────────────────────────────────────────────────────────────
struct OffloadRequest {
    int         requester_node_id = -1;
    std::string process_id;          // opaque identifier for the process
};

struct OffloadResponse {
    std::string target_ip;           // empty = leader could not place the process
    bool        success = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// NodeInfo  –  per-peer state maintained by the leader (and kept locally
//              on followers so they can score themselves)
// ─────────────────────────────────────────────────────────────────────────────
struct NodeInfo {
    int         id                = -1;
    std::string ip;
    double      benchmark_metric  = 0.0;  // raw compute_check score
    double      heartbeat_score   = 0.0;  // metric * health composite

    // Higher score = better offload target.
    double score() const { return heartbeat_score; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RaftDistribution  –  main facade
//
//  Lifecycle:
//    1. Construct with the QUIC transport reference and initial peer list.
//    2. Call start() once.
//    3. Any node may call request_offload(process_id) at any time.
//    4. The leader handles placement internally and returns the target IP.
//    5. Call stop() before destruction.
// ─────────────────────────────────────────────────────────────────────────────
class RaftDistribution {
public:
    // peer_endpoints: map of { node_id -> "ip:port" } for ALL nodes (incl. self)
    // peer_ips:       map of { node_id -> "plain_ip" } returned to requesters
    RaftDistribution(
        QuicTransport&                             quic,
        const std::unordered_map<int,std::string>& peer_endpoints,
        const std::unordered_map<int,std::string>& peer_ips
    );

    ~RaftDistribution();

    // Start Raft server and join cluster.
    void start();

    // Gracefully leave and stop threads.
    void stop();

    // ── Heartbeat API ────────────────────────────────────────────────────────

    // Called by your QUIC layer whenever a heartbeat+metric arrives from a peer.
    // Thread-safe.
    void on_heartbeat_received(int from_node_id, double heartbeat_score,
                               double benchmark_metric, int64_t seq);

    // Called periodically by your QUIC layer to piggyback the local metric.
    // Returns the current local [0,1] load metric to embed in the next heartbeat.
    double local_metric() const;

    // Update our own load metric (e.g. from a scheduler or monitoring thread).
    // Thread-safe.
    void update_local_metric(double metric);

    // Leader calls this when a JOIN is accepted to register the new peer.
    void add_peer(int node_id,
                  const std::string& endpoint,
                  const std::string& ip,
                  double benchmark_metric,
                  double capacity_weight);

    // ── Offload API ──────────────────────────────────────────────────────────

    // Any node calls this.  Blocks until the leader commits the decision.
    // Returns the IP of the node that should execute process_id, or "" on error.
    std::string request_offload(const std::string& process_id,
                                std::chrono::milliseconds timeout =
                                    std::chrono::milliseconds(5000));

    // ── Introspection ────────────────────────────────────────────────────────

    bool is_leader() const;
    int  leader_id() const;
    int  my_node_id() const { return LOCAL_RAFT_NODE_ID; }

    // Snapshot of current scores for all known nodes (leader-only meaningful).
    std::unordered_map<int, NodeInfo> node_snapshot() const;

private:
    // ── Internal helpers ─────────────────────────────────────────────────────
    std::string elect_target(double requestor_score) const;

    // ── NuRaft types hidden behind pImpl so this header needs no NuRaft include
    struct RaftImpl;
    std::unique_ptr<RaftImpl> impl_;

    // ── State ────────────────────────────────────────────────────────────────
    mutable std::mutex                         nodes_mtx_;
    std::unordered_map<int, NodeInfo>          nodes_;      // keyed by node_id

    std::atomic<double>                        local_metric_{1.0};

    QuicTransport&                             quic_;
std::unordered_map<int,std::string>  peer_endpoints_;

    bool running_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Log entry type tags  (encoded in the first byte of every nuraft log buffer)
// ─────────────────────────────────────────────────────────────────────────────
enum class EntryType : uint8_t {
    OFFLOAD_REQUEST  = 0x01,
    OFFLOAD_RESPONSE = 0x02,
};

} // namespace dist
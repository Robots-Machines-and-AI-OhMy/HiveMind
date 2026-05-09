#pragma once
// Network.hpp
// ─────────────────────────────────────────────────────────────────────────────
// MindMesh network layer.
//
// Responsibilities:
//   - MSQuic connection and stream management (QuicTransport).
//   - UDP broadcast scan for peer discovery (port SCAN_UDP_PORT).
//   - Network creation / join / disconnect lifecycle.
//   - Heartbeat transmission (200 ms interval) piggybacking the
//     remaining-performance metric from Calculate_Performance.hpp.
//   - Wires into RaftDistribution for leader election and offload decisions.
//
// Thread safety:
//   All public methods on NetworkManager are thread-safe unless noted.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>

// MSQuic (pre-installed, headers on include path)
#include <msquic.h>

#include "global.hpp"

// Forward-declare so Network.hpp has no hard dependency on NuRaft headers.
namespace dist { class RaftDistribution; }

// ─────────────────────────────────────────────────────────────────────────────
// ScanResult — one entry returned by NetworkManager::scan()
// ─────────────────────────────────────────────────────────────────────────────
struct ScanResult {
    std::string name;         // network name broadcast by the creator
    std::string leader_ip;    // IP of the node that responded
    bool        has_password; // true if a password is required to join
};

// ─────────────────────────────────────────────────────────────────────────────
// QuicTransport
//
// Thin wrapper around the MSQuic API.  Exposes the interface expected by
// RaftDistribution and TransferEngine:
//
//   bool send(endpoint, data, len)        — send bytes to "ip:port"
//   void set_recv_callback(cb)            — called on every incoming datagram
//
// Also used internally by NetworkManager for heartbeat streams.
// ─────────────────────────────────────────────────────────────────────────────
class QuicTransport {
public:
    using RecvCallback = std::function<
        void(const std::string& from_endpoint,
             const uint8_t*    data,
             size_t            len)>;

    QuicTransport();
    ~QuicTransport();

    // Initialise MSQuic and start the listener on QUIC_DATA_PORT.
    // Must be called before send() or set_recv_callback().
    bool init(const std::string& local_ip);

    // Shut down MSQuic cleanly.
    void shutdown();

    // Send raw bytes to a peer.  Returns false on failure.
    bool send(const std::string& endpoint,
              const uint8_t*    data,
              size_t            len);

    // Register callback invoked on every incoming message (any stream).
    void set_recv_callback(RecvCallback cb);

    // True after init() succeeds.
    bool is_ready() const { return ready_; }

private:
    // MSQuic handles
    const QUIC_API_TABLE* api_       = nullptr;
    HQUIC                 reg_       = nullptr;  // QUIC registration
    HQUIC                 config_    = nullptr;  // connection config
    HQUIC                 listener_  = nullptr;

    std::atomic<bool>     ready_     { false };
    RecvCallback          recv_cb_;
    mutable std::mutex    cb_mtx_;

    // Static MSQuic callbacks (must match QUIC_LISTENER_CALLBACK etc.)
    static QUIC_STATUS QUIC_API listener_cb(HQUIC listener,
                                            void* ctx,
                                            QUIC_LISTENER_EVENT* ev);
    static QUIC_STATUS QUIC_API conn_cb(HQUIC conn,
                                        void* ctx,
                                        QUIC_CONNECTION_EVENT* ev);
    static QUIC_STATUS QUIC_API stream_cb(HQUIC stream,
                                          void* ctx,
                                          QUIC_STREAM_EVENT* ev);
};

// ─────────────────────────────────────────────────────────────────────────────
// NetworkManager — singleton
//
// Lifecycle:
//   1. getInstance() / init() on startup after metric_calculation().
//   2. createNetwork() or joinNetwork() for a session.
//   3. Heartbeat loop starts automatically after join/create.
//   4. disconnect() / cleanup() on exit.
// ─────────────────────────────────────────────────────────────────────────────
class NetworkManager {
public:
    // Singleton access.
    static NetworkManager& getInstance();

    // Deleted copy/move.
    NetworkManager(const NetworkManager&)            = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    // Initialise transport and resolve local IP.  Call once before anything else.
    bool init();

    // ── Network operations (map 1:1 to CLI commands) ─────────────────────────

    // CREATE — become leader of a new network.
    // name: display name  password: "" for open network.
    bool createNetwork(const std::string& name,
                       const std::string& password);

    // SCAN — UDP broadcast, collect responses for ~2 s.
    std::vector<ScanResult> scan();

    // JOIN — connect to an existing network.
    bool joinNetwork(const std::string& leader_ip,
                     const std::string& network_name,
                     const std::string& password);

    // STATUS — human-readable string for the CLI.
    std::string statusString() const;

    // LEADER — returns the current leader's IP, or "" if unknown.
    std::string leaderIp() const;

    // DISCONNECT — leave the cluster gracefully.
    bool disconnect();

    // CLEANUP — called on EXIT; tears down threads and MSQuic.
    void cleanup();

    // ── Offload integration ───────────────────────────────────────────────────

    // Called by AskEngine (Offload_Hook.cpp) when a process is intercepted.
    // Returns the target node IP chosen by the Raft leader, or "" for local run.
    std::string requestOffload(const std::string& process_id);

    // ── Accessors ─────────────────────────────────────────────────────────────

    bool isConnected()  const;
    bool isLeader()     const;
    QuicTransport& transport() { return *quic_; }

private:
    NetworkManager() = default;
    ~NetworkManager() = default;

    // ── Heartbeat ─────────────────────────────────────────────────────────────
    void heartbeatLoop();          // runs on heartbeat_thread_
    void wireHeartbeatReceiver();  // routes incoming QUIC packets into raft_

    // ── Scan responder ────────────────────────────────────────────────────────
    void scanListenerLoop();  // runs on scan_listener_thread_ (leader only)

    // ── Internal state ────────────────────────────────────────────────────────
    std::unique_ptr<QuicTransport>        quic_;
    std::shared_ptr<dist::RaftDistribution> raft_;

    std::string network_name_;
    std::string network_password_;
    std::string local_ip_;

    std::atomic<bool> connected_    { false };
    std::atomic<bool> stopping_     { false };

    mutable std::mutex state_mtx_;

    std::thread heartbeat_thread_;
    std::thread scan_listener_thread_;

    // Node-id counter (leader assigns IDs; followers receive theirs on join).
    int next_node_id_ = 2;   // leader always takes ID 1
};
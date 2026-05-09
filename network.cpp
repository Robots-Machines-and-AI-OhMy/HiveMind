// Network.cpp
// ─────────────────────────────────────────────────────────────────────────────
// MindMesh network layer implementation.
//
// MSQuic is used for all cluster data transport (heartbeats, offload requests,
// process bundles).  UDP broadcast on SCAN_UDP_PORT is used for peer discovery
// only — it carries no sensitive data and does not participate in the QUIC
// security model.
// ─────────────────────────────────────────────────────────────────────────────

#include "Network.hpp"
#include "Raft_Engine.hpp"
#include "Calculate_Performance.hpp"
#include "global.hpp"

#include <iostream>
#include <sstream>
#include <cstring>
#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>

// Windows sockets for UDP scan (MSQuic handles the QUIC side).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// ─────────────────────────────────────────────────────────────────────────────
// Wire formats for the UDP scan protocol (plain ASCII, null-terminated)
//
//  Broadcast: "MINDMESH_SCAN\0"
//  Reply:     "MINDMESH_HERE name=<name> pw=<0|1>\0"
//
// Heartbeat payload (sent over a QUIC stream, binary):
//   [8 bytes double] remaining performance metric (Calculate_Performance)
//   [8 bytes double] benchmark metric             (global::metric)
//   [4 bytes int32]  sender node id
// ─────────────────────────────────────────────────────────────────────────────

static constexpr char SCAN_PROBE[]  = "MINDMESH_SCAN";
static constexpr char SCAN_REPLY[]  = "MINDMESH_HERE";
static constexpr int  SCAN_WAIT_MS  = 2000;

#pragma pack(push,1)
struct HeartbeatPacket {
    // Combined score = metric * getSystemHealth() product.
    // Each member stores the latest value for every peer so the
    // leader has a full cluster overview for placement decisions.
    double  heartbeat_score;   // metric * (0.7*cpuHealth + 0.3*ramHealth)
    double  benchmark_metric;  // raw compute_check result (static per session)
    int32_t node_id;
};
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
// §1  QuicTransport
// ─────────────────────────────────────────────────────────────────────────────

// ALPN for MindMesh QUIC connections.
static const QUIC_BUFFER MINDMESH_ALPN = {
    sizeof("mindmesh") - 1,
    (uint8_t*)"mindmesh"
};

QuicTransport::QuicTransport() = default;

QuicTransport::~QuicTransport()
{
    shutdown();
}

bool QuicTransport::init(const std::string& local_ip)
{
    (void)local_ip; // listener binds to INADDR_ANY

    // Open MSQuic.
    if (QUIC_FAILED(MsQuicOpen2(&api_))) {
        std::cerr << "[QuicTransport] MsQuicOpen2 failed\n";
        return false;
    }

    // Registration.
    QUIC_REGISTRATION_CONFIG reg_cfg = {};
    reg_cfg.AppName    = "MindMesh";
    reg_cfg.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;
    if (QUIC_FAILED(api_->RegistrationOpen(&reg_cfg, &reg_))) {
        std::cerr << "[QuicTransport] RegistrationOpen failed\n";
        return false;
    }

    // Configuration (no TLS cert for now — use QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION
    // in test environments; replace with a real cert for production).
    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs              = 30000;
    settings.IsSet.IdleTimeoutMs        = 1;
    settings.PeerBidiStreamCount        = 16;
    settings.IsSet.PeerBidiStreamCount  = 1;

    if (QUIC_FAILED(api_->ConfigurationOpen(
            reg_, &MINDMESH_ALPN, 1,
            &settings, sizeof(settings),
            nullptr, &config_))) {
        std::cerr << "[QuicTransport] ConfigurationOpen failed\n";
        return false;
    }

    QUIC_CREDENTIAL_CONFIG cred_cfg = {};
    cred_cfg.Type  = QUIC_CREDENTIAL_TYPE_NONE;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                     QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    if (QUIC_FAILED(api_->ConfigurationLoadCredential(config_, &cred_cfg))) {
        std::cerr << "[QuicTransport] ConfigurationLoadCredential failed\n";
        return false;
    }

    // Listener on QUIC_DATA_PORT.
    if (QUIC_FAILED(api_->ListenerOpen(reg_, listener_cb, this, &listener_))) {
        std::cerr << "[QuicTransport] ListenerOpen failed\n";
        return false;
    }

    QUIC_ADDR addr = {};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&addr, static_cast<uint16_t>(QUIC_DATA_PORT));

    if (QUIC_FAILED(api_->ListenerStart(listener_, &MINDMESH_ALPN, 1, &addr))) {
        std::cerr << "[QuicTransport] ListenerStart failed\n";
        return false;
    }

    ready_ = true;
    std::cout << "[QuicTransport] Listening on UDP port " << QUIC_DATA_PORT << "\n";
    return true;
}

// Called by NetworkManager after both quic_ and raft_ are constructed
// to wire incoming heartbeat packets into the Raft engine.
// Defined here so it has access to the HeartbeatPacket type.
void NetworkManager::wireHeartbeatReceiver()
{
    quic_->set_recv_callback(
        [this](const std::string& from, const uint8_t* data, size_t len)
        {
            if (len < sizeof(HeartbeatPacket)) return;
            HeartbeatPacket pkt;
            memcpy(&pkt, data, sizeof(pkt));
            if (!raft_) return;
            // Forward both scores into the Raft engine so it maintains
            // a full per-node table (benchmark + live heartbeat score).
            raft_->on_heartbeat_received(pkt.node_id,
                                         pkt.heartbeat_score,
                                         pkt.benchmark_metric,
                                         0 /* seq — NuRaft handles ordering */);
        });
}

void QuicTransport::shutdown()
{
    if (!ready_) return;
    ready_ = false;
    if (listener_ && api_) api_->ListenerClose(listener_);
    if (config_   && api_) api_->ConfigurationClose(config_);
    if (reg_      && api_) api_->RegistrationClose(reg_);
    if (api_)              MsQuicClose(api_);
    listener_ = config_ = reg_ = nullptr;
    api_ = nullptr;
}

bool QuicTransport::send(const std::string& endpoint,
                         const uint8_t*    data,
                         size_t            len)
{
    if (!ready_) return false;

    // Parse "ip:port".
    auto colon = endpoint.rfind(':');
    std::string ip   = (colon != std::string::npos)
                     ? endpoint.substr(0, colon) : endpoint;
    uint16_t    port = (colon != std::string::npos)
                     ? static_cast<uint16_t>(std::stoi(endpoint.substr(colon+1)))
                     : static_cast<uint16_t>(QUIC_DATA_PORT);

    // Open a short-lived connection, send on stream 0, close.
    // For heartbeats and small messages this is acceptable; the transfer
    // engine will hold connections open for large bundles.
    HQUIC conn = nullptr;
    if (QUIC_FAILED(api_->ConnectionOpen(reg_, conn_cb, this, &conn)))
        return false;

    QUIC_ADDR peer = {};
    QuicAddrSetFamily(&peer, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&peer, port);
    inet_pton(AF_INET, ip.c_str(),
              &reinterpret_cast<SOCKADDR_IN*>(&peer)->sin_addr);

    if (QUIC_FAILED(api_->ConnectionStart(conn, config_,
                                           QUIC_ADDRESS_FAMILY_INET,
                                           ip.c_str(), port))) {
        api_->ConnectionClose(conn);
        return false;
    }

    HQUIC stream = nullptr;
    if (QUIC_FAILED(api_->StreamOpen(conn,
                                      QUIC_STREAM_OPEN_FLAG_NONE,
                                      stream_cb, this, &stream))) {
        api_->ConnectionClose(conn);
        return false;
    }

    if (QUIC_FAILED(api_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE))) {
        api_->StreamClose(stream);
        api_->ConnectionClose(conn);
        return false;
    }

    // Copy data into a QUIC buffer and send.
    uint8_t* buf_data = new uint8_t[len];
    memcpy(buf_data, data, len);

    QUIC_BUFFER qbuf;
    qbuf.Buffer = buf_data;
    qbuf.Length = static_cast<uint32_t>(len);

    QUIC_STATUS st = api_->StreamSend(stream, &qbuf, 1,
                                       QUIC_SEND_FLAG_FIN, buf_data);
    if (QUIC_FAILED(st)) {
        delete[] buf_data;
        api_->StreamClose(stream);
        api_->ConnectionClose(conn);
        return false;
    }
    // buf_data freed in stream_cb SEND_COMPLETE event.
    return true;
}

void QuicTransport::set_recv_callback(RecvCallback cb)
{
    std::lock_guard<std::mutex> lk(cb_mtx_);
    recv_cb_ = std::move(cb);
}

// ── MSQuic static callbacks ───────────────────────────────────────────────────

QUIC_STATUS QUIC_API QuicTransport::listener_cb(
        HQUIC, void* ctx, QUIC_LISTENER_EVENT* ev)
{
    auto* self = static_cast<QuicTransport*>(ctx);
    if (ev->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        self->api_->SetCallbackHandler(
            ev->NEW_CONNECTION.Connection, (void*)conn_cb, self);
        return self->api_->ConnectionSetConfiguration(
            ev->NEW_CONNECTION.Connection, self->config_);
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicTransport::conn_cb(
        HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev)
{
    auto* self = static_cast<QuicTransport*>(ctx);
    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        self->api_->SetCallbackHandler(
            ev->PEER_STREAM_STARTED.Stream, (void*)stream_cb, self);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        self->api_->ConnectionClose(conn);
        break;
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicTransport::stream_cb(
        HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev)
{
    auto* self = static_cast<QuicTransport*>(ctx);
    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Collect received buffers and fire the callback.
        std::string from = "unknown";  // TODO: extract peer addr from conn context
        std::lock_guard<std::mutex> lk(self->cb_mtx_);
        if (self->recv_cb_) {
            for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
                self->recv_cb_(from,
                               ev->RECEIVE.Buffers[i].Buffer,
                               ev->RECEIVE.Buffers[i].Length);
            }
        }
        break;
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        // Free the buffer we allocated in send().
        delete[] static_cast<uint8_t*>(ev->SEND_COMPLETE.ClientContext);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        self->api_->StreamClose(stream);
        break;
    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  NetworkManager
// ─────────────────────────────────────────────────────────────────────────────

NetworkManager& NetworkManager::getInstance()
{
    static NetworkManager instance;
    return instance;
}

bool NetworkManager::init()
{
    // Initialise Winsock for UDP scan.
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    // Resolve local IP (first non-loopback IPv4).
    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname));

    addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,
                  &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                  ip, sizeof(ip));
        local_ip_ = ip;
        LOCAL_NODE_IP = ip;
        freeaddrinfo(res);
    } else {
        local_ip_ = "127.0.0.1";
        LOCAL_NODE_IP = "127.0.0.1";
    }

    // Compute and store capacity weight (normalised from benchmark metric).
    // We clamp to (0, 1] — a score of 0 means the node is unsuitable.
    if (metric > 0.0)
        NODE_CAPACITY_WEIGHT = std::min(1.0, metric / 1'000'000.0);
    else
        NODE_CAPACITY_WEIGHT = 0.0;

    // Start MSQuic transport.
    quic_ = std::make_unique<QuicTransport>();
    if (!quic_->init(local_ip_)) {
        std::cerr << "[Network] QuicTransport init failed\n";
        return false;
    }

    std::cout << "[Network] Local IP: " << local_ip_ << "\n";
    return true;
}

// ── CREATE ────────────────────────────────────────────────────────────────────

bool NetworkManager::createNetwork(const std::string& name,
                                   const std::string& password)
{
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (connected_) {
        std::cerr << "[Network] Already connected. Disconnect first.\n";
        return false;
    }

    network_name_     = name;
    network_password_ = password;

    LOCAL_RAFT_NODE_ID  = 1;
    LOCAL_RAFT_ENDPOINT = local_ip_ + ":" + std::to_string(RAFT_PORT);

    // Single-node cluster — self is both leader and only member.
    std::unordered_map<int, std::string> endpoints = {
        { 1, LOCAL_RAFT_ENDPOINT }
    };
    std::unordered_map<int, std::string> ips = {
        { 1, local_ip_ }
    };

    raft_ = std::make_shared<dist::RaftDistribution>(*quic_, endpoints, ips);
    raft_->start();
    wireHeartbeatReceiver();

    connected_ = true;
    stopping_  = false;

    heartbeat_thread_     = std::thread(&NetworkManager::heartbeatLoop,    this);
    scan_listener_thread_ = std::thread(&NetworkManager::scanListenerLoop,  this);

    std::cout << "[Network] Created network '" << name << "' — you are leader.\n";
    return true;
}

// ── SCAN ──────────────────────────────────────────────────────────────────────

std::vector<ScanResult> NetworkManager::scan()
{
    std::vector<ScanResult> results;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return results;

    BOOL bcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<char*>(&bcast), sizeof(bcast));

    DWORD timeout_ms = SCAN_WAIT_MS / 4;  // per-recv timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in dest = {};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(static_cast<u_short>(SCAN_UDP_PORT));
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    sendto(sock, SCAN_PROBE, static_cast<int>(strlen(SCAN_PROBE)) + 1, 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(SCAN_WAIT_MS);

    char buf[512];
    sockaddr_in from = {};
    int from_len = sizeof(from);

    while (std::chrono::steady_clock::now() < deadline) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        if (strncmp(buf, SCAN_REPLY, strlen(SCAN_REPLY)) != 0) continue;

        // Parse "MINDMESH_HERE name=<name> pw=<0|1>"
        ScanResult sr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        sr.leader_ip = ip;

        char* p = buf + strlen(SCAN_REPLY);
        char* name_tag = strstr(p, "name=");
        char* pw_tag   = strstr(p, "pw=");
        if (name_tag) {
            name_tag += 5;
            char* sp = strchr(name_tag, ' ');
            sr.name = sp ? std::string(name_tag, sp) : std::string(name_tag);
        }
        if (pw_tag) {
            sr.has_password = (pw_tag[3] == '1');
        }

        // Deduplicate by leader_ip.
        bool dup = false;
        for (auto& r : results)
            if (r.leader_ip == sr.leader_ip) { dup = true; break; }
        if (!dup) results.push_back(std::move(sr));
    }

    closesocket(sock);
    return results;
}

// ── JOIN ──────────────────────────────────────────────────────────────────────

bool NetworkManager::joinNetwork(const std::string& leader_ip,
                                  const std::string& network_name,
                                  const std::string& password)
{
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (connected_) {
        std::cerr << "[Network] Already connected. Disconnect first.\n";
        return false;
    }

    // TODO: authenticate password with leader over QUIC before proceeding.
    // For now we trust the caller has verified the password via CLI.
    (void)password;

    network_name_ = network_name;

    // Assign a node ID — in the full implementation the leader assigns this
    // and sends it back over QUIC.  For now we use a client-side increment.
    LOCAL_RAFT_NODE_ID  = 2;  // TODO: receive from leader
    LOCAL_RAFT_ENDPOINT = local_ip_ + ":" + std::to_string(RAFT_PORT);

    std::unordered_map<int, std::string> endpoints = {
        { 1, leader_ip + ":" + std::to_string(RAFT_PORT) },
        { LOCAL_RAFT_NODE_ID, LOCAL_RAFT_ENDPOINT }
    };
    std::unordered_map<int, std::string> ips = {
        { 1, leader_ip },
        { LOCAL_RAFT_NODE_ID, local_ip_ }
    };

    raft_ = std::make_shared<dist::RaftDistribution>(*quic_, endpoints, ips);
    raft_->start();
    wireHeartbeatReceiver();

    connected_ = true;
    stopping_  = false;

    heartbeat_thread_     = std::thread(&NetworkManager::heartbeatLoop,    this);
    scan_listener_thread_ = std::thread(&NetworkManager::scanListenerLoop,  this);

    std::cout << "[Network] Joined network '" << network_name
              << "' via " << leader_ip << "\n";
    return true;
}

// ── HEARTBEAT LOOP ────────────────────────────────────────────────────────────

void NetworkManager::heartbeatLoop()
{
    // Heartbeat interval per specification: 200 ms.
    constexpr auto INTERVAL = std::chrono::milliseconds(200);

    while (!stopping_) {
        auto next = std::chrono::steady_clock::now() + INTERVAL;

        if (connected_ && raft_) {
            // Build heartbeat packet.
            HeartbeatPacket pkt;
            SystemHealth sh = getSystemHealth();
            // Remaining performance = weighted health composite.
            double remaining = 0.7 * sh.cpuHealth + 0.3 * sh.ramHealth;
            // Piggybacked score = benchmark_metric * remaining_perf.
            // This single value captures both hardware capability and
            // current load, and is what peers use for placement decisions.
            pkt.heartbeat_score  = metric * remaining;
            pkt.benchmark_metric = metric;
            pkt.node_id          = LOCAL_RAFT_NODE_ID;

            // Update our own entry in the Raft engine with the combined score.
            raft_->update_local_metric(pkt.heartbeat_score);

            // Broadcast to all known peers via QUIC.
            // RaftDistribution::node_snapshot() returns all known nodes.
            auto snapshot = raft_->node_snapshot();
            for (auto& [id, info] : snapshot) {
                if (id == LOCAL_RAFT_NODE_ID) continue;
                std::string ep = info.ip + ":" + std::to_string(QUIC_DATA_PORT);
                quic_->send(ep,
                            reinterpret_cast<const uint8_t*>(&pkt),
                            sizeof(pkt));
            }
        }

        std::this_thread::sleep_until(next);
    }
}

// ── UDP SCAN RESPONDER ────────────────────────────────────────────────────────

void NetworkManager::scanListenerLoop()
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    // Allow port reuse so multiple apps on the same machine can coexist.
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<u_short>(SCAN_UDP_PORT));
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    DWORD timeout_ms = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&timeout_ms), sizeof(timeout_ms));

    char buf[256];
    sockaddr_in from = {};
    int from_len = sizeof(from);

    while (!stopping_) {
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        if (strcmp(buf, SCAN_PROBE) != 0) continue;

        // Respond with our network info.
        char reply[256];
        snprintf(reply, sizeof(reply),
                 "%s name=%s pw=%d",
                 SCAN_REPLY,
                 network_name_.c_str(),
                 network_password_.empty() ? 0 : 1);

        sendto(sock, reply, static_cast<int>(strlen(reply)) + 1, 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
    }

    closesocket(sock);
}

// ── STATUS / LEADER / DISCONNECT ─────────────────────────────────────────────

std::string NetworkManager::statusString() const
{
    if (!connected_) return "Not connected to any network.";

    std::ostringstream os;
    os << "Network     : " << network_name_ << "\n"
       << "Connected   : Yes\n"
       << "Role        : " << (isLeader() ? "Leader" : "Follower") << "\n"
       << "Node ID     : " << LOCAL_RAFT_NODE_ID << "\n"
       << "Local IP    : " << local_ip_ << "\n"
       << "Metric      : " << metric << "\n"
       << "Capacity    : " << NODE_CAPACITY_WEIGHT;
    return os.str();
}

std::string NetworkManager::leaderIp() const
{
    if (!connected_ || !raft_) return "";
    int lid = raft_->leader_id();
    if (lid < 0) return "election in progress";
    auto snap = raft_->node_snapshot();
    auto it = snap.find(lid);
    return (it != snap.end()) ? it->second.ip : "";
}

bool NetworkManager::isConnected() const { return connected_; }

bool NetworkManager::isLeader() const
{
    if (!connected_ || !raft_) return false;
    return raft_->is_leader();
}

bool NetworkManager::disconnect()
{
    stopping_ = true;
    if (heartbeat_thread_.joinable())     heartbeat_thread_.join();
    if (scan_listener_thread_.joinable()) scan_listener_thread_.join();

    if (raft_) { raft_->stop(); raft_.reset(); }

    connected_ = false;
    network_name_.clear();
    network_password_.clear();

    std::cout << "[Network] Disconnected.\n";
    return true;
}

void NetworkManager::cleanup()
{
    if (connected_) disconnect();
    if (quic_)      quic_->shutdown();
    WSACleanup();
}

// ── OFFLOAD INTEGRATION ───────────────────────────────────────────────────────

std::string NetworkManager::requestOffload(const std::string& process_id)
{
    if (!connected_ || !raft_) return "";
    return raft_->request_offload(process_id);
}
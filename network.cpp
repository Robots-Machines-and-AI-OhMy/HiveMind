// network.cpp
// ─────────────────────────────────────────────────────────────────────────────
// MindMesh network layer implementation.
// ─────────────────────────────────────────────────────────────────────────────

// global.hpp MUST come first — it only depends on <string> and has no
// Windows headers. Including it before Network.hpp (which pulls in
// msquic.h -> windows.h) ensures extern declarations are visible
// before the Windows macro pollution can interfere.

#include "global.hpp"
#include "global_strings.hpp"
#include "Calculate_Performance.hpp"
#include "Raft_Engine.hpp"
#include "Network.hpp"
#include "tiny_sha.h"
#include "join_protocol.hpp"
#include "leader_protocol.hpp"

// Network.hpp sets WIN32_LEAN_AND_MEAN + NOMINMAX + winsock2.h
// before msquic.h, so the order above is safe.

#include <iostream>
#include <sstream>
#include <cstring>
#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>

// winsock2/msquic already included via Network.hpp.
// ws2_32.lib linked via CMakeLists.txt — no pragma needed.
#include <ws2tcpip.h>   // inet_pton, getaddrinfo extras

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
// §0  Auth helpers
// ─────────────────────────────────────────────────────────────────────────────

// Hash a password with SHA-256 using tiny-sha.
// Empty password (open network) produces all-zero hash.
static void hash_password(const std::string& pw, uint8_t out[32])
{
    memset(out, 0, 32);
    if (pw.empty()) return;
    SHA256_CTX ctx;
    SHA256Init(&ctx);
    SHA256Update(&ctx,
                 reinterpret_cast<const uint8_t*>(pw.data()),
                 pw.size());
    SHA256Final(&ctx, out);
}

// Send a fixed-size struct over a blocking UDP socket to a specific endpoint.
static bool udp_send_to(const std::string& ip, uint16_t port,
                         const void* data, size_t len)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &dst.sin_addr);
    int r = sendto(s, reinterpret_cast<const char*>(data),
                   static_cast<int>(len), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    closesocket(s);
    return r == static_cast<int>(len);
}

// Blocking receive with timeout_ms.  Returns bytes read or -1.
static int udp_recv_timed(SOCKET s, void* buf, int buf_len, int timeout_ms)
{
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&tv), sizeof(tv));
    sockaddr_in from = {};
    int from_len = sizeof(from);
    return recvfrom(s, reinterpret_cast<char*>(buf), buf_len, 0,
                    reinterpret_cast<sockaddr*>(&from), &from_len);
}

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
// ─────────────────────────────────────────────────────────────────────────────
// OFLD forwarding helpers
//
// Wire format for follower→leader offload request (sent via QUIC):
//   [4]  'O','F','L','D'
//   [4]  requester_node_id  (little-endian uint32)
//   [4]  process_id length  (little-endian uint32)
//   [N]  process_id bytes
// ─────────────────────────────────────────────────────────────────────────────

static uint32_t le32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1])<<8
         | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}

void NetworkManager::wireHeartbeatReceiver()
{
    quic_->set_recv_callback(
        [this](const std::string& from, const uint8_t* data, size_t len)
        {
            if (!raft_) return;

            // ── OFLD message (follower → leader) ─────────────────────────────
            if (len >= 12
                && data[0]=='O' && data[1]=='F'
                && data[2]=='L' && data[3]=='D')
            {
                if (!raft_->is_leader()) return;  // drop if we are not the leader

                uint32_t requester_id = le32(data + 4);
                uint32_t pid_len      = le32(data + 8);
                if (len < 12 + pid_len) return;

                std::string process_id(
                    reinterpret_cast<const char*>(data + 12), pid_len);

                // Leader performs placement and commits to Raft log.
                std::string target = raft_->request_offload(
                    process_id,
                    std::chrono::milliseconds(LEADER_RESPONSE_TIMEOUT_MS));

                // Send response back to the requesting follower.
                // Wire: [4]'OFRS' [4]success [4]ip_len [N]ip [4]pid_len [M]pid
                uint32_t ip_len  = uint32_t(target.size());
                uint32_t pid_len2= uint32_t(process_id.size());
                uint8_t  succ    = target.empty() ? 0 : 1;
                std::vector<uint8_t> rsp(4+4+4+ip_len+4+pid_len2);
                rsp[0]='O'; rsp[1]='F'; rsp[2]='R'; rsp[3]='S';
                auto pu32 = [](uint8_t* d, uint32_t v){
                    d[0]=v; d[1]=v>>8; d[2]=v>>16; d[3]=v>>24; };
                pu32(&rsp[4],  succ);
                pu32(&rsp[8],  ip_len);
                if (ip_len) memcpy(&rsp[12], target.data(), ip_len);
                pu32(&rsp[12+ip_len], pid_len2);
                if (pid_len2) memcpy(&rsp[16+ip_len],
                                     process_id.data(), pid_len2);
                quic_->send(from, rsp.data(), rsp.size());
                (void)requester_id;
                return;
            }

            // ── OFRS message (leader → follower response) ────────────────────
            if (len >= 12
                && data[0]=='O' && data[1]=='F'
                && data[2]=='R' && data[3]=='S')
            {
                uint32_t success = le32(data + 4);
                uint32_t ip_len2 = le32(data + 8);
                if (len < 12 + ip_len2 + 4) return;
                std::string target_ip(
                    reinterpret_cast<const char*>(data + 12), ip_len2);
                uint32_t pid_len3 = le32(data + 12 + ip_len2);
                if (len < 16 + ip_len2 + pid_len3) return;
                std::string pid(
                    reinterpret_cast<const char*>(data + 16 + ip_len2), pid_len3);

                raft_->resolve_offload_response(
                    pid,
                    dist::OffloadResponse{ target_ip, success != 0 });
                return;
            }

            // ── Heartbeat packet ──────────────────────────────────────────────
            if (len < sizeof(HeartbeatPacket)) return;
            HeartbeatPacket pkt;
            memcpy(&pkt, data, sizeof(pkt));
            raft_->on_heartbeat_received(pkt.node_id,
                                         pkt.heartbeat_score,
                                         pkt.benchmark_metric,
                                         0);
        });
}

// ── Per-connection context ────────────────────────────────────────────────────
//
// Allocated when a connection is accepted (listener_cb) or opened (send()).
// Stores the peer IP string so stream_cb can pass it to recv_cb_.
// Also accumulates received bytes across multiple RECEIVE events so the
// caller always sees complete messages (heartbeats, OFLD, OFRS).
//
// Lifetime: created on NEW_CONNECTION / ConnectionOpen, deleted in
//           SHUTDOWN_COMPLETE.  MSQuic guarantees SHUTDOWN_COMPLETE fires
//           exactly once per connection handle.

struct ConnCtx {
    QuicTransport* self;
    std::string    peer_ip;    // dotted-decimal, e.g. "192.168.1.5"
    std::string    peer_ep;    // "ip:port" for the recv callback
    std::vector<uint8_t> buf;  // accumulation buffer for fragmented receives
};

// Extract dotted-decimal peer IP from a QUIC connection handle.
static std::string peer_ip_from_conn(const QUIC_API_TABLE* api, HQUIC conn)
{
    QUIC_ADDR addr = {};
    uint32_t  size = sizeof(addr);
    if (QUIC_FAILED(api->GetParam(conn,
                                   QUIC_PARAM_CONN_REMOTE_ADDRESS,
                                   &size, &addr)))
        return "unknown";

    char buf[INET6_ADDRSTRLEN] = {};
    // MSQuic uses QUIC_ADDR which is a union of sockaddr variants.
    // Cast to sockaddr_in for IPv4 (our network is IPv4-only).
    auto* sa4 = reinterpret_cast<sockaddr_in*>(&addr);
    inet_ntop(AF_INET, &sa4->sin_addr, buf, sizeof(buf));
    uint16_t port = ntohs(sa4->sin_port);

    return std::string(buf) + ":" + std::to_string(port);
}

void QuicTransport::shutdown()
{
    if (!ready_) return;
    ready_ = false;

    // Close the listener first so no new connections arrive.
    if (listener_ && api_) {
        api_->ListenerStop(listener_);
        api_->ListenerClose(listener_);
        listener_ = nullptr;
    }

    // Shutdown all tracked connections gracefully.
    {
        std::lock_guard<std::mutex> lk(conn_mtx_);
        for (auto& [ip, conn] : conn_map_)
            if (conn && api_)
                api_->ConnectionShutdown(conn,
                                         QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        conn_map_.clear();
    }

    // Give connections a moment to complete SHUTDOWN_COMPLETE callbacks.
    Sleep(50);

    if (config_   && api_) api_->ConfigurationClose(config_);
    if (reg_      && api_) api_->RegistrationClose(reg_);
    if (api_)              MsQuicClose(api_);
    config_ = reg_ = nullptr;
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

    // Reuse an existing connection to this peer if one is open,
    // otherwise open a new one.  This avoids per-heartbeat connection churn
    // and keeps the peer IP available in conn_map_ for inbound routing.
    HQUIC conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(conn_mtx_);
        auto it = conn_map_.find(ip);
        if (it != conn_map_.end())
            conn = it->second;
    }

    bool new_conn = (conn == nullptr);
    if (new_conn) {
        // Allocate context before ConnectionOpen so conn_cb has it immediately.
        auto* cctx    = new ConnCtx();
        cctx->self    = this;
        cctx->peer_ip = ip;
        cctx->peer_ep = ip + ":" + std::to_string(port);

        if (QUIC_FAILED(api_->ConnectionOpen(reg_, conn_cb, cctx, &conn))) {
            delete cctx;
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(conn_mtx_);
            conn_map_[ip] = conn;
        }

        if (QUIC_FAILED(api_->ConnectionStart(conn, config_,
                                               QUIC_ADDRESS_FAMILY_INET,
                                               ip.c_str(), port))) {
            std::lock_guard<std::mutex> lk(conn_mtx_);
            conn_map_.erase(ip);
            // conn_cb SHUTDOWN_COMPLETE will free cctx and call ConnectionClose
            return false;
        }
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
        // On failure remove from map so next call opens a fresh connection.
        if (new_conn) {
            std::lock_guard<std::mutex> lk(conn_mtx_);
            conn_map_.erase(ip);
        }
        return false;
    }
    // buf_data freed in stream_cb SEND_COMPLETE.
    // Connection stays open in conn_map_ for reuse.
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
        HQUIC conn = ev->NEW_CONNECTION.Connection;

        // Allocate per-connection context with the peer's address.
        auto* cctx      = new ConnCtx();
        cctx->self      = self;
        cctx->peer_ep   = peer_ip_from_conn(self->api_, conn);
        // Store plain IP (without port) for routing
        auto colon      = cctx->peer_ep.rfind(':');
        cctx->peer_ip   = (colon != std::string::npos)
                          ? cctx->peer_ep.substr(0, colon)
                          : cctx->peer_ep;

        // Register this IP so send() can look up the connection later.
        {
            std::lock_guard<std::mutex> lk(self->conn_mtx_);
            self->conn_map_[cctx->peer_ip] = conn;
        }

        self->api_->SetCallbackHandler(conn, (void*)conn_cb, cctx);
        return self->api_->ConnectionSetConfiguration(conn, self->config_);
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicTransport::conn_cb(
        HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev)
{
    auto* cctx = static_cast<ConnCtx*>(ctx);
    auto* self = cctx->self;

    switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        // Pass the ConnCtx as stream context so stream_cb has the peer IP.
        self->api_->SetCallbackHandler(
            ev->PEER_STREAM_STARTED.Stream, (void*)stream_cb, cctx);
        break;

    case QUIC_CONNECTION_EVENT_CONNECTED:
        // Outbound connection: peer address is now available.
        if (cctx->peer_ip.empty() || cctx->peer_ip == "unknown") {
            cctx->peer_ep = peer_ip_from_conn(self->api_, conn);
            auto colon    = cctx->peer_ep.rfind(':');
            cctx->peer_ip = (colon != std::string::npos)
                            ? cctx->peer_ep.substr(0, colon)
                            : cctx->peer_ep;
            std::lock_guard<std::mutex> lk(self->conn_mtx_);
            self->conn_map_[cctx->peer_ip] = conn;
        }
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        // Remove from map and free context.
        {
            std::lock_guard<std::mutex> lk(self->conn_mtx_);
            self->conn_map_.erase(cctx->peer_ip);
        }
        self->api_->ConnectionClose(conn);
        delete cctx;
        break;

    default: break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicTransport::stream_cb(
        HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev)
{
    auto* cctx = static_cast<ConnCtx*>(ctx);
    auto* self = cctx->self;

    switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
        // Accumulate received buffers — a single logical message may arrive
        // in multiple RECEIVE events (especially on slow links or with large
        // OFLD/OFRS payloads).
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
            const uint8_t* d = ev->RECEIVE.Buffers[i].Buffer;
            uint32_t        n = ev->RECEIVE.Buffers[i].Length;
            cctx->buf.insert(cctx->buf.end(), d, d + n);
        }

        // Dispatch complete messages to recv_cb_.
        // HeartbeatPacket (20 bytes), OFLD (12+N), OFRS (16+N+M) all
        // self-describe their length.  Dispatch whatever we have — the
        // callback in wireHeartbeatReceiver() is defensive about length.
        if (!cctx->buf.empty()) {
            std::lock_guard<std::mutex> lk(self->cb_mtx_);
            if (self->recv_cb_) {
                self->recv_cb_(cctx->peer_ip,
                               cctx->buf.data(),
                               cctx->buf.size());
            }
            cctx->buf.clear();
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
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (connected_) {
            std::cerr << "[Network] Already connected. Disconnect first.\n";
            return false;
        }
    }
    // Network I/O (UDP send + 3 s wait) happens WITHOUT state_mtx_ held
    // to avoid deadlocking with scanListenerLoop which also takes state_mtx_.

    // ── Step 1: Build and send JoinRequest ───────────────────────────────────
    JoinRequest req = {};
    req.magic            = JOIN_REQ_MAGIC;
    req.version          = JOIN_PROTO_VER;
    req.raft_port        = static_cast<uint32_t>(RAFT_PORT);
    req.benchmark_metric = metric;
    req.capacity_weight  = NODE_CAPACITY_WEIGHT;

    // Copy local IP into the fixed char array.
    strncpy_s(reinterpret_cast<char*>(req.joiner_ip),
              sizeof(req.joiner_ip),
              local_ip_.c_str(), _TRUNCATE);

    // Hash the password — open networks send all-zero hash.
    hash_password(password, req.password_sha256);

    std::cout << "[Network] Sending join request to " << leader_ip << "...\n";

    // Open a UDP socket bound to an ephemeral port so we can receive the reply.
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[Network] Failed to create join socket.\n";
        return false;
    }

    // Bind to any local port so the leader can reply to us.
    sockaddr_in local_addr = {};
    local_addr.sin_family      = AF_INET;
    local_addr.sin_port        = 0;          // OS picks the port
    local_addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr));

    // Send the request.
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<u_short>(JOIN_LISTEN_PORT));
    inet_pton(AF_INET, leader_ip.c_str(), &dst.sin_addr);

    int sent = sendto(sock,
                      reinterpret_cast<const char*>(&req), sizeof(req), 0,
                      reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    if (sent != sizeof(req)) {
        std::cerr << "[Network] Failed to send join request.\n";
        closesocket(sock);
        return false;
    }

    // ── Step 2: Wait for JoinResponse ────────────────────────────────────────
    JoinResponse resp = {};
    int n = udp_recv_timed(sock, &resp, sizeof(resp), JOIN_RESPONSE_TIMEOUT_MS);
    closesocket(sock);

    if (n != sizeof(resp) || resp.magic != JOIN_RESP_MAGIC) {
        std::cerr << "[Network] No valid response from leader (timeout or bad magic).\n";
        return false;
    }

    // ── Step 3: Interpret response ────────────────────────────────────────────
    switch (resp.result) {
    case JOIN_RESULT_OK:
        break;
    case JOIN_RESULT_BAD_PASSWORD:
        std::cerr << "[Network] Join rejected: incorrect password.\n";
        return false;
    case JOIN_RESULT_NETWORK_FULL:
        std::cerr << "[Network] Join rejected: network is full.\n";
        return false;
    default:
        std::cerr << "[Network] Join rejected by leader (code " << resp.result << ").\n";
        return false;
    }

    uint32_t assigned_id = resp.assigned_node_id;
    std::cout << "[Network] Join accepted. Assigned node ID: " << assigned_id << "\n";

    // ── Step 4: Start Raft with leader-assigned ID ────────────────────────────
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (connected_) return false;  // raced with another join
    network_name_        = network_name;
    network_password_    = password;
    LOCAL_RAFT_NODE_ID   = static_cast<int>(assigned_id);
    LOCAL_RAFT_ENDPOINT  = local_ip_ + ":" + std::to_string(RAFT_PORT);

    std::unordered_map<int, std::string> endpoints = {
        { 1,                 leader_ip + ":" + std::to_string(RAFT_PORT) },
        { LOCAL_RAFT_NODE_ID, LOCAL_RAFT_ENDPOINT }
    };
    std::unordered_map<int, std::string> ips = {
        { 1,                 leader_ip },
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
    // ── UDP scan responder ────────────────────────────────────────────────────
    SOCKET scan_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (scan_sock == INVALID_SOCKET) return;

    BOOL reuse = TRUE;
    setsockopt(scan_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in scan_addr = {};
    scan_addr.sin_family      = AF_INET;
    scan_addr.sin_port        = htons(static_cast<u_short>(SCAN_UDP_PORT));
    scan_addr.sin_addr.s_addr = INADDR_ANY;
    bind(scan_sock, reinterpret_cast<sockaddr*>(&scan_addr), sizeof(scan_addr));

    DWORD scan_timeout = 200;
    setsockopt(scan_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&scan_timeout), sizeof(scan_timeout));

    // ── JOIN request listener ─────────────────────────────────────────────────
    // Only the leader handles JoinRequests; followers ignore them.
    SOCKET join_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (join_sock == INVALID_SOCKET) { closesocket(scan_sock); return; }

    setsockopt(join_sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in join_addr = {};
    join_addr.sin_family      = AF_INET;
    join_addr.sin_port        = htons(static_cast<u_short>(JOIN_LISTEN_PORT));
    join_addr.sin_addr.s_addr = INADDR_ANY;
    bind(join_sock, reinterpret_cast<sockaddr*>(&join_addr), sizeof(join_addr));

    DWORD join_timeout = 200;
    setsockopt(join_sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<char*>(&join_timeout), sizeof(join_timeout));

    // Pre-compute the expected password hash so we only hash once per session.
    uint8_t expected_hash[32];
    hash_password(network_password_, expected_hash);

    char scan_buf[512];
    sockaddr_in from = {};
    int from_len = sizeof(from);

    while (!stopping_) {
        // ── Handle scan probe ─────────────────────────────────────────────────
        from_len = sizeof(from);
        int n = recvfrom(scan_sock, scan_buf, sizeof(scan_buf) - 1, 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n > 0) {
            scan_buf[n] = '\0';
            if (strcmp(scan_buf, SCAN_PROBE) == 0) {
                char reply[256];
                snprintf(reply, sizeof(reply),
                         "%s name=%s pw=%d",
                         SCAN_REPLY,
                         network_name_.c_str(),
                         network_password_.empty() ? 0 : 1);
                sendto(scan_sock, reply, static_cast<int>(strlen(reply)) + 1, 0,
                       reinterpret_cast<sockaddr*>(&from), from_len);
            }
        }

        // ── Handle join request (leader only) ─────────────────────────────────
        if (!isLeader()) continue;

        JoinRequest req = {};
        from_len = sizeof(from);
        n = recvfrom(join_sock,
                     reinterpret_cast<char*>(&req), sizeof(req), 0,
                     reinterpret_cast<sockaddr*>(&from), &from_len);

        if (n != sizeof(req) || req.magic != JOIN_REQ_MAGIC) continue;

        // Authenticate: compare submitted hash with expected hash.
        JoinResponse resp = {};
        resp.magic   = JOIN_RESP_MAGIC;
        resp.version = JOIN_PROTO_VER;

        if (memcmp(req.password_sha256, expected_hash, 32) != 0) {
            resp.result           = JOIN_RESULT_BAD_PASSWORD;
            resp.assigned_node_id = 0;
            std::cerr << "[Network] JOIN rejected (bad password) from "
                      << reinterpret_cast<char*>(req.joiner_ip) << "\n";
        } else {
            // Assign next node ID atomically (no state_mtx_ — avoid deadlock
            // with joinNetwork() which holds state_mtx_ during the UDP wait).
            int new_id = next_node_id_++;

            std::string joiner_ip(reinterpret_cast<char*>(req.joiner_ip));
            std::string joiner_ep = joiner_ip + ":" + std::to_string(req.raft_port);

            resp.result           = JOIN_RESULT_OK;
            resp.assigned_node_id = static_cast<uint32_t>(new_id);

            std::cout << "[Network] JOIN accepted: node " << new_id
                      << " (" << joiner_ip << ")\n";

            // Add peer to Raft on a detached thread — add_srv() may block
            // waiting for Raft consensus and must not stall the JOIN handler.
            if (raft_) {
                auto r = raft_;
                std::thread([r, new_id, joiner_ep, joiner_ip,
                             bm = req.benchmark_metric,
                             cw = req.capacity_weight]() {
                    r->add_peer(new_id, joiner_ep, joiner_ip, bm, cw);
                }).detach();
            }
        }

        // Send response back to joiner.
        sendto(join_sock,
               reinterpret_cast<const char*>(&resp), sizeof(resp), 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
    }

    closesocket(scan_sock);
    closesocket(join_sock);
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

    // Stop Raft before joining threads — prevents deadlock where
    // the detached add_peer thread holds a NuRaft lock that shutdown()
    // waits on, while heartbeat/scan threads wait for stopping_ loops.
    if (raft_) { raft_->stop(); raft_.reset(); }

    if (heartbeat_thread_.joinable())     heartbeat_thread_.join();
    if (scan_listener_thread_.joinable()) scan_listener_thread_.join();

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
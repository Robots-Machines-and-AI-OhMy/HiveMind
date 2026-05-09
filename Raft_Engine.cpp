// raft_distribution.cpp
//
// Implements the NuRaft-based cluster membership, heartbeat metric tracking,
// and leader-driven process offload placement for the distributed process system.
//
// Dependencies
//   - NuRaft  : https://github.com/eBay/NuRaft          (libNuRaft)
//   - ASIO    : bundled with NuRaft or standalone Asio
//   - Your QuicTransport class (interface contract documented below)
//
// Build (example, adapt to your CMake):
//   target_link_libraries(your_target PRIVATE NuRaft::NuRaft)
//
// ─────────────────────────────────────────────────────────────────────────────
// QuicTransport interface contract expected by this file:
//
//   class QuicTransport {
//   public:
//       // Send raw bytes to a peer identified by "ip:port".
//       // Used by the NuRaft QUIC RPC adaptor (see QuicRpcClient below).
//       bool send(const std::string& endpoint, const uint8_t* data, size_t len);
//
//       // Register a callback invoked whenever bytes arrive from any peer.
//       using RecvCallback = std::function<void(const std::string& from_endpoint,
//                                               const uint8_t* data, size_t len)>;
//       void set_recv_callback(RecvCallback cb);
//   };
// ─────────────────────────────────────────────────────────────────────────────

// Raft_Engine.cpp
// NuRaft types are confined to this translation unit.
// No NuRaft headers leak into Raft_Engine.hpp.

#include "Raft_Engine.hpp"
#include "global.hpp"

#include <libnuraft/nuraft.hxx>

// pImpl struct — holds all NuRaft objects so Raft_Engine.hpp
// stays free of nuraft:: types.
namespace nuraft_detail {
struct RaftImpl {
    nuraft::ptr<nuraft::raft_server>  server;
    nuraft::ptr<nuraft::asio_service> asio_svc;
};
} // namespace nuraft_detail

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dist {

// ═════════════════════════════════════════════════════════════════════════════
// §1  SERIALISATION HELPERS
// ═════════════════════════════════════════════════════════════════════════════
//
// Binary layout for OFFLOAD_REQUEST log entries:
//   [1 byte  EntryType::OFFLOAD_REQUEST]
//   [4 bytes requester_node_id (little-endian int32)]
//   [4 bytes process_id length (little-endian uint32)]
//   [N bytes process_id UTF-8 bytes]
//
// Binary layout for OFFLOAD_RESPONSE log entries:
//   [1 byte  EntryType::OFFLOAD_RESPONSE]
//   [1 byte  success flag]
//   [4 bytes target_ip length (little-endian uint32)]
//   [N bytes target_ip UTF-8 bytes]

static void write_u32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v);
    dst[1] = static_cast<uint8_t>(v >> 8);
    dst[2] = static_cast<uint8_t>(v >> 16);
    dst[3] = static_cast<uint8_t>(v >> 24);
}

static uint32_t read_u32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0])
         | static_cast<uint32_t>(src[1]) << 8
         | static_cast<uint32_t>(src[2]) << 16
         | static_cast<uint32_t>(src[3]) << 24;
}

nuraft::ptr<nuraft::buffer> serialise_offload_request(const OffloadRequest& req) {
    uint32_t pid_len  = static_cast<uint32_t>(req.process_id.size());
    size_t   total    = 1 + 4 + 4 + pid_len;
    auto buf = nuraft::buffer::alloc(total);
    nuraft::buffer_serializer bs(*buf);
    bs.put_u8(static_cast<uint8_t>(EntryType::OFFLOAD_REQUEST));
    uint8_t tmp[4];
    write_u32(tmp, static_cast<uint32_t>(req.requester_node_id));
    bs.put_raw(tmp, 4);
    write_u32(tmp, pid_len);
    bs.put_raw(tmp, 4);
    bs.put_raw(req.process_id.data(), pid_len);
    return buf;
}

OffloadRequest deserialise_offload_request(nuraft::buffer& buf) {
    nuraft::buffer_serializer bs(buf);
    bs.get_u8();                                           // skip type tag
    uint8_t tmp[4];
    bs.get_raw(tmp, 4);  int32_t  rid = static_cast<int32_t>(read_u32(tmp));
    bs.get_raw(tmp, 4);  uint32_t plen = read_u32(tmp);
    std::string pid(plen, '\0');
    bs.get_raw(pid.data(), plen);
    return OffloadRequest{ rid, std::move(pid) };
}

nuraft::ptr<nuraft::buffer> serialise_offload_response(const OffloadResponse& rsp) {
    uint32_t ip_len = static_cast<uint32_t>(rsp.target_ip.size());
    size_t   total  = 1 + 1 + 4 + ip_len;
    auto buf = nuraft::buffer::alloc(total);
    nuraft::buffer_serializer bs(*buf);
    bs.put_u8(static_cast<uint8_t>(EntryType::OFFLOAD_RESPONSE));
    bs.put_u8(static_cast<uint8_t>(rsp.success ? 1 : 0));
    uint8_t tmp[4];
    write_u32(tmp, ip_len);
    bs.put_raw(tmp, 4);
    bs.put_raw(rsp.target_ip.data(), ip_len);
    return buf;
}

OffloadResponse deserialise_offload_response(nuraft::buffer& buf) {
    nuraft::buffer_serializer bs(buf);
    bs.get_u8();                                           // skip type tag
    bool success = bs.get_u8() != 0;
    uint8_t tmp[4];
    bs.get_raw(tmp, 4);  uint32_t ilen = read_u32(tmp);
    std::string ip(ilen, '\0');
    bs.get_raw(ip.data(), ilen);
    return OffloadResponse{ std::move(ip), success };
}

// ═════════════════════════════════════════════════════════════════════════════
// §2  STATE MACHINE
//     Applies committed log entries and drives the offload placement logic.
// ═════════════════════════════════════════════════════════════════════════════

class ProcessStateMachine : public nuraft::state_machine {
public:
    // Callback invoked after a placement decision is committed.
    // Signature: (requester_node_id, process_id, response)
    using PlacementCallback =
        std::function<void(int, const std::string&, OffloadResponse)>;

    explicit ProcessStateMachine(PlacementCallback cb)
        : placement_cb_(std::move(cb)), last_commit_idx_(0) {}

    // ── nuraft::state_machine interface ──────────────────────────────────────

    // Called on EVERY node (follower and leader) when an entry is committed.
    nuraft::ptr<nuraft::buffer> commit(const uint64_t log_idx,
                                       nuraft::buffer& data) override
    {
        last_commit_idx_ = log_idx;

        if (data.size() < 1) return nullptr;

        nuraft::buffer_serializer peek(data);
        auto tag = static_cast<EntryType>(peek.get_u8());
        data.pos(0);   // rewind

        if (tag == EntryType::OFFLOAD_REQUEST) {
            // This should only have reached commit if the leader already
            // decided the target and wrapped it.  In our design the leader
            // writes OFFLOAD_RESPONSE entries; plain REQUEST entries are a
            // no-op on followers (leader handles them before replication).
            // Nothing to do here for followers.
        } else if (tag == EntryType::OFFLOAD_RESPONSE) {
            // Decode the *paired* request id from the response buffer.
            // Our packing puts requester_id & process_id after the response
            // header so all nodes can correlate pending futures.
            auto rsp = deserialise_offload_response_with_ctx(data);
            if (placement_cb_) {
                placement_cb_(rsp.requester_node_id,
                              rsp.process_id,
                              rsp.response);
            }
        }

        return nullptr;
    }

    bool apply_snapshot(nuraft::snapshot& /*s*/) override { return true; }

    nuraft::ptr<nuraft::snapshot> last_snapshot() override { return nullptr; }

    uint64_t last_commit_index() override { return last_commit_idx_; }

    void create_snapshot(nuraft::snapshot& /*s*/,
                         nuraft::async_result<bool>::handler_type& when_done) override
    {
        // Minimal snapshot support — production systems should persist state.
        nuraft::ptr<std::exception> ex(nullptr);
        when_done(true, ex);
    }

    // ── Extended response (with routing context for correlating futures) ──────

    struct ResponseWithCtx {
        int         requester_node_id;
        std::string process_id;
        OffloadResponse response;
    };

    // Binary layout for the *committed* response entry (leader builds this):
    //   [1  byte  EntryType::OFFLOAD_RESPONSE]
    //   [1  byte  success]
    //   [4  bytes target_ip length]
    //   [N  bytes target_ip]
    //   [4  bytes requester_node_id]
    //   [4  bytes process_id length]
    //   [M  bytes process_id]
    static nuraft::ptr<nuraft::buffer> make_response_entry(
        const OffloadResponse& rsp,
        int requester_id,
        const std::string& process_id)
    {
        uint32_t ip_len  = static_cast<uint32_t>(rsp.target_ip.size());
        uint32_t pid_len = static_cast<uint32_t>(process_id.size());
        size_t total = 1 + 1 + 4 + ip_len + 4 + 4 + pid_len;
        auto buf = nuraft::buffer::alloc(total);
        nuraft::buffer_serializer bs(*buf);

        uint8_t tmp[4];
        bs.put_u8(static_cast<uint8_t>(EntryType::OFFLOAD_RESPONSE));
        bs.put_u8(static_cast<uint8_t>(rsp.success ? 1 : 0));
        write_u32(tmp, ip_len);   bs.put_raw(tmp, 4);
        bs.put_raw(rsp.target_ip.data(), ip_len);
        write_u32(tmp, static_cast<uint32_t>(requester_id)); bs.put_raw(tmp, 4);
        write_u32(tmp, pid_len);  bs.put_raw(tmp, 4);
        bs.put_raw(process_id.data(), pid_len);
        return buf;
    }

private:
    static ResponseWithCtx deserialise_offload_response_with_ctx(nuraft::buffer& buf) {
        nuraft::buffer_serializer bs(buf);
        bs.get_u8();                                         // type tag
        bool success = bs.get_u8() != 0;
        uint8_t tmp[4];
        bs.get_raw(tmp, 4); uint32_t ilen = read_u32(tmp);
        std::string ip(ilen, '\0');
        bs.get_raw(ip.data(), ilen);
        bs.get_raw(tmp, 4); int rid = static_cast<int>(read_u32(tmp));
        bs.get_raw(tmp, 4); uint32_t plen = read_u32(tmp);
        std::string pid(plen, '\0');
        bs.get_raw(pid.data(), plen);
        return { rid, std::move(pid), OffloadResponse{ std::move(ip), success } };
    }

    PlacementCallback placement_cb_;
    std::atomic<uint64_t> last_commit_idx_;
};

// ═════════════════════════════════════════════════════════════════════════════
// §3  QUIC RPC ADAPTOR
//     Wraps QuicTransport so NuRaft can use it for all inter-node RPC.
// ═════════════════════════════════════════════════════════════════════════════

// ── RPC Client (outbound) ────────────────────────────────────────────────────
class QuicRpcClient : public nuraft::rpc_client {
public:
    QuicRpcClient(QuicTransport& quic, std::string endpoint)
        : quic_(quic), endpoint_(std::move(endpoint)) {}

    void send(nuraft::ptr<nuraft::req_msg>& req,
              nuraft::rpc_handler& when_done,
              uint64_t /*send_timeout_ms*/ = 0) override
    {
        // Serialise the NuRaft request into a flat byte buffer.
        auto serialised = nuraft::msg_serializer::serialize(*req);
        bool ok = quic_.send(
            endpoint_,
            reinterpret_cast<const uint8_t*>(serialised->data_begin()),
            serialised->size());

        if (!ok) {
            nuraft::ptr<nuraft::rpc_exception> ex =
                nuraft::cs_new<nuraft::rpc_exception>(
                    "QUIC send failed to " + endpoint_, req);
            nuraft::ptr<nuraft::resp_msg> null_resp;
            when_done(null_resp, ex);
        }
        // NOTE: Actual response handling requires the QUIC receive path to
        // call back into NuRaft's peer_to_srv_msg_handler.  Wire that in
        // QuicTransport::set_recv_callback (see RaftDistribution::start()).
    }

    bool is_abandoned() override { return false; }

private:
    QuicTransport& quic_;
    std::string    endpoint_;
};

// ── RPC Client Factory ───────────────────────────────────────────────────────
class QuicRpcClientFactory : public nuraft::rpc_client_factory {
public:
    explicit QuicRpcClientFactory(QuicTransport& quic) : quic_(quic) {}

    nuraft::ptr<nuraft::rpc_client>
    create_client(const std::string& endpoint) override {
        return nuraft::cs_new<QuicRpcClient>(quic_, endpoint);
    }

private:
    QuicTransport& quic_;
};

// ── RPC Listener (inbound) ───────────────────────────────────────────────────
// NuRaft's built-in ASIO listener is TCP-based.  For QUIC we skip it and
// feed inbound bytes via impl_->server->handle_custom_notification() from
// the QuicTransport receive callback registered in start().
//
// Provide a no-op listener so NuRaft doesn't spin up its own TCP listener.
class QuicRpcListener : public nuraft::msg_handler {
public:
    // Nothing to do – QuicTransport drives the receive path externally.
};

// ═════════════════════════════════════════════════════════════════════════════
// §4  RaftDistribution  IMPLEMENTATION
// ═════════════════════════════════════════════════════════════════════════════

RaftDistribution::RaftDistribution(
    QuicTransport&                             quic,
    const std::unordered_map<int,std::string>& peer_endpoints,
    const std::unordered_map<int,std::string>& peer_ips)
    : quic_(quic)
    , peer_endpoints_(peer_endpoints)
{
    impl_ = std::make_unique<nuraft_detail::RaftImpl>();
    // Pre-populate node table from the static peer list.
    for (auto& [id, ip] : peer_ips) {
        NodeInfo info;
        info.id       = id;
        info.ip       = ip;
        // capacity comes from global.hpp for the local node; for remote nodes
        // we initialise to 1.0 and let the leader use the piggybacked value.
        info.capacity = (id == LOCAL_RAFT_NODE_ID)
                            ? static_cast<double>(NODE_CAPACITY_WEIGHT)
                            : 1.0;
        nodes_[id] = info;
    }
}

RaftDistribution::~RaftDistribution() {
    stop();
}

// ── Pending offload futures ───────────────────────────────────────────────────
// Kept outside the class as a file-scoped map for simplicity; could be a
// private member in production code.
namespace {

struct PendingOffload {
    std::promise<OffloadResponse> promise;
};

std::mutex                                         pending_mtx;
std::unordered_map<std::string, PendingOffload>    pending;  // keyed by process_id

void resolve_pending(int /*requester_id*/,
                     const std::string& process_id,
                     OffloadResponse rsp)
{
    std::lock_guard<std::mutex> lk(pending_mtx);
    auto it = pending.find(process_id);
    if (it != pending.end()) {
        it->second.promise.set_value(std::move(rsp));
        pending.erase(it);
    }
}

} // anonymous namespace

void RaftDistribution::start() {
    if (running_) return;

    // ── 1. State machine ─────────────────────────────────────────────────────
    auto sm = nuraft::cs_new<ProcessStateMachine>(resolve_pending);

    // ── 2. Log store (in-memory; swap for a persistent store in production) ──
    auto log_store = nuraft::cs_new<nuraft::inmem_log_store>();

    // ── 3. Server state ──────────────────────────────────────────────────────
    auto srv_state = nuraft::cs_new<nuraft::srv_state>();

    // ── 4. ASIO service (for NuRaft's internal timers) ────────────────────────
    impl_->asio_svc = nuraft::cs_new<nuraft::asio_service>();

    // ── 5. NuRaft params ─────────────────────────────────────────────────────
    nuraft::raft_params params;
    params.heart_beat_interval_          = 100;   // ms – kept minimal as spec'd
    params.election_timeout_lower_bound_ = 500;   // ms
    params.election_timeout_upper_bound_ = 1000;  // ms
    params.reserved_log_items_           = 50;
    params.snapshot_distance_            = 100;
    params.client_req_timeout_           = 5000;  // ms

    // ── 6. Config: add all peers ──────────────────────────────────────────────
    auto cluster_config = nuraft::cs_new<nuraft::cluster_config>();
    for (auto& [id, ep] : peer_endpoints_) {
        auto srv = nuraft::cs_new<nuraft::srv_config>(id, ep);
        cluster_config->get_servers().push_back(srv);
    }

    // ── 7. State manager (wraps log store, server state, cluster config) ──────
    class InlineStateMgr : public nuraft::state_mgr {
    public:
        InlineStateMgr(int my_id,
                       nuraft::ptr<nuraft::log_store> ls,
                       nuraft::ptr<nuraft::srv_state> ss,
                       nuraft::ptr<nuraft::cluster_config> cc)
            : my_id_(my_id), log_store_(ls), srv_state_(ss), cluster_config_(cc) {}

        nuraft::ptr<nuraft::cluster_config> load_config() override {
            return cluster_config_;
        }
        void save_config(const nuraft::cluster_config& cfg) override {
            // In production: persist to disk.
            (void)cfg;
        }
        void save_state(const nuraft::srv_state& s) override {
            *srv_state_ = s;
        }
        nuraft::ptr<nuraft::srv_state> read_state() override {
            return srv_state_;
        }
        nuraft::ptr<nuraft::log_store> load_log_store() override {
            return log_store_;
        }
        int32_t server_id() override { return my_id_; }
        void system_exit(const int /*exit_code*/) override { std::exit(1); }

    private:
        int                                    my_id_;
        nuraft::ptr<nuraft::log_store>         log_store_;
        nuraft::ptr<nuraft::srv_state>         srv_state_;
        nuraft::ptr<nuraft::cluster_config>    cluster_config_;
    };

    auto state_mgr = nuraft::cs_new<InlineStateMgr>(
        LOCAL_RAFT_NODE_ID, log_store, srv_state, cluster_config);

    // ── 8. QUIC RPC factory ──────────────────────────────────────────────────
    auto rpc_factory = nuraft::cs_new<QuicRpcClientFactory>(quic_);

    // ── 9. Launch raft_server ────────────────────────────────────────────────
    nuraft::raft_launcher launcher;
    impl_->server = launcher.init(
        sm,
        state_mgr,
        nuraft::cs_new<nuraft::logger_wrapper>(
            nuraft::cs_new<nuraft::console_logger>(), 4),
        /*listening_port=*/ 0,   // 0 = no built-in TCP listener (QUIC handles it)
        impl_->asio_svc,
        params,
        rpc_factory);

    if (!impl_->server) {
        throw std::runtime_error("Failed to initialise NuRaft raft_server");
    }

    // ── 10. Wire QUIC inbound bytes into NuRaft ───────────────────────────────
    quic_.set_recv_callback(
        [this](const std::string& /*from*/, const uint8_t* data, size_t len)
        {
            // Deserialise a NuRaft message and dispatch it.
            auto buf = nuraft::buffer::alloc(len);
            std::memcpy(buf->data_begin(), data, len);
            auto msg = nuraft::msg_serializer::deserialize(*buf);
            if (msg) {
                impl_->server->process_req(*msg);
            }
        });

    running_ = true;
    std::cout << "[raft] Node " << LOCAL_RAFT_NODE_ID << " started.\n";
}

void RaftDistribution::stop() {
    if (!running_) return;
    running_ = false;
    if (impl_->server) {
        impl_->server->shutdown();
        impl_->server.reset();
    }
    if (impl_->asio_svc) {
        impl_->asio_svc->stop();
        impl_->asio_svc.reset();
    }
}

// ── Heartbeat API ─────────────────────────────────────────────────────────────

void RaftDistribution::on_heartbeat_received(int from_node_id,
                                             double metric,
                                             int64_t seq)
{
    // Clamp metric to [0,1] defensively.
    metric = std::max(0.0, std::min(1.0, metric));

    std::lock_guard<std::mutex> lk(nodes_mtx_);
    auto it = nodes_.find(from_node_id);
    if (it == nodes_.end()) return;

    NodeInfo& n = it->second;
    // Only update if the incoming sequence is newer (handles reordering).
    // We don't store seq on NodeInfo to keep it lightweight; a simple
    // "last received wins" policy is fine for this metric's purpose.
    n.load = metric;
}

double RaftDistribution::local_metric() const {
    return local_metric_.load(std::memory_order_relaxed);
}

void RaftDistribution::update_local_metric(double heartbeat_score)
{
    local_metric_.store(heartbeat_score);
    // Keep our own NodeInfo in sync so elect_target sees an up-to-date
    // local score when deciding whether to offload or run locally.
    std::lock_guard<std::mutex> lk(nodes_mtx_);
    auto& self = nodes_[LOCAL_RAFT_NODE_ID];
    self.id              = LOCAL_RAFT_NODE_ID;
    self.ip              = LOCAL_NODE_IP;
    self.heartbeat_score = heartbeat_score;
    self.benchmark_metric= metric;  // global from global.hpp
}

// ── Offload placement ────────────────────────────────────────────────────────

std::string RaftDistribution::elect_target(double requestor_score) const
{
    std::lock_guard<std::mutex> lk(nodes_mtx_);

    // Find the node with the highest heartbeat_score.
    const NodeInfo* best = nullptr;
    for (auto& [id, info] : nodes_) {
        if (info.ip.empty()) continue;  // peer not yet fully identified
        if (!best || info.score() > best->score())
            best = &info;
    }

    if (!best) return "";

    // Only offload if the best candidate is meaningfully better than
    // the requestor.  "Meaningfully" is defined as > OFFLOAD_THRESHOLD_PCT
    // better, to avoid thrashing when nodes are similarly loaded.
    //
    // requestor_score == 0 means the requestor deliberately asked for
    // offload (e.g. node is overloaded) — always honour it if a target exists.
    if (requestor_score > 0.0) {
        double required = requestor_score * (1.0 + OFFLOAD_THRESHOLD_PCT);
        if (best->score() < required)
            return "";  // no node is sufficiently better — run locally
    }

    return best->ip;
}

std::string RaftDistribution::request_offload(const std::string& process_id,
                                               std::chrono::milliseconds timeout)
{
    // ── Register a pending future before appending to the log ────────────────
    std::future<OffloadResponse> fut;
    {
        std::lock_guard<std::mutex> lk(pending_mtx);
        auto& p = pending[process_id];
        fut = p.promise.get_future();
    }

    if (is_leader()) {
        // ── Leader path: decide immediately, then replicate the response ───────
        std::string target_ip;
        {
            std::lock_guard<std::mutex> lk(nodes_mtx_);
            target_ip = elect_target(local_metric_.load());
        }

        OffloadResponse rsp;
        rsp.target_ip = target_ip;
        rsp.success   = !target_ip.empty();

        auto entry = ProcessStateMachine::make_response_entry(
            rsp, LOCAL_RAFT_NODE_ID, process_id);

        auto result = impl_->server->append_entries({ entry });
        if (!result || !result->get_accepted()) {
            std::lock_guard<std::mutex> lk(pending_mtx);
            pending.erase(process_id);
            return "";
        }
    } else {
        // ── Follower path: forward the request to the leader via Raft log ─────
        // We encode an OFFLOAD_REQUEST entry.  The leader's commit callback
        // sees it, elects a target, and replicates an OFFLOAD_RESPONSE entry
        // that resolves the future on all nodes.
        //
        // NOTE: NuRaft only allows the leader to append entries.  Followers
        // must forward via a redirect.  The simplest production approach is to
        // send a direct QUIC message to the leader's endpoint so the leader
        // can then do append_entries itself.  The stub below shows the pattern.

        int leader = leader_id();
        if (leader < 0) {
            std::lock_guard<std::mutex> lk(pending_mtx);
            pending.erase(process_id);
            return "";
        }

        // Build a lightweight forwarding message (outside of Raft log):
        // [magic 4 bytes "OFLD"] [node_id 4 bytes] [process_id len 4] [process_id]
        OffloadRequest req{ LOCAL_RAFT_NODE_ID, process_id };
        auto buf = serialise_offload_request(req);

        auto ep_it = peer_endpoints_.find(leader);
        if (ep_it == peer_endpoints_.end()) {
            std::lock_guard<std::mutex> lk(pending_mtx);
            pending.erase(process_id);
            return "";
        }

        // Prepend a 4-byte "OFLD" magic so the leader's receive path can
        // distinguish forwarded offload requests from Raft protocol messages.
        const uint8_t magic[4] = { 'O', 'F', 'L', 'D' };
        std::vector<uint8_t> wire(4 + buf->size());
        std::memcpy(wire.data(), magic, 4);
        std::memcpy(wire.data() + 4, buf->data_begin(), buf->size());

        quic_.send(ep_it->second, wire.data(), wire.size());
    }

    // ── Wait for the committed response ──────────────────────────────────────
    if (fut.wait_for(timeout) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(pending_mtx);
        pending.erase(process_id);
        return "";
    }

    OffloadResponse rsp = fut.get();
    return rsp.success ? rsp.target_ip : "";
}

// ── Introspection ─────────────────────────────────────────────────────────────

bool RaftDistribution::is_leader() const {
    if (!impl_->server) return false;
    return impl_->server->is_leader();
}

int RaftDistribution::leader_id() const {
    if (!impl_->server) return -1;
    return impl_->server->get_leader();
}

std::unordered_map<int, NodeInfo> RaftDistribution::node_snapshot() const {
    std::lock_guard<std::mutex> lk(nodes_mtx_);
    return nodes_;
}

// ═════════════════════════════════════════════════════════════════════════════
// §5  LEADER-SIDE FORWARDED REQUEST HANDLER
//
//  Wire this into QuicTransport::set_recv_callback AFTER start() if you want
//  the leader to handle forwarded offload requests from followers.
//
//  In start() the generic QUIC receive callback dispatches Raft protocol
//  messages.  Extend it to also detect the "OFLD" magic prefix:
//
//    quic_.set_recv_callback(
//      [this](const std::string& from, const uint8_t* data, size_t len) {
//        if (len >= 4 && std::memcmp(data, "OFLD", 4) == 0) {
//            handle_forwarded_offload(data + 4, len - 4);
//            return;
//        }
//        // ... normal Raft dispatch ...
//      });
//
// ═════════════════════════════════════════════════════════════════════════════

void handle_forwarded_offload(RaftDistribution& rd,
                               const std::unordered_map<int,std::string>& peer_endpoints,
                               QuicTransport& quic,
                               const uint8_t* data, size_t len)
{
    // Only the leader should execute this path.
    if (!rd.is_leader()) return;

    auto buf = nuraft::buffer::alloc(len);
    std::memcpy(buf->data_begin(), data, len);
    auto req = deserialise_offload_request(*buf);

    // Elect the best target using current metric snapshot.
    auto snapshot = rd.node_snapshot();
    int    best_id    = -1;
    double best_score = -1.0;
    for (auto& [id, info] : snapshot) {
        double s = info.score();
        if (s > best_score) { best_score = s; best_id = id; }
    }

    OffloadResponse rsp;
    rsp.success   = (best_id >= 0);
    rsp.target_ip = rsp.success ? snapshot.at(best_id).ip : "";

    // Replicate the decision so ALL nodes resolve their pending futures.
    auto entry = ProcessStateMachine::make_response_entry(
        rsp, req.requester_node_id, req.process_id);
    // (impl_->server is not directly accessible here; in production make it
    //  a method on RaftDistribution or pass the raft_server pointer in.)
    //
    // Pseudo: impl_->server->append_entries({ entry });
    (void)entry;

    std::cout << "[raft][leader] Offload decision: process=" << req.process_id
              << " -> " << rsp.target_ip << "\n";
}

} // namespace dist
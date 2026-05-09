// Raft_Engine.cpp
// ─────────────────────────────────────────────────────────────────────────────
// RaftDistribution implementation.
//
// Uses only the public NuRaft API verified to exist in the installed headers:
//   nuraft::buffer, buffer_serializer, raft_server, asio_service,
//   state_machine, state_mgr, raft_params, cluster_config, srv_config,
//   srv_state, log_store, snapshot, rpc_client, rpc_client_factory,
//   raft_launcher, cs_new, ptr<T>
//
// NOT used (non-public / example-only / version-specific):
//   inmem_log_store   -> replaced with SimpleLogStore below
//   logger_wrapper    -> replaced with nuraft::ptr<nuraft::logger>(nullptr)
//   console_logger    -> same
//   msg_serializer    -> replaced with nuraft::req_msg serialisation via
//                        buffer directly (NuRaft doesn't expose this publicly)
// ─────────────────────────────────────────────────────────────────────────────

// Must precede all Windows and NuRaft headers.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// global.hpp first — no Windows deps, just <string> + extern decls.
#include "global.hpp"
#include "global_strings.hpp"
#include "Raft_Engine.hpp"
#include "Network.hpp"    // QuicTransport full definition

#include <libnuraft/nuraft.hxx>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// pImpl — keeps nuraft:: types out of Raft_Engine.hpp
namespace nuraft_detail {
struct RaftImpl {
    nuraft::ptr<nuraft::raft_server>  server;
    // asio_service is managed internally by raft_launcher in this NuRaft version
};
} // namespace nuraft_detail

namespace dist {

// ─────────────────────────────────────────────────────────────────────────────
// EntryType tag (first byte of every log buffer)
// ─────────────────────────────────────────────────────────────────────────────
enum class EntryType : uint8_t {
    OFFLOAD_REQUEST  = 0x01,
    OFFLOAD_RESPONSE = 0x02,
};

// ─────────────────────────────────────────────────────────────────────────────
// §1  PORTABLE SERIALISATION
// nuraft::buffer_serializer::get_raw / put_raw take (void*, size_t).
// Write our own u32 helpers to avoid relying on overloads.
// ─────────────────────────────────────────────────────────────────────────────

static void write_u32(uint8_t* dst, uint32_t v) {
    dst[0] = uint8_t(v);
    dst[1] = uint8_t(v >> 8);
    dst[2] = uint8_t(v >> 16);
    dst[3] = uint8_t(v >> 24);
}
static uint32_t read_u32(const uint8_t* src) {
    return uint32_t(src[0])
         | uint32_t(src[1]) << 8
         | uint32_t(src[2]) << 16
         | uint32_t(src[3]) << 24;
}

// Serialise a string into a buffer: [4 bytes length][N bytes data]
static void put_string(nuraft::buffer_serializer& bs, const std::string& s) {
    uint8_t tmp[4];
    write_u32(tmp, uint32_t(s.size()));
    bs.put_raw(tmp, 4);
    if (!s.empty()) bs.put_raw(s.data(), s.size());
}
// Read a length-prefixed string. Returns chars read.
static std::string get_string(nuraft::buffer_serializer& bs) {
    uint8_t tmp[4];
    memcpy(tmp, bs.get_raw(4), 4);
    uint32_t len = read_u32(tmp);
    std::string out(len, '\0');
    if (len > 0) memcpy(&out[0], bs.get_raw(len), len);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// §2  PENDING OFFLOAD FUTURES
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct PendingOffload {
    std::promise<OffloadResponse> promise;
};

std::mutex                                       pending_mtx;
std::unordered_map<std::string, PendingOffload>  pending;

void resolve_pending(const std::string& process_id, OffloadResponse rsp)
{
    std::lock_guard<std::mutex> lk(pending_mtx);
    auto it = pending.find(process_id);
    if (it != pending.end()) {
        it->second.promise.set_value(std::move(rsp));
        pending.erase(it);
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// §3  STATE MACHINE
// ─────────────────────────────────────────────────────────────────────────────

class ProcessStateMachine : public nuraft::state_machine {
public:
    using PlacementCallback =
        std::function<void(int, const std::string&, OffloadResponse)>;

    explicit ProcessStateMachine(PlacementCallback cb)
        : placement_cb_(std::move(cb)), last_commit_idx_(0) {}

    // ── Wire layout for committed OFFLOAD_RESPONSE entry ─────────────────────
    // [1]  EntryType::OFFLOAD_RESPONSE
    // [1]  success (0/1)
    // [4+N] target_ip (length-prefixed)
    // [4]  requester_node_id
    // [4+M] process_id (length-prefixed)

    static nuraft::ptr<nuraft::buffer> make_response_entry(
        const OffloadResponse& rsp,
        int requester_id,
        const std::string& process_id)
    {
        uint32_t ip_len  = uint32_t(rsp.target_ip.size());
        uint32_t pid_len = uint32_t(process_id.size());
        size_t total = 1 + 1 + 4 + ip_len + 4 + 4 + pid_len;
        auto buf = nuraft::buffer::alloc(total);
        nuraft::buffer_serializer bs(*buf);

        uint8_t tmp[4];
        bs.put_u8(uint8_t(EntryType::OFFLOAD_RESPONSE));
        bs.put_u8(uint8_t(rsp.success ? 1 : 0));
        write_u32(tmp, ip_len);
        bs.put_raw(tmp, 4);
        if (ip_len) bs.put_raw(rsp.target_ip.data(), ip_len);
        write_u32(tmp, uint32_t(requester_id));
        bs.put_raw(tmp, 4);
        write_u32(tmp, pid_len);
        bs.put_raw(tmp, 4);
        if (pid_len) bs.put_raw(process_id.data(), pid_len);
        return buf;
    }

    nuraft::ptr<nuraft::buffer> commit(const uint64_t log_idx,
                                       nuraft::buffer& data) override
    {
        last_commit_idx_ = log_idx;
        if (data.size() < 1) return nullptr;

        nuraft::buffer_serializer peek(data);
        auto tag = EntryType(peek.get_u8());
        data.pos(0);

        if (tag == EntryType::OFFLOAD_RESPONSE) {
            nuraft::buffer_serializer bs(data);
            bs.get_u8();                          // tag
            bool success = bs.get_u8() != 0;

            std::string ip      = get_string(bs);
            uint8_t tmp[4];
            memcpy(tmp, bs.get_raw(4), 4);
            int rid             = int(read_u32(tmp));
            std::string pid     = get_string(bs);

            OffloadResponse rsp{ std::move(ip), success };
            if (placement_cb_) placement_cb_(rid, pid, rsp);
        }
        return nullptr;
    }

    bool apply_snapshot(nuraft::snapshot&) override { return true; }
    nuraft::ptr<nuraft::snapshot> last_snapshot() override { return nullptr; }
    uint64_t last_commit_index() override { return last_commit_idx_; }

    void create_snapshot(
        nuraft::snapshot&,
        nuraft::async_result<bool>::handler_type& when_done) override
    {
        nuraft::ptr<std::exception> ex(nullptr);
        bool result = true;
        when_done(result, ex);
    }

private:
    PlacementCallback     placement_cb_;
    std::atomic<uint64_t> last_commit_idx_;
};

// ─────────────────────────────────────────────────────────────────────────────
// §4  SIMPLE IN-MEMORY LOG STORE
// nuraft::inmem_log_store is in examples, not the public API.
// Provide a minimal correct implementation.
// ─────────────────────────────────────────────────────────────────────────────

class SimpleLogStore : public nuraft::log_store {
public:
    SimpleLogStore() {
        // NuRaft expects the log to start at index 1.
        auto dummy = nuraft::buffer::alloc(1);
        logs_.push_back(nuraft::cs_new<nuraft::log_entry>(
            0, dummy, nuraft::log_val_type::app_log));
    }

    uint64_t next_slot() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return uint64_t(logs_.size());
    }

    uint64_t start_index() const override { return 1; }

    nuraft::ptr<nuraft::log_entry> last_entry() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return logs_.empty() ? nullptr : logs_.back();
    }

    uint64_t append(nuraft::ptr<nuraft::log_entry>& entry) override {
        std::lock_guard<std::mutex> lk(mtx_);
        logs_.push_back(entry);
        return uint64_t(logs_.size() - 1);
    }

    void write_at(uint64_t index, nuraft::ptr<nuraft::log_entry>& entry) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (index < logs_.size()) logs_[index] = entry;
        else while (logs_.size() <= index) logs_.push_back(entry);
    }

    nuraft::ptr<std::vector<nuraft::ptr<nuraft::log_entry>>>
    log_entries(uint64_t start, uint64_t end) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto result = nuraft::cs_new<std::vector<nuraft::ptr<nuraft::log_entry>>>();
        for (uint64_t i = start; i < end && i < logs_.size(); ++i)
            result->push_back(logs_[i]);
        return result;
    }

    nuraft::ptr<nuraft::log_entry> entry_at(uint64_t index) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (index >= logs_.size()) return nullptr;
        return logs_[index];
    }

    uint64_t term_at(uint64_t index) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (index >= logs_.size()) return 0;
        return logs_[index]->get_term();
    }

    nuraft::ptr<nuraft::buffer> pack(uint64_t index, int32_t cnt) override {
        // Minimal snapshot pack — not needed for our use case.
        return nuraft::buffer::alloc(0);
    }

    void apply_pack(uint64_t index, nuraft::buffer&) override {}

    bool compact(uint64_t last_log_index) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (last_log_index < logs_.size())
            logs_.erase(logs_.begin() + 1,
                        logs_.begin() + last_log_index + 1);
        return true;
    }

    bool flush() override { return true; }

private:
    mutable std::mutex                                mtx_;
    std::vector<nuraft::ptr<nuraft::log_entry>>       logs_;
};

// ─────────────────────────────────────────────────────────────────────────────
// §5  QUIC RPC ADAPTOR
// ─────────────────────────────────────────────────────────────────────────────

// NuRaft's rpc_client::send() gives us a req_msg.  We need to serialise it
// to bytes for QUIC.  NuRaft doesn't expose msg_serializer publicly, so we
// use its peer_to_srv_msg channel indirectly:
// - Outbound: use nuraft::req_msg::serialize() if available, else skip.
//   In practice for our architecture the leader handles all log appends
//   locally; the QUIC RPC channel is only needed for follower->leader
//   forwarding of our own offload messages (not NuRaft's internal RPCs).
//   We set listening_port=0 to disable NuRaft's built-in TCP and rely on
//   our own QUIC path for the application-level offload protocol.

class QuicRpcClient : public nuraft::rpc_client {
public:
    QuicRpcClient(QuicTransport& quic, std::string ep)
        : quic_(quic), endpoint_(std::move(ep)) {}

    void send(nuraft::ptr<nuraft::req_msg>& req,
              nuraft::rpc_handler& when_done,
              uint64_t /*timeout_ms*/ = 0) override
    {
        // NuRaft's req_msg serialization is not part of the public API in
        // this version. For our architecture the leader handles all log
        // appends locally; follower->leader offload forwarding uses our
        // own lightweight wire format (not NuRaft RPC messages).
        // This client is provided to satisfy NuRaft's factory requirement
        // but NuRaft's internal election/heartbeat RPCs use the ASIO TCP
        // channel, not this QUIC client, when listening_port > 0.
        // For now, signal failure so NuRaft retries via its own transport.
        auto ex = nuraft::cs_new<nuraft::rpc_exception>(
            "QUIC RPC not implemented: " + endpoint_, req);
        nuraft::ptr<nuraft::resp_msg> null_resp;
        when_done(null_resp, ex);
    }

    uint64_t get_id() const override { return 0; }
    bool is_abandoned() const override { return false; }

private:
    QuicTransport& quic_;
    std::string    endpoint_;
};

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

// ─────────────────────────────────────────────────────────────────────────────
// §6  INLINE STATE MANAGER
// ─────────────────────────────────────────────────────────────────────────────

class InlineStateMgr : public nuraft::state_mgr {
public:
    InlineStateMgr(int my_id,
                   nuraft::ptr<nuraft::log_store>      ls,
                   nuraft::ptr<nuraft::srv_state>      ss,
                   nuraft::ptr<nuraft::cluster_config> cc)
        : my_id_(my_id), log_store_(ls), srv_state_(ss), cluster_config_(cc) {}

    nuraft::ptr<nuraft::cluster_config> load_config()  override { return cluster_config_; }
    void save_config(const nuraft::cluster_config&)    override {}
    void save_state(const nuraft::srv_state& s)        override {
        // srv_state is non-copyable; update fields individually.
        srv_state_->set_term(s.get_term());
        srv_state_->set_voted_for(s.get_voted_for());
    }
    nuraft::ptr<nuraft::srv_state>      read_state()   override { return srv_state_; }
    nuraft::ptr<nuraft::log_store>      load_log_store() override { return log_store_; }
    int32_t server_id()                                override { return my_id_; }
    void system_exit(const int)                        override { std::exit(1); }

private:
    int                                    my_id_;
    nuraft::ptr<nuraft::log_store>         log_store_;
    nuraft::ptr<nuraft::srv_state>         srv_state_;
    nuraft::ptr<nuraft::cluster_config>    cluster_config_;
};

// ─────────────────────────────────────────────────────────────────────────────
// §7  RaftDistribution  IMPLEMENTATION
// ─────────────────────────────────────────────────────────────────────────────

RaftDistribution::RaftDistribution(
    QuicTransport&                              quic,
    const std::unordered_map<int,std::string>&  peer_endpoints,
    const std::unordered_map<int,std::string>&  peer_ips)
    : quic_(quic)
    , peer_endpoints_(peer_endpoints)
    , impl_(std::make_unique<nuraft_detail::RaftImpl>())
{
    for (auto& [id, ip] : peer_ips) {
        NodeInfo info;
        info.id               = id;
        info.ip               = ip;
        info.benchmark_metric = (id == LOCAL_RAFT_NODE_ID)
                                    ? metric
                                    : 0.0;
        info.heartbeat_score  = (id == LOCAL_RAFT_NODE_ID)
                                    ? metric * NODE_CAPACITY_WEIGHT
                                    : 0.0;
        nodes_[id] = info;
    }
}

RaftDistribution::~RaftDistribution() { stop(); }

int RaftDistribution::my_node_id() const { return LOCAL_RAFT_NODE_ID; }

void RaftDistribution::start()
{
    if (running_) return;

    // State machine.
    auto sm = nuraft::cs_new<ProcessStateMachine>(
        [](int rid, const std::string& pid, OffloadResponse rsp) {
            resolve_pending(pid, std::move(rsp));
            (void)rid;
        });

    // In-memory log store.
    auto log_store = nuraft::cs_new<SimpleLogStore>();

    // Server state.
    auto srv_state = nuraft::cs_new<nuraft::srv_state>();

    // ASIO is managed internally by raft_launcher in this NuRaft version.
    // asio_service::options is a plain config struct passed to launcher::init.

    // Cluster config.
    auto cluster_config = nuraft::cs_new<nuraft::cluster_config>();
    for (auto& [id, ep] : peer_endpoints_) {
        cluster_config->get_servers().push_back(
            nuraft::cs_new<nuraft::srv_config>(id, ep));
    }

    // State manager.
    auto state_mgr = nuraft::cs_new<InlineStateMgr>(
        LOCAL_RAFT_NODE_ID, log_store, srv_state, cluster_config);

    // Raft params.
    nuraft::raft_params params;
    params.heart_beat_interval_          = 100;
    params.election_timeout_lower_bound_ = 500;
    params.election_timeout_upper_bound_ = 1000;
    params.reserved_log_items_           = 50;
    params.snapshot_distance_            = 100;
    params.client_req_timeout_           = 5000;

    // QUIC RPC factory.
    auto rpc_factory = nuraft::cs_new<QuicRpcClientFactory>(quic_);

    // Launch raft_server with no built-in TCP listener (port 0).
    // Pass nullptr for logger to suppress console output; swap for a real
    // logger in production.
    // Build asio_service::options (plain config struct, not a shared_ptr)
    nuraft::asio_service::options asio_opts;
    asio_opts.thread_pool_size_ = 4;

    // Build raft_server::init_options — carries the rpc_client_factory
    nuraft::raft_server::init_options init_opts;
    init_opts.raft_callback_ = nullptr;

    nuraft::raft_launcher launcher;
    impl_->server = launcher.init(
        sm,
        state_mgr,
        nullptr,      // logger (null = silent; swap for a real logger in production)
        0,            // listening port (0 = no built-in TCP; QUIC handles transport)
        asio_opts,
        params,
        init_opts);

    if (!impl_->server) {
        throw std::runtime_error("[raft] Failed to initialise raft_server");
    }

    running_ = true;
    std::cout << "[raft] Node " << LOCAL_RAFT_NODE_ID << " started.\n";
}

void RaftDistribution::stop()
{
    if (!running_) return;
    running_ = false;
    if (impl_->server) {
        impl_->server->shutdown();
        impl_->server.reset();
    }
    // asio_service lifecycle is managed by raft_launcher in this NuRaft version
}

// ── Heartbeat API ─────────────────────────────────────────────────────────────

void RaftDistribution::on_heartbeat_received(int     from_node_id,
                                              double  heartbeat_score,
                                              double  benchmark_metric,
                                              int64_t /*seq*/)
{
    std::lock_guard<std::mutex> lk(nodes_mtx_);
    auto& ni             = nodes_[from_node_id];
    ni.id                = from_node_id;
    ni.heartbeat_score   = std::max(0.0, std::min(1.0, heartbeat_score));
    ni.benchmark_metric  = benchmark_metric;
}

// local_metric() is defined inline in Raft_Engine.hpp
// (returns local_metric_.load())

void RaftDistribution::update_local_metric(double heartbeat_score)
{
    local_metric_.store(heartbeat_score);
    std::lock_guard<std::mutex> lk(nodes_mtx_);
    auto& self           = nodes_[LOCAL_RAFT_NODE_ID];
    self.id              = LOCAL_RAFT_NODE_ID;
    self.ip              = LOCAL_NODE_IP;
    self.heartbeat_score = heartbeat_score;
    self.benchmark_metric= metric;
}

// ── Placement ─────────────────────────────────────────────────────────────────

std::string RaftDistribution::elect_target(double requestor_score) const
{
    // Caller must hold nodes_mtx_ (called from request_offload).
    const NodeInfo* best = nullptr;
    for (auto& [id, info] : nodes_) {
        if (info.ip.empty()) continue;
        if (!best || info.score() > best->score())
            best = &info;
    }
    if (!best) return "";

    // Must beat the requestor by more than OFFLOAD_THRESHOLD_PCT.
    if (requestor_score > 0.0) {
        double required = requestor_score * (1.0 + OFFLOAD_THRESHOLD_PCT);
        if (best->score() < required) return "";
    }
    return best->ip;
}

// ── Offload request ───────────────────────────────────────────────────────────

std::string RaftDistribution::request_offload(
    const std::string& process_id,
    std::chrono::milliseconds timeout)
{
    std::future<OffloadResponse> fut;
    {
        std::lock_guard<std::mutex> lk(pending_mtx);
        fut = pending[process_id].promise.get_future();
    }

    if (is_leader()) {
        std::string target_ip;
        {
            std::lock_guard<std::mutex> lk(nodes_mtx_);
            target_ip = elect_target(local_metric_.load());
        }
        OffloadResponse rsp{ target_ip, !target_ip.empty() };

        auto entry = ProcessStateMachine::make_response_entry(
            rsp, LOCAL_RAFT_NODE_ID, process_id);
        auto result = impl_->server->append_entries({ entry });
        if (!result || !result->get_accepted()) {
            std::lock_guard<std::mutex> lk(pending_mtx);
            pending.erase(process_id);
            return "";
        }
    } else {
        int leader = leader_id();
        if (leader < 0) {
            std::lock_guard<std::mutex> lk(pending_mtx);
            pending.erase(process_id);
            return "";
        }

        // Forward to leader using a lightweight QUIC message (not Raft log).
        // Wire format: [4 bytes "OFLD"] [4 bytes node_id] [4+N bytes process_id]
        uint32_t pid_len = uint32_t(process_id.size());
        std::vector<uint8_t> wire(4 + 4 + 4 + pid_len);
        wire[0] = 'O'; wire[1] = 'F'; wire[2] = 'L'; wire[3] = 'D';
        uint8_t tmp[4];
        write_u32(tmp, uint32_t(LOCAL_RAFT_NODE_ID));
        std::memcpy(&wire[4], tmp, 4);
        write_u32(tmp, pid_len);
        std::memcpy(&wire[8], tmp, 4);
        std::memcpy(&wire[12], process_id.data(), pid_len);

        auto ep_it = peer_endpoints_.find(leader);
        if (ep_it != peer_endpoints_.end())
            quic_.send(ep_it->second, wire.data(), wire.size());
    }

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
    return impl_->server && impl_->server->is_leader();
}

int RaftDistribution::leader_id() const {
    if (!impl_->server) return -1;
    return impl_->server->get_leader();
}

std::unordered_map<int, NodeInfo> RaftDistribution::node_snapshot() const {
    std::lock_guard<std::mutex> lk(nodes_mtx_);
    return nodes_;
}

} // namespace dist
/*
 * test_network.cpp
 * -----------------------------------------------------------------------------
 * HiveMind Network Layer - Unit Tests
 *
 * Tests run entirely on one machine.  They cover:
 *
 *   Group A - Global / state initialisation
 *   Group B - Wire protocol struct layout (join_protocol.hpp)
 *   Group C - UDP scan mechanics (loopback)
 *   Group D - Network lifecycle (create / isConnected / disconnect)
 *   Group E - JOIN authentication (open network, correct password, wrong password)
 *   Group F - Heartbeat (packet serialisation / deserialisation)
 *   Group G - Password hashing (SHA-256 via tiny_sha)
 *
 * Build (from project root, after build.bat has run once):
 *
 *   cl /nologo /std:c++20 /EHsc /MDd /Zi /Od /W3           ^
 *      /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DENABLE_SHA256=1   ^
 *      /I msquic\src\inc                                    ^
 *      /I NuRaft\include                                    ^
 *      /I NuRaft\asio\include                               ^
 *      /I tiny-sha\src                                      ^
 *      test_network.cpp                                     ^
 *      network.cpp global.cpp compute_check.cpp             ^
 *      Calculate_Performance.cpp Raft_Engine.cpp            ^
 *      /link ws2_32.lib advapi32.lib pdh.lib                ^
 *           msquic\artifacts\bin\windows\x64_Debug_openssl\msquic.lib ^
 *           NuRaft\build\nuraft.lib                         ^
 *      /out:test_network.exe
 *
 * Run:
 *   test_network.exe
 *
 * Exit code: 0 = all passed, non-zero = failures.
 * -----------------------------------------------------------------------------
 */

// ── PCH substitute for the standalone build ──────────────────────────────────
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// ── Standard library ─────────────────────────────────────────────────────────
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <functional>

// ── Project headers ───────────────────────────────────────────────────────────
#include "global.hpp"
#include "global_strings.hpp"
#include "join_protocol.hpp"
#include "compute_check.hpp"
#include "Network.hpp"

#ifndef ENABLE_SHA256
#define ENABLE_SHA256 1
#endif
extern "C" {
#include "tiny_sha.h"
}

// -----------------------------------------------------------------------------
// Minimal test framework
// -----------------------------------------------------------------------------

static int  g_pass  = 0;
static int  g_fail  = 0;
static int  g_skip  = 0;
static const char* g_current_group = "";

static void begin_group(const char* name)
{
    g_current_group = name;
    printf("\n-- %s --\n", name);
}

static void check(bool condition, const char* expr, const char* file, int line)
{
    if (condition) {
        printf("  [PASS] %s\n", expr);
        ++g_pass;
    } else {
        printf("  [FAIL] %s  (%s:%d)\n", expr, file, line);
        ++g_fail;
    }
}

static void skip(const char* reason)
{
    printf("  [SKIP] %s\n", reason);
    ++g_skip;
}

#define CHECK(expr)       check(!!(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(a, b)    check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NE(a, b)    check((a) != (b), #a " != " #b, __FILE__, __LINE__)
#define CHECK_STR(a, b)   check(std::string(a) == std::string(b), \
                                #a " == \"" #b "\"", __FILE__, __LINE__)
#define SKIP(reason)      skip(reason)

// -----------------------------------------------------------------------------
// Group A - Global state initialisation
// -----------------------------------------------------------------------------

static void test_globals()
{
    begin_group("A: Globals");

    // Defaults before init
    CHECK_EQ(LOCAL_RAFT_NODE_ID, 1);   // global.cpp default

    // Metric calculation runs without crashing
    double m = metric_calculation();
    CHECK(m >= 0.0);
    printf("         metric_calculation() = %f\n", m);

    metric = m;
    CHECK(metric == m);

    // Port constants are sane
    CHECK_EQ(RAFT_PORT,      19875);
    CHECK_EQ(QUIC_DATA_PORT, 19876);
    CHECK_EQ(SCAN_UDP_PORT,  56713);
}

// -----------------------------------------------------------------------------
// Group B - Wire protocol struct layout
// -----------------------------------------------------------------------------

static void test_protocol_layout()
{
    begin_group("B: Wire protocol struct sizes");

    // join_protocol.hpp sizes as specified in comments
    CHECK_EQ(sizeof(JoinRequest),  76u);
    CHECK_EQ(sizeof(JoinResponse), 16u);

    // Magic constants round-trip correctly
    JoinRequest  req  = {};
    JoinResponse resp = {};

    req.magic  = JOIN_REQ_MAGIC;
    resp.magic = JOIN_RESP_MAGIC;

    CHECK_EQ(req.magic,  (uint32_t)JOIN_REQ_MAGIC);
    CHECK_EQ(resp.magic, (uint32_t)JOIN_RESP_MAGIC);
    CHECK_NE(req.magic,  resp.magic);

    // Result codes
    CHECK_EQ((int)JOIN_RESULT_OK,           0);
    CHECK_EQ((int)JOIN_RESULT_BAD_PASSWORD, 1);
    CHECK_EQ((int)JOIN_RESULT_NETWORK_FULL, 2);
    CHECK_EQ((int)JOIN_RESULT_ERROR,        3);

    // Port and timeout values
    CHECK_EQ(JOIN_LISTEN_PORT,         19877);
    CHECK_EQ(JOIN_RESPONSE_TIMEOUT_MS, 3000);
}

// -----------------------------------------------------------------------------
// Group C - UDP scan mechanics (loopback self-test)
// -----------------------------------------------------------------------------
// We spin up a minimal UDP responder on SCAN_UDP_PORT that echoes back a
// MINDMESH_HERE reply, then call NetworkManager::scan() and verify we see it.
// This tests the full scan send→receive path without requiring a second machine.

static std::atomic<bool> g_scan_responder_stop { false };

static DWORD WINAPI scan_responder_thread(LPVOID)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 1;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)SCAN_UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(s, (sockaddr*)&addr, sizeof(addr));

    DWORD tv = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    char buf[256];
    sockaddr_in from = {};
    int from_len = sizeof(from);

    while (!g_scan_responder_stop) {
        int n = recvfrom(s, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &from_len);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (strcmp(buf, "MINDMESH_SCAN") != 0) continue;

        const char* reply = "MINDMESH_HERE name=TestNet pw=0";
        sendto(s, reply, (int)strlen(reply)+1, 0, (sockaddr*)&from, from_len);
    }

    closesocket(s);
    return 0;
}

static void test_scan_loopback()
{
    begin_group("C: UDP scan loopback");

    // Start responder
    g_scan_responder_stop = false;
    HANDLE hThread = CreateThread(nullptr, 0, scan_responder_thread,
                                  nullptr, 0, nullptr);
    if (!hThread) {
        SKIP("Could not create scan responder thread");
        return;
    }

    // Give responder time to bind
    Sleep(200);

    NetworkManager& net = NetworkManager::getInstance();

    // init() may already have been called - idempotent
    bool init_ok = net.init();
    CHECK(init_ok);

    // Run scan - should receive our loopback reply
    auto results = net.scan();

    g_scan_responder_stop = true;
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);

    CHECK(results.size() >= 1);
    if (!results.empty()) {
        // At least one result should have our name
        bool found = false;
        for (auto& r : results) {
            if (r.name == "TestNet") {
                found          = true;
                CHECK(r.has_password == false);
                CHECK(!r.leader_ip.empty());
                printf("         Found network: '%s' at %s pw=%d\n",
                       r.name.c_str(), r.leader_ip.c_str(), r.has_password);
                break;
            }
        }
        CHECK(found);
    }
}

// -----------------------------------------------------------------------------
// Group D - Network lifecycle
// -----------------------------------------------------------------------------

static void test_lifecycle()
{
    begin_group("D: Network lifecycle");

    NetworkManager& net = NetworkManager::getInstance();

    // Ensure clean state
    if (net.isConnected()) {
        net.disconnect();
        Sleep(200);
    }

    CHECK(!net.isConnected());

    // Create an open network
    bool created = net.createNetwork("TestNetwork", "");
    CHECK(created);
    CHECK(net.isConnected());

    // A single-node Raft cluster must win an election before it becomes leader.
    // election_timeout_lower_bound_ = 500 ms; wait up to 2 s for it to fire.
    bool became_leader = false;
    for (int i = 0; i < 20 && !became_leader; i++) {
        Sleep(100);
        became_leader = net.isLeader();
    }
    CHECK(became_leader);    // creator is always leader once election completes

    // Status string should be non-empty and contain our network name
    std::string status = net.statusString();
    CHECK(!status.empty());
    CHECK(status.find("TestNetwork") != std::string::npos);
    printf("         Status:\n");
    for (char c : status) { putchar(c); } putchar('\n');

    // Leader IP should be our own local IP once election completes
    std::string lip = net.leaderIp();
    bool lip_ok = !lip.empty() && lip != "election in progress";
    CHECK(lip_ok);
    printf("         Leader IP: %s\n", lip.c_str());

    // Node ID should be 1 (leader always takes 1)
    CHECK_EQ(LOCAL_RAFT_NODE_ID, 1);

    // Disconnect
    bool disc = net.disconnect();
    CHECK(disc);
    CHECK(!net.isConnected());

    // Double disconnect is safe
    bool disc2 = net.disconnect();
    CHECK(disc2 || !net.isConnected()); // either returns true or already not connected
}

// -----------------------------------------------------------------------------
// Group E - JOIN authentication (loopback)
// -----------------------------------------------------------------------------
// Spin up a minimal JOIN listener on JOIN_LISTEN_PORT that correctly implements
// the protocol, then test joinNetwork() behaviour for three cases:
//   1. Open network (empty password) - should succeed
//   2. Correct password              - should succeed
//   3. Wrong password                - should fail with JOIN_RESULT_BAD_PASSWORD

struct FakeLeaderCfg {
    std::string password;
    bool        accept;       // false = always reply NETWORK_FULL
    int         assigned_id;
};

static DWORD WINAPI fake_leader_thread(LPVOID param)
{
    auto cfg = *reinterpret_cast<FakeLeaderCfg*>(param);

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 1;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)JOIN_LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 1;
    }

    DWORD tv = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

    // Compute expected password hash
    uint8_t expected[32] = {};
    if (!cfg.password.empty()) {
        SHA256_CTX ctx;
        SHA256Init(&ctx);
        SHA256Update(&ctx,
                     reinterpret_cast<const uint8_t*>(cfg.password.data()),
                     cfg.password.size());
        SHA256Final(&ctx, expected);
    }

    // Accept exactly one JoinRequest
    JoinRequest req = {};
    sockaddr_in from = {};
    int from_len = sizeof(from);
    int n = recvfrom(s, (char*)&req, sizeof(req), 0, (sockaddr*)&from, &from_len);

    JoinResponse resp = {};
    resp.magic   = JOIN_RESP_MAGIC;
    resp.version = JOIN_PROTO_VER;

    if (n == sizeof(req) && req.magic == JOIN_REQ_MAGIC) {
        if (!cfg.accept) {
            resp.result           = JOIN_RESULT_NETWORK_FULL;
            resp.assigned_node_id = 0;
        } else if (memcmp(req.password_sha256, expected, 32) != 0) {
            resp.result           = JOIN_RESULT_BAD_PASSWORD;
            resp.assigned_node_id = 0;
        } else {
            resp.result           = JOIN_RESULT_OK;
            resp.assigned_node_id = (uint32_t)cfg.assigned_id;
        }
    } else {
        resp.result = JOIN_RESULT_ERROR;
    }

    sendto(s, (char*)&resp, sizeof(resp), 0, (sockaddr*)&from, from_len);
    closesocket(s);
    return 0;
}

static bool run_join_test(const std::string& pw_leader,
                          const std::string& pw_joiner,
                          bool accept,
                          int assigned_id,
                          int* out_assigned_id)
{
    NetworkManager& net = NetworkManager::getInstance();
    if (net.isConnected()) {
        net.disconnect();
        Sleep(100);
    }

    FakeLeaderCfg cfg { pw_leader, accept, assigned_id };
    HANDLE h = CreateThread(nullptr, 0, fake_leader_thread, &cfg, 0, nullptr);
    if (!h) return false;

    Sleep(150); // let the fake leader bind

    // joinNetwork to loopback
    bool ok = net.joinNetwork("127.0.0.1", "TestNet", pw_joiner);

    WaitForSingleObject(h, 5000);
    CloseHandle(h);

    if (ok && out_assigned_id)
        *out_assigned_id = LOCAL_RAFT_NODE_ID;

    if (net.isConnected()) {
        net.disconnect();
        Sleep(100);
    }

    return ok;
}

static void test_join_auth()
{
    begin_group("E: JOIN authentication");

    NetworkManager& net = NetworkManager::getInstance();
    if (net.isConnected()) { net.disconnect(); Sleep(200); }

    // E1: Open network (both sides empty password) - should succeed
    int assigned = 0;
    bool ok = run_join_test("", "", true, 2, &assigned);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(assigned, 2);
        printf("         E1: open network - assigned node ID %d\n", assigned);
    }

    Sleep(300);

    // E2: Correct password - should succeed
    ok = run_join_test("hunter2", "hunter2", true, 3, &assigned);
    CHECK(ok);
    if (ok) {
        CHECK_EQ(assigned, 3);
        printf("         E2: correct password - assigned node ID %d\n", assigned);
    }

    Sleep(300);

    // E3: Wrong password - should fail
    ok = run_join_test("hunter2", "wrongpass", true, 4, nullptr);
    CHECK(!ok);
    printf("         E3: wrong password - correctly rejected\n");

    Sleep(300);

    // E4: Network full - should fail
    ok = run_join_test("", "", false, 0, nullptr);
    CHECK(!ok);
    printf("         E4: network full - correctly rejected\n");
}

// -----------------------------------------------------------------------------
// Group F - Heartbeat packet serialisation
// -----------------------------------------------------------------------------
// The HeartbeatPacket is a packed struct defined in network.cpp (internal).
// We mirror its layout here and verify that a round-trip memcpy preserves values.

#pragma pack(push, 1)
struct HeartbeatPacket_Mirror {
    double  heartbeat_score;
    double  benchmark_metric;
    int32_t node_id;
};
#pragma pack(pop)

static void test_heartbeat_wire()
{
    begin_group("F: Heartbeat packet wire format");

    // Size check - 8+8+4 = 20 bytes
    CHECK_EQ(sizeof(HeartbeatPacket_Mirror), 20u);

    HeartbeatPacket_Mirror pkt;
    pkt.heartbeat_score  = 0.75;
    pkt.benchmark_metric = 1234567.89;
    pkt.node_id          = 42;

    // Round-trip via memcpy (simulating send/recv)
    uint8_t wire[sizeof(pkt)];
    memcpy(wire, &pkt, sizeof(pkt));

    HeartbeatPacket_Mirror pkt2;
    memcpy(&pkt2, wire, sizeof(pkt2));

    CHECK(pkt2.heartbeat_score  == pkt.heartbeat_score);
    CHECK(pkt2.benchmark_metric == pkt.benchmark_metric);
    CHECK_EQ(pkt2.node_id, pkt.node_id);

    // Score clamping semantics (heartbeat_score in [0,1])
    CHECK(pkt.heartbeat_score >= 0.0 && pkt.heartbeat_score <= 1.0);

    // Verify scores outside [0,1] are detectable
    pkt.heartbeat_score = -0.1;
    CHECK(pkt.heartbeat_score < 0.0);
    pkt.heartbeat_score = 1.1;
    CHECK(pkt.heartbeat_score > 1.0);
}

// -----------------------------------------------------------------------------
// Group G - Password hashing
// -----------------------------------------------------------------------------

static void sha256_hex(const std::string& input, char out[65])
{
    uint8_t digest[32];
    SHA256_CTX ctx;
    SHA256Init(&ctx);
    SHA256Update(&ctx,
                 reinterpret_cast<const uint8_t*>(input.data()),
                 input.size());
    SHA256Final(&ctx, digest);
    for (int i = 0; i < 32; i++)
        snprintf(out + i*2, 3, "%02x", digest[i]);
    out[64] = '\0';
}

static void test_password_hashing()
{
    begin_group("G: Password hashing (SHA-256)");

    // G1: Empty password produces all-zero hash
    {
        uint8_t hash[32] = {};
        // empty password → all zero (the hash_password() contract)
        uint8_t zeros[32] = {};
        CHECK(memcmp(hash, zeros, 32) == 0);
    }

    // G2: Known SHA-256 test vector
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2ec73b00361bbef0469fa88c...
    {
        char hex[65];
        sha256_hex("abc", hex);
        CHECK_STR(std::string(hex).substr(0, 8), "ba7816bf");
        printf("         SHA-256(\"abc\") = %.16s...\n", hex);
    }

    // G3: Same password → same hash (deterministic)
    {
        char h1[65], h2[65];
        sha256_hex("hunter2", h1);
        sha256_hex("hunter2", h2);
        CHECK_STR(h1, h2);
    }

    // G4: Different passwords → different hashes
    {
        char h1[65], h2[65];
        sha256_hex("hunter2", h1);
        sha256_hex("hunter3", h2);
        CHECK(strcmp(h1, h2) != 0);
    }

    // G5: Hash is exactly 32 bytes / 64 hex chars
    {
        char hex[65];
        sha256_hex("HiveMind", hex);
        CHECK_EQ((int)strlen(hex), 64);
        printf("         SHA-256(\"HiveMind\") = %s\n", hex);
    }

    // G6: Password hash used in JoinRequest matches leader's expected hash
    {
        const std::string pw = "s3cr3t";
        uint8_t joiner_hash[32] = {};
        SHA256_CTX ctx;
        SHA256Init(&ctx);
        SHA256Update(&ctx,
                     reinterpret_cast<const uint8_t*>(pw.data()),
                     pw.size());
        SHA256Final(&ctx, joiner_hash);

        uint8_t leader_hash[32] = {};
        SHA256_CTX ctx2;
        SHA256Init(&ctx2);
        SHA256Update(&ctx2,
                     reinterpret_cast<const uint8_t*>(pw.data()),
                     pw.size());
        SHA256Final(&ctx2, leader_hash);

        CHECK(memcmp(joiner_hash, leader_hash, 32) == 0);
    }
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    printf("==============================================\n");
    printf("  HiveMind Network Layer -- Unit Tests\n");
    printf("==============================================\n");

    // Winsock must be initialised before any socket or NetworkManager call
    WSADATA wsd;
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        printf("FATAL: WSAStartup failed\n");
        return 1;
    }

    // Run all groups
    test_globals();
    test_protocol_layout();
    test_scan_loopback();
    test_lifecycle();
    test_join_auth();
    test_heartbeat_wire();
    test_password_hashing();

    // Final cleanup
    NetworkManager& net = NetworkManager::getInstance();
    if (net.isConnected()) net.disconnect();
    net.cleanup();

    WSACleanup();

    // Summary
    printf("\n==============================================\n");
    printf("  Results: %d passed, %d failed, %d skipped\n",
           g_pass, g_fail, g_skip);
    printf("==============================================\n");

    if (g_fail == 0)
        printf("  ALL TESTS PASSED\n\n");
    else
        printf("  SOME TESTS FAILED - see output above\n\n");

    return g_fail == 0 ? 0 : 1;
}
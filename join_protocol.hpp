#pragma once
// join_protocol.hpp
// ─────────────────────────────────────────────────────────────────────────────
// Wire protocol for the JOIN handshake between a joining node and the leader.
//
// Transport: QUIC stream (short-lived, request/response).
// All integers little-endian.  Fixed-size structs, no framing needed.
//
// Flow:
//   Joiner  ──JoinRequest──►  Leader
//   Leader  ◄─JoinResponse──  Joiner
//
// Password auth:
//   The joiner hashes the password with SHA-256 using tiny_sha and sends the
//   hash.  The leader hashes its stored password and compares.  Plaintext
//   passwords are never sent on the wire.  Open networks use an all-zero hash.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef JOIN_PROTOCOL_HPP
#define JOIN_PROTOCOL_HPP

#include <stdint.h>

// ── Magic / version ───────────────────────────────────────────────────────────
#define JOIN_REQ_MAGIC   0x484D4A52u   // "HMJR" HiveMind Join Request
#define JOIN_RESP_MAGIC  0x484D4A41u   // "HMJA" HiveMind Join Accept/reject
#define JOIN_PROTO_VER   1u

// ── Timeouts ─────────────────────────────────────────────────────────────────
#define JOIN_RESPONSE_TIMEOUT_MS  3000   // joiner waits up to 3 s for leader reply
#define JOIN_LISTEN_PORT          19877  // leader listens for join requests here

// ── Result codes in JoinResponse::result ─────────────────────────────────────
#define JOIN_RESULT_OK            0      // accepted, node_id is valid
#define JOIN_RESULT_BAD_PASSWORD  1      // wrong password hash
#define JOIN_RESULT_NETWORK_FULL  2      // leader hit node limit
#define JOIN_RESULT_ERROR         3      // generic leader-side error

#pragma pack(push, 1)

// ── JoinRequest (joiner → leader) ────────────────────────────────────────────
// Size: 4+4+4+16+32+8+8 = 76 bytes
typedef struct JoinRequest {
    uint32_t magic;               // JOIN_REQ_MAGIC
    uint32_t version;             // JOIN_PROTO_VER
    uint32_t raft_port;           // joiner's RAFT_PORT (so leader can add it)
    uint8_t  joiner_ip[16];       // joiner IP as null-padded ASCII string
    uint8_t  password_sha256[32]; // SHA-256 of password; all-zero for open networks
    double   benchmark_metric;    // raw compute_check score (for leader's node table)
    double   capacity_weight;     // NODE_CAPACITY_WEIGHT
} JoinRequest;

// ── JoinResponse (leader → joiner) ───────────────────────────────────────────
// Size: 4+4+4+4 = 16 bytes
typedef struct JoinResponse {
    uint32_t magic;               // JOIN_RESP_MAGIC
    uint32_t version;             // JOIN_PROTO_VER
    uint32_t result;              // JOIN_RESULT_*
    uint32_t assigned_node_id;    // valid when result == JOIN_RESULT_OK
} JoinResponse;

#pragma pack(pop)

#endif // JOIN_PROTOCOL_HPP
/*
 * leader_protocol.hpp
 * --------------------------------------------------------------
 * Wire protocol between AskEngine (client) and the RAFT leader.
 *
 * This header is the contract your teammate implements on the
 * leader side.  AskEngine sends a CapabilityRequest; the leader
 * responds with an OffloadResponse.
 *
 * Transport assumption: lsQUIC stream (single stream per request,
 * short-lived).  Both packets are fixed-size with no framing —
 * the stream is request/response and then closed.
 *
 * All integers are little-endian.
 *
 * TEAMMATE NOTES:
 *   - Leader must respond within LEADER_RESPONSE_TIMEOUT_MS or
 *     AskEngine will signal OFFLOAD_FALLBACK and the process runs
 *     locally — this is safe by design.
 *   - Leader may respond NO for any reason (no suitable node,
 *     cluster degraded, binary on local-only list, etc.).
 *   - target_node is an ASCII "ip:port" string, null-padded.
 *     Port should be REMOTE_AGENT_PORT unless the remote agent
 *     was started on a custom port.
 * --------------------------------------------------------------
 */

#ifndef LEADER_PROTOCOL_HPP
#define LEADER_PROTOCOL_HPP

#include <stdint.h>

/* ============================================================
 *  TUNABLES
 * ============================================================ */

/* Max time AskEngine waits for the leader response (ms).
 * Must be less than DECISION_TIMEOUT_MS in hook.h (200 ms)
 * so the hook's own timeout fires before the queue slot expires.
 * Set to 150 ms leaving 50 ms headroom.                        */
#define LEADER_RESPONSE_TIMEOUT_MS  150

/* Leader's default QUIC port */
#define LEADER_PORT  19875

/* ============================================================
 *  MAGIC / VERSION
 * ============================================================ */

#define LEADER_REQ_MAGIC   0x4D4D5251u   /* "MMRQ" MindMesh Request  */
#define LEADER_RESP_MAGIC  0x4D4D5253u   /* "MMRS" MindMesh Response */
#define LEADER_PROTO_VER   1u

/* ============================================================
 *  CAPABILITY REQUEST
 *  Sent by AskEngine to the leader for every intercepted process.
 *
 *  Size: fixed 344 bytes.
 * ============================================================ */

#pragma pack(push, 1)

typedef struct CapabilityRequest {
    uint32_t magic;             /* LEADER_REQ_MAGIC                    */
    uint32_t version;           /* LEADER_PROTO_VER                    */

    /* Process identity */
    char     name[64];          /* exe basename, UTF-8, null-padded    */
    char     full_path[260];    /* full path, UTF-8, null-padded       */

    /* Local node hardware snapshot — leader uses this for scheduling  */
    uint32_t local_cpu_count;
    uint64_t local_phys_mem_mb;
    uint64_t local_avail_mem_mb;

    /* Hints */
    uint8_t  has_gui;           /* 1 if process needs a display        */
    uint8_t  reserved[7];       /* pad to 8-byte alignment             */
} CapabilityRequest;            /* total: 4+4+64+260+4+8+8+1+7 = 360  */

/* ============================================================
 *  OFFLOAD RESPONSE
 *  Sent by the leader in reply to a CapabilityRequest.
 *
 *  Size: fixed 72 bytes.
 * ============================================================ */

typedef struct OffloadResponse {
    uint32_t magic;             /* LEADER_RESP_MAGIC                   */
    uint32_t version;           /* LEADER_PROTO_VER                    */

    uint8_t  decision;          /* 0 = YES, 1 = NO  (matches OffloadDecision) */
    uint8_t  reserved[3];

    /* Filled when decision == YES, empty string when NO */
    char     target_node[64];   /* "ip:port", ASCII, null-terminated   */
} OffloadResponse;              /* total: 4+4+1+3+64 = 76              */

#pragma pack(pop)

/* ============================================================
 *  HELPER: populate a CapabilityRequest from a ProcessProfile
 *
 *  Include hook.h before this header to use this function.
 * ============================================================ */

#ifdef HOOK_H   /* only available when hook.h was included first */

#include <string.h>

static inline void fill_capability_request(
    CapabilityRequest      *req,
    const ProcessProfile   *profile)
{
    memset(req, 0, sizeof(*req));
    req->magic              = LEADER_REQ_MAGIC;
    req->version            = LEADER_PROTO_VER;
    req->local_cpu_count    = profile->local_cpu_count;
    req->local_phys_mem_mb  = profile->local_phys_mem_mb;
    req->local_avail_mem_mb = profile->local_avail_mem_mb;
    req->has_gui            = (uint8_t)profile->has_gui;

    /* Convert wide strings to UTF-8 for the wire */
    WideCharToMultiByte(CP_UTF8, 0,
        profile->name,      -1,
        req->name,          sizeof(req->name) - 1,
        NULL, NULL);

    WideCharToMultiByte(CP_UTF8, 0,
        profile->full_path, -1,
        req->full_path,     sizeof(req->full_path) - 1,
        NULL, NULL);
}

#endif /* HOOK_H */

#endif /* LEADER_PROTOCOL_HPP */
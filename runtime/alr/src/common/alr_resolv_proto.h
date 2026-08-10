/* alr_resolv_proto.h — wire format for the resolver bridge.
 *
 * WHY THIS EXISTS (docs/RISKS.md R15, measured 2026-08-02):
 * glibc's resolver sends DNS straight to whatever /etc/resolv.conf names, over
 * plain UDP/TCP port 53.  On Android that path is dead whenever the device has
 * Private DNS (DoT) or a VPN active -- apps are expected to resolve through
 * netd instead, which is what bionic's getaddrinfo does.  So the guest gets
 * "Temporary failure in name resolution" while raw IP sockets work fine.
 *
 * The fix is to hand the question to the other side of the ABI boundary: the
 * preload interposes getaddrinfo and forwards it to a helper thread inside the
 * bionic `alr` process, which calls bionic getaddrinfo and gets netd, DoT and
 * VPN handling for free.
 *
 * Shared verbatim by the bionic server and the glibc client, so the layout must
 * not depend on libc: fixed-width integers, explicit sizes, no structs with
 * implementation-defined padding beyond what is spelled out here.
 */
#ifndef ALR_RESOLV_PROTO_H
#define ALR_RESOLV_PROTO_H

#include <stdint.h>

#define ALR_RESOLV_MAGIC   0x414C5231u   /* "ALR1" */
#define ALR_RESOLV_MAXNAME 256
#define ALR_RESOLV_MAXSERV 64
#define ALR_RESOLV_MAXADDR 28            /* sockaddr_in6 is 28 bytes */
#define ALR_RESOLV_MAXRES  16
#define ALR_RESOLV_ENV     "ALR_RESOLV_SOCK"

/* Request: fixed header, then node[node_len] then serv[serv_len], no NULs. */
struct alr_resolv_req {
    uint32_t magic;
    int32_t  family;      /* AF_UNSPEC/AF_INET/AF_INET6 as seen by the GUEST */
    int32_t  socktype;
    int32_t  protocol;
    int32_t  flags;       /* only AI_PASSIVE/AI_NUMERICHOST are forwarded */
    uint32_t node_len;    /* 0 => node was NULL */
    uint32_t serv_len;    /* 0 => service was NULL */
    uint32_t has_node;
    uint32_t has_serv;
};

struct alr_resolv_ent {
    int32_t  family;
    int32_t  socktype;
    int32_t  protocol;
    uint32_t addrlen;
    uint8_t  addr[ALR_RESOLV_MAXADDR];
};

/* Response: header then `count` entries. */
struct alr_resolv_rsp {
    uint32_t magic;
    int32_t  rc;          /* 0 on success, otherwise a getaddrinfo EAI_* code */
    uint32_t count;
};

/* AF_* and SOCK_* numeric values are identical between bionic and glibc on
 * Linux (they come from the kernel headers), so no translation is needed.
 * EAI_* codes are NOT identical, which is why the server maps them: see
 * alr_resolv_map_eai(). */

#endif /* ALR_RESOLV_PROTO_H */

/*
 * alr_doh_wire.h — TLS-independent DNS wire-format + DoH framing core.
 *
 * This header declares the pure, allocation-free, dependency-free core of the
 * ALR DoH (DNS-over-HTTPS) resolver shim (see libalr_doh.c). It is deliberately
 * split out from the LD_PRELOAD/TLS transport so the wire-format logic can be
 * compiled and unit-tested on the HOST with an ordinary cc — no sockets, no
 * OpenSSL, no Android. Everything here is RFC 1035 (DNS message) + RFC 4648
 * §5 (base64url) + RFC 8484 (DoH GET/POST framing) byte plumbing.
 *
 * Scope (what the core does):
 *   - build a minimal DNS query message for one QNAME + QTYPE (A or AAAA)
 *   - base64url-encode that message for an RFC 8484 GET ?dns=<...> request
 *   - parse a DNS RESPONSE message, extracting the A (4-byte) / AAAA (16-byte)
 *     address RDATA, following CNAME chains, with strict bounds checks and
 *     compression-pointer-safe name skipping (loop-guarded).
 *
 * Out of scope here (lives in libalr_doh.c): the actual TLS/443 connection, the
 * dlopen of libssl, the getaddrinfo() interposition, and the result->addrinfo
 * marshalling. The core never allocates and never calls libc beyond <stddef.h>
 * size types, so it is safe to call from a signal-fragile interposer context and
 * trivially fuzzable on the host.
 *
 * All multi-byte integers in DNS are big-endian (network order); the encoders
 * and decoders here do byte-explicit shifts so the code is endian-agnostic and
 * needs no <arpa/inet.h>.
 */
#ifndef ALR_DOH_WIRE_H
#define ALR_DOH_WIRE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DNS RR TYPE codes we handle. */
#define ALR_DNS_T_A     1u    /* IPv4 host address (RDATA = 4 bytes)  */
#define ALR_DNS_T_AAAA  28u   /* IPv6 host address (RDATA = 16 bytes) */
#define ALR_DNS_T_CNAME 5u    /* canonical name (followed during parse) */

/* DNS CLASS code. */
#define ALR_DNS_C_IN    1u

/* RCODE values referenced by the parser/caller. */
#define ALR_DNS_RCODE_NOERROR  0u
#define ALR_DNS_RCODE_NXDOMAIN 3u

/* Hard caps. A DNS-over-HTTPS query for a single name is tiny; these bounds keep
 * every buffer on the stack and make overflow structurally impossible. */
#define ALR_DOH_MAX_QNAME   255   /* RFC 1035 max domain name length (octets) */
#define ALR_DOH_MAX_QUERY   512   /* a single-question query never exceeds this */
#define ALR_DOH_MAX_B64     768   /* base64url of <=512 bytes (+NUL) fits here  */
#define ALR_DOH_MAX_ADDRS    16   /* max addresses we hand back per lookup       */

/* One decoded address. family is AF_INET(2) or AF_INET6(10) as the caller's libc
 * defines them; the core only fills `len` (4 or 16) and the raw bytes, leaving
 * the numeric family for the transport layer to stamp (so the core needs no
 * <sys/socket.h>). */
typedef struct {
    int     len;            /* 4 (A) or 16 (AAAA) */
    uint8_t addr[16];       /* network-order address bytes */
} alr_doh_addr;

/* Result of parsing a DoH/DNS response body. */
typedef struct {
    int          rcode;                     /* DNS RCODE from the header */
    int          count;                     /* number of addresses filled */
    alr_doh_addr addrs[ALR_DOH_MAX_ADDRS];  /* A and/or AAAA, in wire order */
} alr_doh_result;

/* Error/status codes returned by the encode/decode entry points. >=0 on the
 * encoders means "number of bytes written"; a negative value is one of these. */
enum {
    ALR_DOH_OK            =  0,
    ALR_DOH_E_BADNAME     = -1,   /* QNAME empty/too long/ malformed label    */
    ALR_DOH_E_SMALLBUF    = -2,   /* output buffer too small                  */
    ALR_DOH_E_TRUNC       = -3,   /* response shorter than its own framing    */
    ALR_DOH_E_FORMAT      = -4,   /* response not parseable / id mismatch     */
    ALR_DOH_E_BADTYPE     = -5,   /* qtype not A/AAAA                         */
};

/*
 * alr_doh_build_query — encode one DNS query message into `out`.
 *
 *   id      : the 16-bit DNS transaction id to stamp (caller picks; for DoH it
 *             is conventionally 0, and many resolvers require/prefer 0 so the
 *             GET URL is cache-friendly — but the parser verifies whatever id is
 *             given matches the response, so a random id also works).
 *   qname   : the host to resolve, e.g. "deb.debian.org" (no trailing dot
 *             required; a single trailing dot is tolerated). ASCII only.
 *   qtype   : ALR_DNS_T_A or ALR_DNS_T_AAAA.
 *   out     : caller buffer; outcap should be >= ALR_DOH_MAX_QUERY.
 *
 * Sets RD (recursion desired) and a single question; no EDNS OPT (kept minimal —
 * resolvers answer A/AAAA without it). Returns the byte length written (>0) or a
 * negative ALR_DOH_E_* code. Never writes past outcap.
 */
int alr_doh_build_query(uint16_t id, const char *qname, uint16_t qtype,
                        uint8_t *out, size_t outcap);

/*
 * alr_doh_base64url — RFC 4648 §5 base64url (URL/filename-safe alphabet, NO
 * padding) of `in[0..inlen)` into NUL-terminated `out`. This is the encoding for
 * the RFC 8484 GET request: GET /dns-query?dns=<base64url(dnsmsg)>.
 * Returns the string length (excluding NUL) or ALR_DOH_E_SMALLBUF.
 */
int alr_doh_base64url(const uint8_t *in, size_t inlen, char *out, size_t outcap);

/*
 * alr_doh_parse_response — parse a DNS response message body (the bytes a DoH
 * server returns with Content-Type application/dns-message) and collect A/AAAA
 * addresses into `res`.
 *
 *   msg/msglen : the raw DNS response.
 *   want_id    : the id passed to build_query; a mismatch -> ALR_DOH_E_FORMAT.
 *   want_qtype : ALR_DNS_T_A or ALR_DNS_T_AAAA — only RRs of this type are
 *                collected (CNAMEs are still followed to locate them).
 *   res        : filled with rcode + the address list (cleared first).
 *
 * Returns ALR_DOH_OK on a well-formed response (even an NXDOMAIN/empty one —
 * inspect res->rcode and res->count), or a negative ALR_DOH_E_* on truncation /
 * malformed framing. Compression pointers are followed with a strict jump-count
 * guard so a crafted self-referential pointer cannot loop. Never reads out of
 * bounds.
 */
int alr_doh_parse_response(const uint8_t *msg, size_t msglen,
                           uint16_t want_id, uint16_t want_qtype,
                           alr_doh_result *res);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* ALR_DOH_WIRE_H */

/*
 * alr_doh_wire.c — implementation of the TLS-independent DNS wire-format +
 * DoH framing core declared in alr_doh_wire.h.
 *
 * Pure byte plumbing: no libc string/heap calls, no <arpa/inet.h>, no sockets.
 * Every integer is encoded/decoded byte-explicitly (network/big-endian), so the
 * file is endian-agnostic and host-portable. See the header for the contract.
 *
 * The two non-trivial bits, with their safety argument:
 *
 *  (1) QNAME encoding (alr_doh_build_query): a domain name becomes a sequence of
 *      length-prefixed labels terminated by a zero length byte. We reject an
 *      empty label (".." or a leading dot), a label > 63 octets, and a total
 *      name > 255 octets — exactly RFC 1035 §3.1. Output length is checked
 *      against outcap on every byte, so a too-small buffer returns E_SMALLBUF
 *      rather than overflowing.
 *
 *  (2) Name SKIPPING in a response (skip_name): DNS uses compression pointers
 *      (a label whose top two bits are 11 is a 14-bit offset back into the
 *      message). A malicious/buggy response could point a name at itself to loop
 *      forever. We bound the number of pointer jumps by a small constant
 *      (MAX_JUMPS) and bound every offset to < msglen, so skip_name always
 *      terminates and never reads out of bounds. We only need to SKIP names
 *      (to reach the RR's TYPE/CLASS/TTL/RDLENGTH); we never need to MATERIALIZE
 *      them, which keeps this much simpler than a full decompressor.
 */
#include "alr_doh_wire.h"

/* ---- big-endian load/store helpers (no <arpa/inet.h>) ---- */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

/* ============================ query encoder ============================ */

int alr_doh_build_query(uint16_t id, const char *qname, uint16_t qtype,
                        uint8_t *out, size_t outcap) {
    if (qtype != ALR_DNS_T_A && qtype != ALR_DNS_T_AAAA)
        return ALR_DOH_E_BADTYPE;
    if (!qname || !out)
        return ALR_DOH_E_BADNAME;

    /* Header is 12 bytes; we need at least header + root label + qtype/qclass. */
    if (outcap < 12 + 1 + 4)
        return ALR_DOH_E_SMALLBUF;

    size_t o = 0;

    /* ---- 12-byte header ---- */
    put16(out + o, id);            o += 2;   /* ID */
    /* flags: QR=0 (query), Opcode=0, AA=0, TC=0, RD=1; RA/Z/RCODE=0 -> 0x0100 */
    put16(out + o, 0x0100);        o += 2;
    put16(out + o, 1);             o += 2;   /* QDCOUNT = 1 */
    put16(out + o, 0);             o += 2;   /* ANCOUNT */
    put16(out + o, 0);             o += 2;   /* NSCOUNT */
    put16(out + o, 0);             o += 2;   /* ARCOUNT */

    /* ---- QNAME: length-prefixed labels ---- */
    size_t name_octets = 0;     /* counts label-length bytes + label bytes + root */
    const char *p = qname;
    while (*p) {
        /* measure this label up to '.' or end */
        const char *lab = p;
        size_t llen = 0;
        while (*p && *p != '.') { ++p; ++llen; }

        if (llen == 0) {
            /* empty label: only legal as a single trailing dot ("host."). If we
             * are at end-of-string it is the trailing dot -> stop; otherwise it
             * is "..", a leading dot, or an internal empty label -> malformed. */
            if (*p == '\0') break;            /* trailing dot consumed at loop top */
            return ALR_DOH_E_BADNAME;
        }
        if (llen > 63)
            return ALR_DOH_E_BADNAME;          /* RFC 1035 label cap */

        /* total name octets incl. this length byte + label */
        name_octets += 1 + llen;
        if (name_octets > ALR_DOH_MAX_QNAME)
            return ALR_DOH_E_BADNAME;

        /* bounds: length byte + label + (eventual root) + qtype/qclass */
        if (o + 1 + llen + 1 + 4 > outcap)
            return ALR_DOH_E_SMALLBUF;

        out[o++] = (uint8_t)llen;
        for (size_t i = 0; i < llen; ++i) out[o++] = (uint8_t)lab[i];

        if (*p == '.') ++p;        /* skip the dot, continue to next label */
    }

    /* root label */
    if (o + 1 + 4 > outcap)
        return ALR_DOH_E_SMALLBUF;
    out[o++] = 0x00;

    /* a name with zero labels (qname was "" or ".") is invalid */
    if (name_octets == 0)
        return ALR_DOH_E_BADNAME;

    /* ---- QTYPE, QCLASS ---- */
    put16(out + o, qtype);         o += 2;
    put16(out + o, ALR_DNS_C_IN);  o += 2;

    return (int)o;
}

/* ============================ base64url ============================ */

int alr_doh_base64url(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
    static const char A[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    /* output length = ceil(inlen*4/3), no padding */
    size_t need = (inlen / 3) * 4 + ((inlen % 3) ? (inlen % 3 + 1) : 0);
    if (need + 1 > outcap)
        return ALR_DOH_E_SMALLBUF;

    size_t o = 0, i = 0;
    while (i + 3 <= inlen) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = A[(v >> 18) & 0x3f];
        out[o++] = A[(v >> 12) & 0x3f];
        out[o++] = A[(v >> 6) & 0x3f];
        out[o++] = A[v & 0x3f];
        i += 3;
    }
    size_t rem = inlen - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = A[(v >> 18) & 0x3f];
        out[o++] = A[(v >> 12) & 0x3f];
    } else if (rem == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
        out[o++] = A[(v >> 18) & 0x3f];
        out[o++] = A[(v >> 12) & 0x3f];
        out[o++] = A[(v >> 6) & 0x3f];
    }
    out[o] = '\0';
    return (int)o;
}

/* ============================ response parser ============================ */

/* MAX_JUMPS bounds compression-pointer chasing so a self-referential pointer
 * cannot loop. A legitimate name has at most a handful of pointers; 64 is far
 * beyond any real message and still O(1)-ish. */
#define MAX_JUMPS 64

/*
 * skip_name — advance *pos past the (possibly compressed) domain name that
 * starts at *pos. Returns 0 on success (with *pos pointing just past the name's
 * in-stream bytes), or -1 on any malformation/out-of-bounds.
 *
 * Semantics: a name is a run of labels. A label byte L:
 *   - 0x00            -> end of name.
 *   - 0xC0-prefixed   -> 14-bit pointer; the name CONTINUES at that offset, but
 *                        the IN-STREAM consumption ENDS right after the 2 pointer
 *                        bytes. So once we take the FIRST pointer, the caller's
 *                        cursor is fixed; we keep following pointers only to
 *                        validate termination/bounds (loop-guarded).
 *   - 0x01-0x3F       -> a literal label of that many octets follows.
 *   - 0x40/0x80       -> reserved label types; treated as malformed.
 */
static int skip_name(const uint8_t *msg, size_t msglen, size_t *pos) {
    size_t i = *pos;
    int jumped = 0;          /* have we taken a pointer yet? */
    size_t advanced_to = 0;  /* where the in-stream cursor should land */
    int jumps = 0;

    for (;;) {
        if (i >= msglen) return -1;
        uint8_t b = msg[i];

        if ((b & 0xc0) == 0xc0) {
            /* compression pointer: needs a second byte */
            if (i + 1 >= msglen) return -1;
            if (!jumped) {
                advanced_to = i + 2;   /* in-stream name ends after the pointer */
                jumped = 1;
            }
            if (++jumps > MAX_JUMPS) return -1;
            size_t off = ((size_t)(b & 0x3f) << 8) | msg[i + 1];
            if (off >= msglen) return -1;
            i = off;
            continue;
        }
        if ((b & 0xc0) != 0x00) {
            /* 0x40 / 0x80 high bits -> reserved/extended label types: reject */
            return -1;
        }
        if (b == 0x00) {
            /* end of name */
            if (!jumped) advanced_to = i + 1;
            *pos = advanced_to;
            return 0;
        }
        /* literal label of length b */
        i += 1 + (size_t)b;
        if (i > msglen) return -1;
    }
}

int alr_doh_parse_response(const uint8_t *msg, size_t msglen,
                           uint16_t want_id, uint16_t want_qtype,
                           alr_doh_result *res) {
    /* clear result */
    res->rcode = -1;
    res->count = 0;

    if (msglen < 12) return ALR_DOH_E_TRUNC;

    uint16_t id      = get16(msg + 0);
    uint16_t flags   = get16(msg + 2);
    uint16_t qdcount = get16(msg + 4);
    uint16_t ancount = get16(msg + 6);
    /* nscount/arcount at +8/+10 are not needed (we stop after the answer scan) */

    if (id != want_id) return ALR_DOH_E_FORMAT;

    /* must be a response (QR=1) */
    if ((flags & 0x8000) == 0) return ALR_DOH_E_FORMAT;
    res->rcode = (int)(flags & 0x000f);

    size_t pos = 12;

    /* ---- skip the question section (qdcount questions) ---- */
    for (uint16_t q = 0; q < qdcount; ++q) {
        if (skip_name(msg, msglen, &pos) != 0) return ALR_DOH_E_FORMAT;
        if (pos + 4 > msglen) return ALR_DOH_E_TRUNC;   /* QTYPE + QCLASS */
        pos += 4;
    }

    /* If the server reported an error rcode, there is nothing to collect; a
     * well-formed NXDOMAIN/SERVFAIL is still "parsed OK" — the caller inspects
     * res->rcode. We still return OK so the transport can distinguish a network
     * failure from an authoritative "no such name". */
    if (res->rcode != ALR_DNS_RCODE_NOERROR)
        return ALR_DOH_OK;

    /* ---- walk the answer section ---- */
    for (uint16_t a = 0; a < ancount; ++a) {
        if (skip_name(msg, msglen, &pos) != 0) return ALR_DOH_E_FORMAT;
        /* fixed RR header: TYPE(2) CLASS(2) TTL(4) RDLENGTH(2) = 10 bytes */
        if (pos + 10 > msglen) return ALR_DOH_E_TRUNC;
        uint16_t rtype   = get16(msg + pos);
        uint16_t rclass  = get16(msg + pos + 2);
        /* ttl at pos+4 (4 bytes) ignored */
        uint16_t rdlen   = get16(msg + pos + 8);
        pos += 10;
        if (pos + rdlen > msglen) return ALR_DOH_E_TRUNC;

        if (rclass == ALR_DNS_C_IN && rtype == want_qtype) {
            int want_len = (want_qtype == ALR_DNS_T_A) ? 4 : 16;
            if ((int)rdlen == want_len && res->count < ALR_DOH_MAX_ADDRS) {
                alr_doh_addr *ad = &res->addrs[res->count];
                ad->len = want_len;
                for (int i = 0; i < want_len; ++i) ad->addr[i] = msg[pos + i];
                res->count++;
            }
            /* a wrong-length A/AAAA RDATA is silently skipped (defensive) */
        }
        /* CNAME (and any other RR) is simply skipped: we do not need to resolve
         * the canonical name ourselves because a correct resolver returns the
         * terminal A/AAAA RRs in the SAME answer section after the CNAME chain.
         * Skipping CNAMEs and collecting the trailing address RRs yields the
         * resolved addresses without a second round trip. */
        pos += rdlen;
    }

    return ALR_DOH_OK;
}

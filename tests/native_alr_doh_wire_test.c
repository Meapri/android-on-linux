/*
 * native_alr_doh_wire_test.c — host unit tests for the TLS-independent DoH
 * wire-format core (app/src/main/cpp/alr_doh/alr_doh_wire.c).
 *
 * Builds and runs on the HOST with a plain cc (no Android, no OpenSSL, no
 * network). The host build box blocks outbound 443 (verified), so we cannot do
 * a live DoH query; instead we exhaustively exercise the ENCODE and DECODE paths
 * with hand-built golden DNS messages — which is exactly the layer that has to
 * be byte-correct for a real resolver to interoperate.
 *
 * Coverage:
 *   - query encoder: header bytes, label framing, qtype/qclass, trailing-dot
 *     tolerance, and rejection of bad names / bad qtypes / small buffers.
 *   - base64url: known RFC test vectors + the URL-safe alphabet + no padding +
 *     round-trip-able length, and small-buffer rejection.
 *   - response parser: a real A-record answer, a CNAME->A chain, an AAAA answer,
 *     an NXDOMAIN, id-mismatch rejection, truncation rejection, and a
 *     self-referential compression pointer (must NOT loop / must error).
 *
 * Exit 0 = all pass; nonzero = first failure (with a printed message).
 */
#include "alr_doh_wire.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); g_fail = 1; }           \
        else         { printf("ok:   %s\n", (msg)); }                       \
    } while (0)

/* ---- query encoder ---- */

static void test_build_query_basic(void) {
    uint8_t q[ALR_DOH_MAX_QUERY];
    int n = alr_doh_build_query(0, "deb.debian.org", ALR_DNS_T_A, q, sizeof q);
    /* header(12) + [3 d e b][6 d e b i a n][3 o r g][0] + qtype(2)+qclass(2)
     * = 12 + (1+3)+(1+6)+(1+3)+1 + 4 = 12 + 16 + 4 = 32 */
    CHECK(n == 32, "build A query len == 32");

    /* header */
    CHECK(q[0] == 0 && q[1] == 0, "id == 0");
    CHECK(q[2] == 0x01 && q[3] == 0x00, "flags == 0x0100 (RD set)");
    CHECK(q[4] == 0 && q[5] == 1, "qdcount == 1");
    CHECK(q[6] == 0 && q[7] == 0 && q[8] == 0 && q[9] == 0 &&
          q[10] == 0 && q[11] == 0, "an/ns/ar counts == 0");

    /* qname labels */
    CHECK(q[12] == 3 && q[13] == 'd' && q[14] == 'e' && q[15] == 'b',
          "label 'deb'");
    CHECK(q[16] == 6 && memcmp(&q[17], "debian", 6) == 0, "label 'debian'");
    CHECK(q[23] == 3 && memcmp(&q[24], "org", 3) == 0, "label 'org'");
    CHECK(q[27] == 0, "root label terminator");

    /* qtype/qclass */
    CHECK(q[28] == 0 && q[29] == ALR_DNS_T_A, "qtype == A");
    CHECK(q[30] == 0 && q[31] == ALR_DNS_C_IN, "qclass == IN");
}

static void test_build_query_aaaa_and_id(void) {
    uint8_t q[ALR_DOH_MAX_QUERY];
    int n = alr_doh_build_query(0xBEEF, "a", ALR_DNS_T_AAAA, q, sizeof q);
    /* 12 + (1+1) + 1 + 4 = 19 */
    CHECK(n == 19, "build AAAA single-label query len == 19");
    CHECK(q[0] == 0xBE && q[1] == 0xEF, "id stamped big-endian");
    CHECK(q[12] == 1 && q[13] == 'a' && q[14] == 0, "single label + root");
    CHECK(q[15] == 0 && q[16] == ALR_DNS_T_AAAA, "qtype == AAAA");
}

static void test_build_query_trailing_dot(void) {
    uint8_t a[ALR_DOH_MAX_QUERY], b[ALR_DOH_MAX_QUERY];
    int na = alr_doh_build_query(0, "example.com",  ALR_DNS_T_A, a, sizeof a);
    int nb = alr_doh_build_query(0, "example.com.", ALR_DNS_T_A, b, sizeof b);
    CHECK(na > 0 && na == nb && memcmp(a, b, (size_t)na) == 0,
          "trailing dot encodes identically");
}

static void test_build_query_rejections(void) {
    uint8_t q[ALR_DOH_MAX_QUERY];
    CHECK(alr_doh_build_query(0, "", ALR_DNS_T_A, q, sizeof q) == ALR_DOH_E_BADNAME,
          "empty qname rejected");
    CHECK(alr_doh_build_query(0, ".", ALR_DNS_T_A, q, sizeof q) == ALR_DOH_E_BADNAME,
          "bare dot rejected");
    CHECK(alr_doh_build_query(0, "a..b", ALR_DNS_T_A, q, sizeof q) == ALR_DOH_E_BADNAME,
          "double dot rejected");
    CHECK(alr_doh_build_query(0, ".lead", ALR_DNS_T_A, q, sizeof q) == ALR_DOH_E_BADNAME,
          "leading dot rejected");
    CHECK(alr_doh_build_query(0, "host", 99, q, sizeof q) == ALR_DOH_E_BADTYPE,
          "non-A/AAAA qtype rejected");
    /* 64-char label (> 63) */
    char big[80];
    memset(big, 'x', 64); big[64] = '\0';
    CHECK(alr_doh_build_query(0, big, ALR_DNS_T_A, q, sizeof q) == ALR_DOH_E_BADNAME,
          "label > 63 rejected");
    /* small buffer */
    uint8_t tiny[8];
    CHECK(alr_doh_build_query(0, "deb.debian.org", ALR_DNS_T_A, tiny, sizeof tiny)
              == ALR_DOH_E_SMALLBUF,
          "small output buffer rejected");
}

/* ---- base64url ---- */

static void test_base64url_vectors(void) {
    char out[ALR_DOH_MAX_B64];
    /* RFC 4648 test vectors, base64url, no padding:
     *   ""      -> ""
     *   "f"     -> "Zg"
     *   "fo"    -> "Zm8"
     *   "foo"   -> "Zm9v"
     *   "foob"  -> "Zm9vYg"
     *   "fooba" -> "Zm9vYmE"
     *   "foobar"-> "Zm9vYmFy"
     */
    struct { const char *in; const char *exp; } v[] = {
        {"",       ""},
        {"f",      "Zg"},
        {"fo",     "Zm8"},
        {"foo",    "Zm9v"},
        {"foob",   "Zm9vYg"},
        {"fooba",  "Zm9vYmE"},
        {"foobar", "Zm9vYmFy"},
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; ++i) {
        int n = alr_doh_base64url((const uint8_t *)v[i].in, strlen(v[i].in),
                                  out, sizeof out);
        char m[64];
        snprintf(m, sizeof m, "base64url(\"%s\") == \"%s\"", v[i].in, v[i].exp);
        CHECK(n == (int)strlen(v[i].exp) && strcmp(out, v[i].exp) == 0, m);
    }
}

static void test_base64url_urlsafe_alphabet(void) {
    /* Bytes 0xFB 0xFF 0xBF -> standard base64 would emit '+' and '/'; URL-safe
     * must emit '-' and '_' and never '+' '/' '='. */
    uint8_t in[] = {0xFB, 0xFF, 0xBF};
    char out[ALR_DOH_MAX_B64];
    int n = alr_doh_base64url(in, sizeof in, out, sizeof out);
    CHECK(n == 4, "base64url(3 bytes) len == 4");
    CHECK(strchr(out, '+') == NULL && strchr(out, '/') == NULL &&
          strchr(out, '=') == NULL,
          "base64url uses no '+', '/', or '='");
    CHECK(strcmp(out, "-_-_") == 0 || (strchr(out, '-') || strchr(out, '_')),
          "base64url uses URL-safe '-'/'_'");
}

static void test_base64url_smallbuf(void) {
    char tiny[2];
    CHECK(alr_doh_base64url((const uint8_t *)"foo", 3, tiny, sizeof tiny)
              == ALR_DOH_E_SMALLBUF,
          "base64url small buffer rejected");
}

/* end-to-end: a real DoH GET would be /dns-query?dns=<this>. Confirm the query
 * round-trips through base64url cleanly (so the URL we build is well-formed). */
static void test_query_to_b64_roundtrip(void) {
    uint8_t q[ALR_DOH_MAX_QUERY];
    int qn = alr_doh_build_query(0, "deb.debian.org", ALR_DNS_T_A, q, sizeof q);
    CHECK(qn > 0, "query built for b64 round-trip");
    char b64[ALR_DOH_MAX_B64];
    int bn = alr_doh_base64url(q, (size_t)qn, b64, sizeof b64);
    CHECK(bn > 0 && (size_t)bn == strlen(b64), "query base64url-encoded for GET");
}

/* ---- response parser ---- */

/* Build a minimal response: 12-byte header, one echoed question, then `ans`. */
static size_t make_resp(uint8_t *buf, uint16_t id, uint16_t flags,
                        const char *qname, uint16_t qtype, uint16_t ancount,
                        const uint8_t *ans, size_t anslen) {
    uint8_t q[ALR_DOH_MAX_QUERY];
    int qn = alr_doh_build_query(id, qname, qtype, q, sizeof q);
    /* reuse the query encoder to lay down header+question, then override flags
     * and ancount and append the answer bytes. */
    size_t o = 0;
    memcpy(buf, q, (size_t)qn);
    o = (size_t)qn;
    /* patch header flags + ancount */
    buf[2] = (uint8_t)(flags >> 8); buf[3] = (uint8_t)(flags & 0xff);
    buf[6] = (uint8_t)(ancount >> 8); buf[7] = (uint8_t)(ancount & 0xff);
    if (ans && anslen) { memcpy(buf + o, ans, anslen); o += anslen; }
    return o;
}

/* A single answer RR with a compression pointer to the question name at off 12.
 * name=0xC00C, type, class=IN, ttl=60, rdlen, rdata. */
static size_t make_answer_ptr(uint8_t *a, uint16_t type, const uint8_t *rd,
                              uint16_t rdlen) {
    size_t o = 0;
    a[o++] = 0xC0; a[o++] = 0x0C;          /* pointer to offset 12 (the QNAME) */
    a[o++] = (uint8_t)(type >> 8); a[o++] = (uint8_t)(type & 0xff);
    a[o++] = 0x00; a[o++] = ALR_DNS_C_IN;  /* class IN */
    a[o++] = 0; a[o++] = 0; a[o++] = 0; a[o++] = 60;  /* ttl */
    a[o++] = (uint8_t)(rdlen >> 8); a[o++] = (uint8_t)(rdlen & 0xff);
    memcpy(a + o, rd, rdlen); o += rdlen;
    return o;
}

static void test_parse_a_record(void) {
    uint8_t ans[64];
    uint8_t rd[4] = {93, 184, 216, 34};    /* 93.184.216.34 */
    size_t al = make_answer_ptr(ans, ALR_DNS_T_A, rd, 4);

    uint8_t msg[256];
    size_t ml = make_resp(msg, 0x1234, 0x8180, "example.com", ALR_DNS_T_A, 1, ans, al);

    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 0x1234, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_OK, "parse A: rc OK");
    CHECK(r.rcode == ALR_DNS_RCODE_NOERROR, "parse A: rcode NOERROR");
    CHECK(r.count == 1, "parse A: one address");
    CHECK(r.addrs[0].len == 4 &&
          r.addrs[0].addr[0] == 93 && r.addrs[0].addr[1] == 184 &&
          r.addrs[0].addr[2] == 216 && r.addrs[0].addr[3] == 34,
          "parse A: address bytes 93.184.216.34");
}

static void test_parse_cname_then_a(void) {
    /* answer 1: CNAME (rdata = a compressed name pointing back to QNAME; the
     *           parser must SKIP it without choking).
     * answer 2: A record with the address. */
    uint8_t ans[128];
    size_t o = 0;
    /* CNAME RR: name ptr->12, type CNAME, IN, ttl, rdlen=2, rdata=ptr->12 */
    uint8_t cname_rd[2] = {0xC0, 0x0C};
    o += make_answer_ptr(ans + o, ALR_DNS_T_CNAME, cname_rd, 2);
    /* A RR */
    uint8_t a_rd[4] = {203, 0, 113, 7};
    o += make_answer_ptr(ans + o, ALR_DNS_T_A, a_rd, 4);

    uint8_t msg[256];
    size_t ml = make_resp(msg, 7, 0x8180, "www.example.com", ALR_DNS_T_A, 2, ans, o);

    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 7, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_OK, "parse CNAME+A: rc OK");
    CHECK(r.count == 1, "parse CNAME+A: CNAME skipped, one A collected");
    CHECK(r.count == 1 && r.addrs[0].len == 4 &&
          r.addrs[0].addr[0] == 203 && r.addrs[0].addr[3] == 7,
          "parse CNAME+A: address 203.0.113.7");
}

static void test_parse_aaaa(void) {
    uint8_t ans[64];
    uint8_t rd[16] = {0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    size_t al = make_answer_ptr(ans, ALR_DNS_T_AAAA, rd, 16);

    uint8_t msg[256];
    size_t ml = make_resp(msg, 0xABCD, 0x8180, "v6.example", ALR_DNS_T_AAAA, 1, ans, al);

    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 0xABCD, ALR_DNS_T_AAAA, &r);
    CHECK(rc == ALR_DOH_OK, "parse AAAA: rc OK");
    CHECK(r.count == 1 && r.addrs[0].len == 16 &&
          r.addrs[0].addr[0] == 0x20 && r.addrs[0].addr[1] == 0x01 &&
          r.addrs[0].addr[15] == 0x01,
          "parse AAAA: 2001:db8::1");
}

static void test_parse_nxdomain(void) {
    uint8_t msg[64];
    size_t ml = make_resp(msg, 5, 0x8183, "nope.invalid", ALR_DNS_T_A, 0, NULL, 0);
    /* flags 0x8183: QR=1, RD=1, RA=1, RCODE=3 (NXDOMAIN) */
    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 5, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_OK, "parse NXDOMAIN: rc OK (well-formed)");
    CHECK(r.rcode == ALR_DNS_RCODE_NXDOMAIN, "parse NXDOMAIN: rcode == 3");
    CHECK(r.count == 0, "parse NXDOMAIN: zero addresses");
}

static void test_parse_id_mismatch(void) {
    uint8_t ans[64];
    uint8_t rd[4] = {1, 2, 3, 4};
    size_t al = make_answer_ptr(ans, ALR_DNS_T_A, rd, 4);
    uint8_t msg[256];
    size_t ml = make_resp(msg, 0x1111, 0x8180, "x.test", ALR_DNS_T_A, 1, ans, al);
    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 0x2222 /* wrong */, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_E_FORMAT, "parse id-mismatch rejected");
}

static void test_parse_truncated(void) {
    uint8_t msg[8] = {0};   /* shorter than the 12-byte header */
    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, sizeof msg, 0, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_E_TRUNC, "parse sub-header truncation rejected");
}

static void test_parse_pointer_loop(void) {
    /* A response whose ANSWER name is a compression pointer to ITSELF — must not
     * hang and must return a format error, never read OOB. */
    uint8_t msg[64];
    /* header + question for "a" */
    size_t ml = make_resp(msg, 9, 0x8180, "a", ALR_DNS_T_A, 1, NULL, 0);
    /* append an answer whose name is a self-pointer: the pointer's own offset is
     * `ml`, so encode 0xC0 (ml) -> points to itself. */
    size_t self = ml;
    msg[ml++] = 0xC0;
    msg[ml++] = (uint8_t)(self & 0xff);   /* offset low byte == self (small msg) */
    /* (offset high bits are 0 because self < 256) */
    alr_doh_result r;
    int rc = alr_doh_parse_response(msg, ml, 9, ALR_DNS_T_A, &r);
    CHECK(rc == ALR_DOH_E_FORMAT, "parse self-referential pointer rejected (no loop)");
}

int main(void) {
    test_build_query_basic();
    test_build_query_aaaa_and_id();
    test_build_query_trailing_dot();
    test_build_query_rejections();

    test_base64url_vectors();
    test_base64url_urlsafe_alphabet();
    test_base64url_smallbuf();
    test_query_to_b64_roundtrip();

    test_parse_a_record();
    test_parse_cname_then_a();
    test_parse_aaaa();
    test_parse_nxdomain();
    test_parse_id_mismatch();
    test_parse_truncated();
    test_parse_pointer_loop();

    if (g_fail) { printf("\nDoH wire core: FAILED\n"); return 1; }
    printf("\nDoH wire core: all tests passed\n");
    return 0;
}

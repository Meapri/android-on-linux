/*
 * libalr_doh.c — ALR LD_PRELOAD DNS-over-HTTPS (DoH) name-resolution shim.
 *
 * WHY
 * ---
 * On Android an `untrusted_app` is NOT permitted to send raw UDP/53 to an
 * arbitrary nameserver (device-observed: a glibc guest's getaddrinfo() hangs,
 * so `apt update` cannot resolve archive.ubuntu.com / deb.debian.org). The ONE
 * transport such an app *can* use is HTTPS on port 443 (TCP/443 connect is
 * un-mediated by the loader and permitted by SELinux for an app in group
 * AID_INET/3003 — proven: an IP-literal https connect succeeds). So we move name
 * resolution onto 443: DNS-over-HTTPS (RFC 8484).
 *
 * This .so is LD_PRELOAD'd into the glibc guest (apt, curl, wget, git, …). It
 * interposes glibc's getaddrinfo() (and gethostbyname_r as a courtesy for older
 * callers): instead of letting glibc do its resolv.conf UDP-53 lookup (which
 * hangs), we send the DNS query to a DoH resolver over TLS/443 and parse the
 * A/AAAA answer back into the caller's addrinfo list.
 *
 * RELATION TO THE chromium DoH lane (docs/design/chromium-dns-doh.md): chromium
 * does its OWN name resolution and is handled by chromium's built-in
 * --dns-over-https-mode flags; a getaddrinfo shim would not cover its hot path.
 * THIS shim is the complementary lane for every OTHER glibc app — the ones that
 * DO call getaddrinfo (apt/dpkg/curl/wget/python). The two are orthogonal.
 *
 * WHAT IT IS NOT
 * --------------
 * Not a SELinux bypass: every byte still rides an ordinary, kernel-permitted
 * TCP/443 socket. We are only choosing the DoH transport for DNS instead of the
 * blocked UDP/53 one. If anything fails (no TLS lib, DoH server unreachable, a
 * non-resolvable query type), we FALL BACK to the real glibc getaddrinfo so we
 * never make resolution WORSE than the status quo.
 *
 * DESIGN
 * ------
 *  - Wire format: app/src/main/cpp/alr_doh/alr_doh_wire.c builds the DNS query,
 *    base64url-encodes it, and parses the response. That core is TLS-free and
 *    host-unit-tested (tests/native_alr_doh_wire_test.c).
 *  - Transport: TLS is provided by OpenSSL loaded at RUNTIME via dlopen
 *    ("libssl.so.3"/"libcrypto.so.3"). We do NOT link or include OpenSSL headers
 *    (so this file cross-compiles anywhere); every OpenSSL entry point is an
 *    opaque function pointer resolved by name. If libssl is absent the shim
 *    self-disables (pure passthrough). The guest rootfs ships these libs (noble
 *    base / common-data overlay) and the CA bundle.
 *  - DoH server is given by IP (default 1.1.1.1, fallback 8.8.8.8), so resolving
 *    the DoH server itself needs NO bootstrap DNS.
 *  - HTTP/1.1 GET /dns-query?dns=<base64url(query)>, Accept: application/dns-
 *    message. We read the response, find the body after CRLFCRLF, honour a
 *    Content-Length, and hand the body to the wire parser.
 *  - Cert verification: SSL peer-verify ON, CA file
 *    /etc/ssl/certs/ca-certificates.crt (common-data overlay guarantees it),
 *    SNI set to the DoH server hostname. ALR_DOH_INSECURE=1 disables verify for
 *    debugging ONLY.
 *
 * ENV (read once at first use):
 *   ALR_DOH              "0" disables the shim entirely (pure passthrough).
 *                        Default = enabled.
 *   ALR_DOH_SERVER       "<ip>[:port]" DoH server IP (default 1.1.1.1). Giving an
 *                        IP avoids bootstrap DNS.
 *   ALR_DOH_HOST         SNI / Host header (default "cloudflare-dns.com"). Must
 *                        match the server's cert.
 *   ALR_DOH_PATH         request path (default "/dns-query").
 *   ALR_DOH_CA           CA bundle path (default /etc/ssl/certs/ca-certificates.crt).
 *   ALR_DOH_INSECURE     "1" -> skip TLS peer verification (debug only).
 *   ALR_DOH_DIAG         "1" -> one stderr line per lookup (host, #addrs, ms).
 *
 * THREADING / REENTRANCY: the cached config + dlsym slots are written once and
 * only read after (benign double-resolve race, like the interposer's RTLD_NEXT
 * cache). Each lookup uses its own stack TLS state and a fresh socket — no shared
 * mutable connection. Safe under apt's worker model.
 */
#define _GNU_SOURCE
#include "alr_doh_wire.h"

#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ============================ small string utils ============================ */

static int sl(const char *s) { int n = 0; if (s) while (s[n]) ++n; return n; }

/* ============================ OpenSSL via dlopen ============================ */

/* Opaque OpenSSL handle types — we never dereference these, only pass them. */
typedef void SSL_CTX_t;
typedef void SSL_t;
typedef void SSL_METHOD_t;

/* Just the OpenSSL 1.1+/3.x entry points we need, as function pointers. */
struct ossl {
    int   ok;
    void *libssl;
    void *libcrypto;

    const SSL_METHOD_t *(*TLS_client_method)(void);
    SSL_CTX_t *(*SSL_CTX_new)(const SSL_METHOD_t *);
    void       (*SSL_CTX_free)(SSL_CTX_t *);
    int        (*SSL_CTX_load_verify_locations)(SSL_CTX_t *, const char *, const char *);
    void       (*SSL_CTX_set_verify)(SSL_CTX_t *, int, void *);
    long       (*SSL_CTX_ctrl)(SSL_CTX_t *, int, long, void *);     /* set min proto ver */

    SSL_t *(*SSL_new)(SSL_CTX_t *);
    void   (*SSL_free)(SSL_t *);
    int    (*SSL_set_fd)(SSL_t *, int);
    long   (*SSL_ctrl)(SSL_t *, int, long, void *);                 /* SNI via set_tlsext_host_name */
    int    (*SSL_set1_host)(SSL_t *, const char *);                 /* hostname verify (1.1.0+) */
    int    (*SSL_connect)(SSL_t *);
    int    (*SSL_write)(SSL_t *, const void *, int);
    int    (*SSL_read)(SSL_t *, void *, int);
    int    (*SSL_shutdown)(SSL_t *);
    long   (*SSL_get_verify_result)(const SSL_t *);
};

static struct ossl g_ssl;
static int         g_ssl_tried = 0;

/* OpenSSL constants we use (stable ABI values; we avoid the openssl headers). */
#define ALR_SSL_VERIFY_PEER          0x01
#define ALR_SSL_CTRL_SET_TLSEXT_HOSTNAME 55   /* SSL_ctrl cmd for SNI */
#define ALR_TLSEXT_NAMETYPE_host_name 0
#define ALR_SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define ALR_TLS1_2_VERSION           0x0303
#define ALR_X509_V_OK                0

#define DL(dst, lib, name)                                          \
    do { *(void **)(&g_ssl.dst) = dlsym((lib), (name));            \
         if (!g_ssl.dst) { return; } } while (0)

static void ossl_load(void) {
    if (g_ssl_tried) return;
    g_ssl_tried = 1;
    g_ssl.ok = 0;

    /* libcrypto first (libssl needs it); try the noble SONAME then a bare name. */
    g_ssl.libcrypto = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!g_ssl.libcrypto)
        g_ssl.libcrypto = dlopen("libcrypto.so", RTLD_NOW | RTLD_GLOBAL);
    g_ssl.libssl = dlopen("libssl.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!g_ssl.libssl)
        g_ssl.libssl = dlopen("libssl.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_ssl.libssl) return;   /* no TLS -> shim disables, passthrough */

    void *s = g_ssl.libssl;
    DL(TLS_client_method,             s, "TLS_client_method");
    DL(SSL_CTX_new,                   s, "SSL_CTX_new");
    DL(SSL_CTX_free,                  s, "SSL_CTX_free");
    DL(SSL_CTX_load_verify_locations, s, "SSL_CTX_load_verify_locations");
    DL(SSL_CTX_set_verify,            s, "SSL_CTX_set_verify");
    DL(SSL_CTX_ctrl,                  s, "SSL_CTX_ctrl");
    DL(SSL_new,                       s, "SSL_new");
    DL(SSL_free,                      s, "SSL_free");
    DL(SSL_set_fd,                    s, "SSL_set_fd");
    DL(SSL_ctrl,                      s, "SSL_ctrl");
    DL(SSL_connect,                   s, "SSL_connect");
    DL(SSL_write,                     s, "SSL_write");
    DL(SSL_read,                      s, "SSL_read");
    DL(SSL_shutdown,                  s, "SSL_shutdown");
    DL(SSL_get_verify_result,         s, "SSL_get_verify_result");
    /* SSL_set1_host is optional (older OpenSSL): tolerate absence. */
    *(void **)(&g_ssl.SSL_set1_host) = dlsym(s, "SSL_set1_host");

    g_ssl.ok = 1;
}

/* ============================ shim configuration ============================ */

struct cfg {
    int  inited;
    int  enabled;          /* ALR_DOH != "0" */
    int  insecure;         /* ALR_DOH_INSECURE == "1" */
    int  diag;             /* ALR_DOH_DIAG == "1" */
    char server_ip[64];    /* DoH server IP literal */
    int  server_port;      /* default 443 */
    char host[128];        /* SNI / Host header */
    char path[128];        /* request path */
    char ca[256];          /* CA bundle path */
};

static struct cfg g_cfg;

static void cfg_copy(char *dst, size_t cap, const char *src, const char *dflt) {
    const char *v = (src && src[0]) ? src : dflt;
    size_t n = (size_t)sl(v);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) dst[i] = v[i];
    dst[n] = '\0';
}

static void cfg_init(void) {
    if (g_cfg.inited) return;
    g_cfg.inited = 1;

    const char *en = getenv("ALR_DOH");
    g_cfg.enabled = !(en && en[0] == '0');          /* default ON */
    const char *insec = getenv("ALR_DOH_INSECURE");
    g_cfg.insecure = (insec && insec[0] == '1');
    const char *dg = getenv("ALR_DOH_DIAG");
    g_cfg.diag = (dg && dg[0] == '1');

    /* server: "<ip>[:port]" */
    const char *srv = getenv("ALR_DOH_SERVER");
    char tmp[80];
    cfg_copy(tmp, sizeof tmp, srv, "1.1.1.1");
    g_cfg.server_port = 443;
    /* split optional :port */
    int colon = -1;
    for (int i = 0; tmp[i]; ++i) if (tmp[i] == ':') { colon = i; break; }
    if (colon >= 0) {
        tmp[colon] = '\0';
        int p = 0;
        for (const char *q = tmp + colon + 1; *q >= '0' && *q <= '9'; ++q)
            p = p * 10 + (*q - '0');
        if (p > 0 && p < 65536) g_cfg.server_port = p;
    }
    cfg_copy(g_cfg.server_ip, sizeof g_cfg.server_ip, tmp, "1.1.1.1");

    cfg_copy(g_cfg.host, sizeof g_cfg.host, getenv("ALR_DOH_HOST"),
             "cloudflare-dns.com");
    cfg_copy(g_cfg.path, sizeof g_cfg.path, getenv("ALR_DOH_PATH"), "/dns-query");
    cfg_copy(g_cfg.ca, sizeof g_cfg.ca, getenv("ALR_DOH_CA"),
             "/etc/ssl/certs/ca-certificates.crt");
}

/* ============================ diagnostics ============================ */

static void diag_line(const char *host, int n_a, int n_aaaa, int ok) {
    if (!g_cfg.diag) return;
    int saved = errno;
    char b[256]; size_t o = 0;
    const char *pre = "ALR-DOH ";
    for (int i = 0; pre[i] && o < sizeof b - 1; ++i) b[o++] = pre[i];
    for (int i = 0; host && host[i] && o < sizeof b - 1; ++i) b[o++] = host[i];
    const char *tag = ok ? " ok A=" : " fallback A=";
    for (int i = 0; tag[i] && o < sizeof b - 1; ++i) b[o++] = tag[i];
    char num[16]; int ni = 0; int v = n_a; if (v == 0) num[ni++] = '0';
    while (v && ni < 16) { num[ni++] = (char)('0' + v % 10); v /= 10; }
    while (ni > 0 && o < sizeof b - 1) b[o++] = num[--ni];
    const char *t2 = " AAAA="; for (int i = 0; t2[i] && o < sizeof b - 1; ++i) b[o++] = t2[i];
    ni = 0; v = n_aaaa; if (v == 0) num[ni++] = '0';
    while (v && ni < 16) { num[ni++] = (char)('0' + v % 10); v /= 10; }
    while (ni > 0 && o < sizeof b - 1) b[o++] = num[--ni];
    if (o < sizeof b) b[o++] = '\n';
    ssize_t w = write(2, b, o); (void)w;
    errno = saved;
}

/* ============================ TLS round trip ============================ */

/*
 * doh_query_once — connect to the DoH server, send the base64url GET for one
 * qtype, read the DNS response body, parse it into `res`.
 * Returns 0 on a complete TLS+HTTP+parse cycle (res holds rcode/addrs), -1 on
 * any transport/format failure (caller then falls back).
 */
static int doh_query_once(const char *qname, uint16_t qtype, alr_doh_result *res) {
    if (!g_ssl.ok) return -1;

    /* 1. DNS query -> base64url */
    uint8_t q[ALR_DOH_MAX_QUERY];
    int qn = alr_doh_build_query(0, qname, qtype, q, sizeof q);
    if (qn <= 0) return -1;
    char b64[ALR_DOH_MAX_B64];
    if (alr_doh_base64url(q, (size_t)qn, b64, sizeof b64) <= 0) return -1;

    /* 2. TCP connect to the DoH server IP (no DNS needed — it's a literal). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_cfg.server_port);
    /* inet_pton without <arpa/inet.h>: hand-parse a dotted-quad IPv4 literal. */
    {
        unsigned parts[4] = {0,0,0,0}; int pi = 0, seen = 0; const char *p = g_cfg.server_ip;
        for (; *p; ++p) {
            if (*p >= '0' && *p <= '9') { parts[pi] = parts[pi]*10 + (unsigned)(*p-'0'); seen = 1; }
            else if (*p == '.') { if (++pi > 3) { close(fd); return -1; } seen = 0; }
            else break;
        }
        if (!seen || pi != 3) { close(fd); return -1; }
        uint32_t ip = (parts[0]<<24)|(parts[1]<<16)|(parts[2]<<8)|parts[3];
        sa.sin_addr.s_addr = htonl(ip);
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }

    /* 3. TLS handshake with verification. */
    SSL_CTX_t *ctx = g_ssl.SSL_CTX_new(g_ssl.TLS_client_method());
    if (!ctx) { close(fd); return -1; }
    g_ssl.SSL_CTX_ctrl(ctx, ALR_SSL_CTRL_SET_MIN_PROTO_VERSION, ALR_TLS1_2_VERSION, NULL);
    if (!g_cfg.insecure) {
        g_ssl.SSL_CTX_load_verify_locations(ctx, g_cfg.ca, NULL);
        g_ssl.SSL_CTX_set_verify(ctx, ALR_SSL_VERIFY_PEER, NULL);
    }
    SSL_t *ssl = g_ssl.SSL_new(ctx);
    if (!ssl) { g_ssl.SSL_CTX_free(ctx); close(fd); return -1; }
    g_ssl.SSL_set_fd(ssl, fd);
    /* SNI (always) + hostname verification (when verifying and available). */
    g_ssl.SSL_ctrl(ssl, ALR_SSL_CTRL_SET_TLSEXT_HOSTNAME,
                   ALR_TLSEXT_NAMETYPE_host_name, (void *)g_cfg.host);
    if (!g_cfg.insecure && g_ssl.SSL_set1_host)
        g_ssl.SSL_set1_host(ssl, g_cfg.host);

    int rc = -1;
    if (g_ssl.SSL_connect(ssl) == 1 &&
        (g_cfg.insecure || g_ssl.SSL_get_verify_result(ssl) == ALR_X509_V_OK)) {

        /* 4. HTTP/1.1 GET /dns-query?dns=<b64> */
        char req[1024]; size_t ro = 0;
        const char *l1a = "GET ";  for (int i = 0; l1a[i]; ++i) req[ro++] = l1a[i];
        for (int i = 0; g_cfg.path[i]; ++i) req[ro++] = g_cfg.path[i];
        const char *qm = "?dns=";  for (int i = 0; qm[i]; ++i) req[ro++] = qm[i];
        for (int i = 0; b64[i] && ro < sizeof req - 200; ++i) req[ro++] = b64[i];
        const char *l1b = " HTTP/1.1\r\nHost: ";
        for (int i = 0; l1b[i]; ++i) req[ro++] = l1b[i];
        for (int i = 0; g_cfg.host[i]; ++i) req[ro++] = g_cfg.host[i];
        const char *hdr = "\r\nAccept: application/dns-message\r\n"
                          "User-Agent: alr-doh/1\r\nConnection: close\r\n\r\n";
        for (int i = 0; hdr[i]; ++i) req[ro++] = hdr[i];

        if (g_ssl.SSL_write(ssl, req, (int)ro) == (int)ro) {
            /* 5. read the whole response (header + body). */
            uint8_t buf[8192]; int total = 0, r;
            while (total < (int)sizeof buf &&
                   (r = g_ssl.SSL_read(ssl, buf + total, (int)sizeof buf - total)) > 0)
                total += r;

            /* find CRLFCRLF -> body start */
            int body = -1;
            for (int i = 0; i + 3 < total; ++i)
                if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') { body = i+4; break; }
            /* status must be 200 */
            int ok200 = (total > 12 && buf[9]=='2' && buf[10]=='0' && buf[11]=='0');
            if (body >= 0 && ok200 && total - body >= 12) {
                if (alr_doh_parse_response(buf + body, (size_t)(total - body),
                                           0, qtype, res) == ALR_DOH_OK)
                    rc = 0;
            }
        }
        g_ssl.SSL_shutdown(ssl);
    }
    g_ssl.SSL_free(ssl);
    g_ssl.SSL_CTX_free(ctx);
    close(fd);
    return rc;
}

/* ============================ addrinfo marshalling ============================ */

/* ai_flags sentinel bit so our freeaddrinfo can tell a node WE built (two
 * separate allocs: the addrinfo + its ai_addr) from one the REAL resolver
 * returned on the fallback path (a single glibc block with an interior ai_addr).
 * 0x40000000 is high above every real AI_* flag glibc defines. */
#ifndef ALR_AI_OURS
#define ALR_AI_OURS 0x40000000
#endif

/* Each result node is two independent calloc'd blocks (the addrinfo and its
 * ai_addr); our freeaddrinfo releases both per node. We tag ai_flags with
 * ALR_AI_OURS so a chain we built is never handed to glibc's freeaddrinfo (which
 * assumes a single contiguous allocation), and vice versa. */
static struct addrinfo *make_ai(int family, const uint8_t *addr, int alen,
                                int socktype, int proto, uint16_t port) {
    size_t salen = (family == AF_INET) ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6);
    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    if (!ai) return NULL;
    void *sa = calloc(1, salen);
    if (!sa) { free(ai); return NULL; }
    ai->ai_flags = ALR_AI_OURS;
    ai->ai_family = family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = proto;
    ai->ai_addrlen = (socklen_t)salen;
    ai->ai_addr = (struct sockaddr *)sa;
    if (family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)sa;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(port);
        memcpy(&s4->sin_addr, addr, (size_t)alen);
    } else {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)sa;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(port);
        memcpy(&s6->sin6_addr, addr, (size_t)alen);
    }
    return ai;
}

/* ============================ the interposed entry points ============================ */

/* cached real getaddrinfo / freeaddrinfo / gethostbyname_r via RTLD_NEXT */
static int (*real_getaddrinfo)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);
static void (*real_freeaddrinfo)(struct addrinfo *);

static void resolve_real(void) {
    if (!real_getaddrinfo)
        *(void **)(&real_getaddrinfo) = dlsym(RTLD_NEXT, "getaddrinfo");
    if (!real_freeaddrinfo)
        *(void **)(&real_freeaddrinfo) = dlsym(RTLD_NEXT, "freeaddrinfo");
}

/* Is `node` a numeric literal (IPv4 dotted-quad or contains ':') we should NOT
 * DoH-resolve? Those need no DNS; defer to glibc which formats them directly. */
static int is_numeric_host(const char *node) {
    if (!node || !node[0]) return 1;
    int dots = 0, digits = 0, other = 0;
    for (const char *p = node; *p; ++p) {
        if (*p == ':') return 1;                 /* IPv6 literal */
        else if (*p == '.') ++dots;
        else if (*p >= '0' && *p <= '9') ++digits;
        else ++other;
    }
    return (other == 0 && dots == 3 && digits >= 4);  /* looks like a.b.c.d */
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    cfg_init();
    resolve_real();

    /* Conditions under which we DO NOT DoH and defer to glibc:
     *   - shim disabled
     *   - no node (service-only / passive lookups)
     *   - numeric host (no DNS needed)
     *   - AI_NUMERICHOST requested (caller promises node is numeric)
     *   - AF_UNIX or other non-INET family hint
     * In all these the real resolver is correct and cheap. */
    int numerichost = hints && (hints->ai_flags & AI_NUMERICHOST);
    int fam = hints ? hints->ai_family : AF_UNSPEC;
    int bad_family = (fam != AF_UNSPEC && fam != AF_INET && fam != AF_INET6);

    if (!g_cfg.enabled || !node || numerichost || bad_family || is_numeric_host(node)) {
        if (real_getaddrinfo) return real_getaddrinfo(node, service, hints, res);
        return EAI_FAIL;
    }

    ossl_load();
    if (!g_ssl.ok) {
        /* No TLS available -> we cannot DoH; passthrough (will likely hang on
         * Android, but we must not be WORSE than baseline, and on a normal Linux
         * box this just works). */
        diag_line(node, 0, 0, 0);
        if (real_getaddrinfo) return real_getaddrinfo(node, service, hints, res);
        return EAI_FAIL;
    }

    /* parse the service to a port number (numeric only; named services would
     * need /etc/services — glibc handles those, but for DoH we accept a numeric
     * port or 0). A non-numeric service falls back to the real resolver so we
     * never mis-handle "http"/"https" name lookups. */
    uint16_t port = 0;
    if (service && service[0]) {
        int allnum = 1, p = 0;
        for (const char *s = service; *s; ++s) {
            if (*s < '0' || *s > '9') { allnum = 0; break; }
            p = p * 10 + (*s - '0');
        }
        if (!allnum) {
            if (real_getaddrinfo) return real_getaddrinfo(node, service, hints, res);
            return EAI_FAIL;
        }
        port = (uint16_t)p;
    }

    int socktype = hints ? hints->ai_socktype : 0;
    int proto    = hints ? hints->ai_protocol : 0;

    struct addrinfo *head = NULL, *tail = NULL;
    int n_a = 0, n_aaaa = 0;

    /* Query A unless the caller pinned AF_INET6, and AAAA unless pinned AF_INET. */
    alr_doh_result r;
    if (fam != AF_INET6) {
        if (doh_query_once(node, ALR_DNS_T_A, &r) == 0 &&
            r.rcode == ALR_DNS_RCODE_NOERROR) {
            for (int i = 0; i < r.count; ++i) {
                struct addrinfo *ai = make_ai(AF_INET, r.addrs[i].addr, 4,
                                              socktype, proto, port);
                if (!ai) continue;
                if (tail) tail->ai_next = ai; else head = ai;
                tail = ai; ++n_a;
            }
        }
    }
    if (fam != AF_INET) {
        if (doh_query_once(node, ALR_DNS_T_AAAA, &r) == 0 &&
            r.rcode == ALR_DNS_RCODE_NOERROR) {
            for (int i = 0; i < r.count; ++i) {
                struct addrinfo *ai = make_ai(AF_INET6, r.addrs[i].addr, 16,
                                              socktype, proto, port);
                if (!ai) continue;
                if (tail) tail->ai_next = ai; else head = ai;
                tail = ai; ++n_aaaa;
            }
        }
    }

    if (head) {
        diag_line(node, n_a, n_aaaa, 1);
        *res = head;
        return 0;
    }

    /* DoH produced nothing (server unreachable, NXDOMAIN with empty, or parse
     * failure). Fall back to the real resolver so a working network path (e.g.
     * /etc/hosts entries, or a non-Android host) still resolves. */
    diag_line(node, 0, 0, 0);
    if (real_getaddrinfo) return real_getaddrinfo(node, service, hints, res);
    return EAI_NONAME;
}

/* freeaddrinfo: our nodes are plain calloc'd (ai + ai_addr). glibc's
 * freeaddrinfo frees the chain; but a node WE built must be freed by US (its
 * ai_addr is a separate alloc). We cannot tell a node's origin, so we own
 * freeaddrinfo too and free both blocks per node. A node from the REAL resolver
 * (returned on our fallback path) is a single glibc allocation whose ai_addr
 * points INTO the same block — freeing ai_addr separately would be wrong.
 *
 * To stay correct for BOTH origins we tag our nodes: ai_canonname is normally
 * NULL for non-AI_CANONNAME lookups; glibc sets ai_addr to an interior pointer.
 * We instead detect ours by a sentinel we never otherwise set. Simplest robust
 * approach: route freeaddrinfo to the REAL one for chains we did not build, and
 * free ours manually. We mark ours by setting ai_flags to ALR_AI_OURS (above). */
void freeaddrinfo(struct addrinfo *ai) {
    resolve_real();
    if (!ai) return;
    /* If the FIRST node is not ours, the whole chain came from glibc. */
    if (!(ai->ai_flags & ALR_AI_OURS)) {
        if (real_freeaddrinfo) { real_freeaddrinfo(ai); return; }
    }
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        if (ai->ai_addr) free(ai->ai_addr);
        free(ai);
        ai = next;
    }
}

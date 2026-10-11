/* alr_resolvd — Termux-side (bionic) resolver for the guest.
 *
 * Runs as a thread inside `alr`, serving a Unix socket the guest's preload
 * connects to.  Because this side is bionic, getaddrinfo() goes through netd
 * and therefore honours Private DNS (DoT), VPNs, and per-network resolvers --
 * all of which the guest's glibc resolver cannot see (docs/RISKS.md R15).
 *
 * The thread must never call waitpid(): the supervisor's main loop owns child
 * reaping and uses __WNOTHREAD, so a stray waiter here would steal its stops.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "alr_resolv_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_listen = -1;
static char g_path[108];

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* bionic and glibc do not agree on EAI_* numbering.  Everything the guest can
 * act on collapses to these three, so map rather than pass through: a glibc
 * caller comparing against its own EAI_NONAME would otherwise mis-branch. */
#define GLIBC_EAI_NONAME  (-2)
#define GLIBC_EAI_AGAIN   (-3)
#define GLIBC_EAI_FAIL    (-4)

static int32_t map_eai(int rc)
{
    switch (rc) {
    case 0:          return 0;
    case EAI_NONAME: return GLIBC_EAI_NONAME;
    case EAI_AGAIN:  return GLIBC_EAI_AGAIN;
    default:         return GLIBC_EAI_FAIL;
    }
}

/* netd's DNS proxy, spoken directly.
 *
 * bionic's getaddrinfo() reaches netd through libnetd_client.so, which libc
 * dlopen()s at start-up.  alr is linked static (Makefile, ALR_API) -- a
 * dynamic alr paid the bionic linker plus that dlopen chain on every
 * `alr run`, ~9 ms on the reference device -- and a static libc cannot
 * dlopen, so its getaddrinfo() falls back to /etc/resolv.conf, which Android
 * does not have: every lookup failed.  This is the request bionic's
 * android_getaddrinfo_proxy() sends (FrameworkListener text command, NUL
 * terminated) and the reply it parses: a 4-byte ASCII code, 222 on success,
 * then per address a big-endian "more" word, flags, family, socktype,
 * protocol, address length, the address, and a canonical-name length plus
 * name.  Going through netd keeps Private DNS, VPNs and per-network
 * resolvers, the reason resolvd exists (docs/RISKS.md R15).
 *
 * Returns a bionic EAI_* code, or -1 when the proxy could not be used (the
 * caller then asks libc). */
#define DNSPROXY_SOCK       "/dev/socket/dnsproxyd"
#define DNSPROXY_RESULT_OK  222

static int read_be32(int fd, int32_t *out)
{
    uint32_t v;
    if (read_full(fd, &v, sizeof v) != 0) return -1;
    *out = (int32_t)ntohl(v);
    return 0;
}

static int proxy_getaddrinfo(const char *node, const char *serv,
                             const struct addrinfo *h,
                             struct alr_resolv_ent *ents, uint32_t *n_out)
{
    struct sockaddr_un sa;
    char cmd[64 + ALR_RESOLV_MAXNAME + ALR_RESOLV_MAXSERV];
    char code[4];
    const char *c;
    int fd, len, ret = -1;
    uint32_t n = 0;

    /* the command is space-separated: such a name cannot be sent */
    for (c = node; *c; c++) if (*c == ' ') return EAI_NONAME;
    for (c = serv ? serv : ""; *c; c++) if (*c == ' ') return EAI_SERVICE;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, DNSPROXY_SOCK, sizeof DNSPROXY_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) goto out;

    /* netid 0 = NETID_UNSET: netd picks this uid's default network. */
    len = snprintf(cmd, sizeof cmd, "getaddrinfo %s %s %d %d %d %d %u",
                   node, serv && *serv ? serv : "^", h->ai_flags, h->ai_family,
                   h->ai_socktype, h->ai_protocol, 0u);
    if (len < 0 || (size_t)len >= sizeof cmd) goto out;
    if (write_full(fd, cmd, (size_t)len + 1) != 0) goto out;   /* with the NUL */
    if (read_full(fd, code, sizeof code) != 0) goto out;
    code[3] = '\0';
    if (atoi(code) != DNSPROXY_RESULT_OK) {
        int32_t e = 0;
        ret = read_full(fd, &e, sizeof e) == 0 && e != 0 ? (int)e : EAI_FAIL;
        goto out;
    }
    for (;;) {
        int32_t more, flags, fam, st, proto, alen, nlen;
        unsigned char addr[128];
        char skip[256];
        if (read_be32(fd, &more) != 0) goto out;
        if (!more) break;
        if (read_be32(fd, &flags) || read_be32(fd, &fam) || read_be32(fd, &st)
            || read_be32(fd, &proto) || read_be32(fd, &alen))
            goto out;
        if (alen < 0 || alen > (int32_t)sizeof addr) goto out;
        if (read_full(fd, addr, (size_t)alen) != 0) goto out;
        if (read_be32(fd, &nlen) != 0 || nlen < 0) goto out;
        while (nlen > 0) {                       /* canonical name: unused */
            int k = nlen > (int32_t)sizeof skip ? (int)sizeof skip : nlen;
            if (read_full(fd, skip, (size_t)k) != 0) goto out;
            nlen -= k;
        }
        (void)flags;
        if (n < ALR_RESOLV_MAXRES && alen <= ALR_RESOLV_MAXADDR) {
            memset(&ents[n], 0, sizeof ents[n]);
            ents[n].family   = fam;
            ents[n].socktype = st;
            ents[n].protocol = proto;
            ents[n].addrlen  = (uint32_t)alen;
            memcpy(ents[n].addr, addr, (size_t)alen);
            n++;
        }
    }
    *n_out = n;
    ret = 0;
out:
    close(fd);
    return ret;
}

/* Names that need no DNS: libc answers these itself, static or not. */
static int needs_dns(const char *node, int flags)
{
    unsigned char b[16];
    if (!node || !*node || (flags & AI_NUMERICHOST)) return 0;
    return inet_pton(AF_INET, node, b) != 1 && inet_pton(AF_INET6, node, b) != 1;
}

static void serve_one(int fd)
{
    struct alr_resolv_req rq;
    struct alr_resolv_rsp rs;
    char node[ALR_RESOLV_MAXNAME + 1];
    char serv[ALR_RESOLV_MAXSERV + 1];
    struct addrinfo hints, *res = NULL, *ai;
    struct alr_resolv_ent ents[ALR_RESOLV_MAXRES];
    uint32_t n = 0;
    int rc;

    memset(&rq, 0, sizeof rq);
    if (read_full(fd, &rq, sizeof rq) != 0) return;
    if (rq.magic != ALR_RESOLV_MAGIC) return;
    if (rq.node_len > ALR_RESOLV_MAXNAME || rq.serv_len > ALR_RESOLV_MAXSERV) return;

    node[0] = serv[0] = '\0';
    if (rq.node_len && read_full(fd, node, rq.node_len) != 0) return;
    if (rq.serv_len && read_full(fd, serv, rq.serv_len) != 0) return;
    node[rq.node_len] = '\0';
    serv[rq.serv_len] = '\0';

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = rq.family;
    hints.ai_socktype = rq.socktype;
    hints.ai_protocol = rq.protocol;
    hints.ai_flags    = rq.flags;

    rc = -1;
    if (rq.has_node && needs_dns(node, hints.ai_flags))
        rc = proxy_getaddrinfo(node, rq.has_serv ? serv : NULL, &hints, ents, &n);
    if (rc < 0) {
        n = 0;
        rc = getaddrinfo(rq.has_node ? node : NULL,
                         rq.has_serv ? serv : NULL, &hints, &res);
    }

    for (ai = res; ai && n < ALR_RESOLV_MAXRES; ai = ai->ai_next) {
        if (!ai->ai_addr || ai->ai_addrlen > ALR_RESOLV_MAXADDR) continue;
        memset(&ents[n], 0, sizeof ents[n]);
        ents[n].family   = ai->ai_family;
        ents[n].socktype = ai->ai_socktype;
        ents[n].protocol = ai->ai_protocol;
        ents[n].addrlen  = (uint32_t)ai->ai_addrlen;
        memcpy(ents[n].addr, ai->ai_addr, ai->ai_addrlen);
        n++;
    }
    if (res) freeaddrinfo(res);

    rs.magic = ALR_RESOLV_MAGIC;
    rs.rc    = map_eai(rc);
    rs.count = n;
    /* A success with zero usable entries would leave the guest with a NULL
     * result and rc==0, which crashes naive callers.  Report NONAME. */
    if (rs.rc == 0 && n == 0) rs.rc = GLIBC_EAI_NONAME;

    if (write_full(fd, &rs, sizeof rs) != 0) return;
    if (n) write_full(fd, ents, n * sizeof ents[0]);
}

static void *loop(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        serve_one(fd);
        close(fd);
    }
    return NULL;
}

/* Start the resolver.  Returns the socket path on success (to be exported as
 * ALR_RESOLV_SOCK), or NULL if it could not start -- in which case the guest
 * simply falls back to glibc's own resolver, which works on devices without
 * Private DNS or a VPN. */
const char *alr_resolvd_start(const char *dir)
{
    struct sockaddr_un sa;
    pthread_t th;
    pthread_attr_t at;

    snprintf(g_path, sizeof g_path, "%s/.alr-resolv-%d.sock", dir, (int)getpid());
    if (strlen(g_path) >= sizeof sa.sun_path) return NULL;   /* 108-byte limit */

    unlink(g_path);
    g_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen < 0) return NULL;

    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, g_path, strlen(g_path) + 1);
    if (bind(g_listen, (struct sockaddr *)&sa, sizeof sa) != 0) goto fail;
    if (listen(g_listen, 16) != 0) goto fail;

    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, loop, NULL) != 0) { pthread_attr_destroy(&at); goto fail; }
    pthread_attr_destroy(&at);
    return g_path;

fail:
    close(g_listen);
    g_listen = -1;
    unlink(g_path);
    return NULL;
}

void alr_resolvd_stop(void)
{
    if (g_listen >= 0) { close(g_listen); g_listen = -1; }
    if (g_path[0]) unlink(g_path);
}

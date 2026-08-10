/* getaddrinfo()'s SERVICE argument, which was dropped on the floor.
 *
 * hosts_addrinfo() hardcoded port 0 and was never passed the service at all,
 * so every guest client resolving a name out of /etc/hosts got port 0 and an
 * instant ECONNREFUSED: `curl localhost:8080`, psql -h localhost, redis-cli,
 * python's socket.create_connection, every dev server anyone runs in a guest.
 *
 * It hid because the BRIDGE path (alr_resolvd) carries the service correctly.
 * Only names answered from /etc/hosts were affected -- and "localhost" is the
 * most common name there is.
 *
 * Three cases, because two of them broke separately:
 *   numeric   "8080" -> 8080.  Plain parse.
 *   named     "http" -> 80.    Needs the GUEST's /etc/services: glibc's
 *                              getservbyname reads the file through an
 *                              internal call LD_PRELOAD cannot see, so the
 *                              first fix still returned 0 here.
 *   absent    NULL   -> 0.     The one case where 0 is the right answer, and
 *                              the control that keeps the other two honest.
 */
#define _GNU_SOURCE
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static int port_of(const char *serv, int *out)
{
    struct addrinfo h, *r = NULL;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", serv, &h, &r) != 0 || !r) return -1;
    *out = ntohs(((struct sockaddr_in *)r->ai_addr)->sin_port);
    freeaddrinfo(r);
    return 0;
}

int main(void)
{
    int n = -1, m = -1, z = -1, bad = 0;
    if (port_of("8080", &n) != 0 || n != 8080) bad++;
    if (port_of("http", &m) != 0 || m != 80)   bad++;
    if (port_of(NULL,   &z) != 0 || z != 0)    bad++;
    printf("numeric=%d named=%d none=%d\n", n, m, z);
    return bad ? 1 : 0;
}

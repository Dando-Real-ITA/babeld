/*
Copyright (c) 2008 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "babeld.h"
#include "interface.h"
#include "source.h"
#include "neighbour.h"
#include "kernel.h"
#include "xroute.h"
#include "route.h"
#include "message.h"
#include "util.h"
#include "configuration.h"
#include "local.h"
#include "version.h"

int local_server_socket = -1;
struct local_socket local_sockets[MAX_LOCAL_SOCKETS];
int num_local_sockets = 0;
int local_server_port = -1;
char *local_server_path;
int local_server_write = 0;

struct local_dump_route_counters {
    int ipv4_inst;
    int ipv4_not_inst;
    int ipv6_inst;
    int ipv6_not_inst;
};

static int
local_dump_prefix_is_ipv4(const unsigned char *prefix, unsigned char plen)
{
    return plen >= 96 && v4mapped(prefix);
}

static int
local_xroute_generic_metric(const struct xroute *xroute)
{
    return MIN((int)xroute->metric +
               output_filter(NULL,
                             xroute->prefix, xroute->plen,
                             xroute->src_prefix, xroute->src_plen,
                             0),
               INFINITY);
}

static void
local_count_dump_route(struct local_dump_route_counters *counters,
                       const unsigned char *prefix, unsigned char plen,
                       int installed)
{
    if(local_dump_prefix_is_ipv4(prefix, plen)) {
        if(installed)
            counters->ipv4_inst++;
        else
            counters->ipv4_not_inst++;
    } else {
        if(installed)
            counters->ipv6_inst++;
        else
            counters->ipv6_not_inst++;
    }
}

static const char *
local_dump_timestamp(time_t when, char *buf, size_t len)
{
    struct tm tm;
    size_t written;

    if(localtime_r(&when, &tm) == NULL)
        goto fallback;

    written = strftime(buf, len, "%Y-%m-%d %H:%M:%S %Z", &tm);
    if(written > 0)
        return buf;

 fallback:
    snprintf(buf, len, "%ld", (long)when);
    return buf;
}

static int
write_timeout(int fd, const void *buf, int len)
{
    int n = 0, rc = 0;
    const char *b = buf;

    while(n < len) {
        rc = write(fd, b + n, len - n);
        if(rc < 0) {
            if(errno == EAGAIN || errno == EINTR) {
                rc = wait_for_fd(1, fd, 100);
                if(rc > 0) {
                    rc = write(fd, b + n, len - n);
                }
            }
        }
        if(rc > 0)
            n += rc;
        else
            break;
    }

    if(n >= len)
        return 1;
    else {
        if(rc >= 0)
            errno = EAGAIN;
        return -1;
    }
}

const char *
local_kind(int kind)
{
    switch(kind) {
    case LOCAL_FLUSH: return "flush";
    case LOCAL_CHANGE: return "change";
    case LOCAL_ADD: return "add";
    default: return "???";
    }
}

static void
local_notify_interface_1(struct local_socket *s,
                         struct interface *ifp, int kind)
{
    char buf[512], v4[INET_ADDRSTRLEN];
    int rc;
    int up;

    up = if_up(ifp);
    if(up && ifp->ipv4)
        inet_ntop(AF_INET, ifp->ipv4, v4, INET_ADDRSTRLEN);
    else
        v4[0] = '\0';
    if(up)
        rc = snprintf(buf, 512,
                      "%s interface %s up true%s%s%s%s\n",
                      local_kind(kind), ifp->name,
                      ifp->numll > 0 ? " ipv6 " : "",
                      ifp->numll > 0 ? format_address(ifp->ll[0]) : "",
                      v4[0] ? " ipv4 " : "", v4);
    else
        rc = snprintf(buf, 512, "%s interface %s up false\n",
                      local_kind(kind), ifp->name);

    if(rc < 0 || rc >= 512)
        goto fail;

    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;
    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

void
local_notify_interface(struct interface *ifp, int kind)
{
    int i;
    for(i = 0; i < num_local_sockets; i++) {
        if(local_sockets[i].monitor)
            local_notify_interface_1(&local_sockets[i], ifp, kind);
    }
}

static void
local_notify_neighbour_1(struct local_socket *s,
                         struct neighbour *neigh, int kind)
{
    char buf[512], rttbuf[64];
    int rc;

    rttbuf[0] = '\0';
    if(valid_rtt(neigh)) {
        rc = snprintf(rttbuf, 64, " rtt %s rttcost %u",
                      format_thousands(neigh->rtt), neighbour_rttcost(neigh));
        if(rc < 0 || rc >= 64)
            rttbuf[0] = '\0';
    }

    rc = snprintf(buf, 512,
                  "%s neighbour %lx address %s "
                  "if %s reach %04x ureach %04x "
                  "rxcost %u txcost %u%s cost %u\n",
                  local_kind(kind),
                  /* Neighbours never move around in memory , so we can use the
                     address as a unique identifier. */
                  (unsigned long int)neigh,
                  format_address(neigh->address),
                  neigh->ifp->name,
                  neigh->hello.reach,
                  neigh->uhello.reach,
                  neighbour_rxcost(neigh),
                  neighbour_txcost(neigh),
                  rttbuf,
                  neighbour_cost(neigh));

    if(rc < 0 || rc >= 512)
        goto fail;

    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;
    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

void
local_notify_neighbour(struct neighbour *neigh, int kind)
{
    int i;
    for(i = 0; i < num_local_sockets; i++) {
        if(local_sockets[i].monitor)
            local_notify_neighbour_1(&local_sockets[i], neigh, kind);
    }
}

static void
local_notify_xroute_1(struct local_socket *s, struct xroute *xroute, int kind,
                      struct local_dump_route_counters *xroute_counters)
{
    char buf[1024], metrics_buf[384];
    int rc;
    int metric;
    const char *dst_prefix = format_prefix(xroute->prefix,
                                           xroute->plen);
    const char *src_prefix = format_prefix(xroute->src_prefix,
                                           xroute->src_plen);

    metric = local_xroute_generic_metric(xroute);
    if(xroute_counters)
        local_count_dump_route(xroute_counters,
                               xroute->prefix, xroute->plen,
                               metric < INFINITY);

    rc = format_xroute_metrics(xroute, metrics_buf, sizeof(metrics_buf));
    if(rc < 0) {
        rc = snprintf(metrics_buf, sizeof(metrics_buf),
                  "metric-generic %d", metric);
        if(rc < 0 || rc >= (int)sizeof(metrics_buf))
            goto fail;
    }

    rc = snprintf(buf, sizeof(buf),
                  "%s xroute %s-%s prefix %s from %s %s table %d\n",
                  local_kind(kind), dst_prefix, src_prefix,
                  dst_prefix, src_prefix, metrics_buf, xroute->table);

    if(rc < 0 || rc >= (int)sizeof(buf))
        goto fail;

    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;
    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

void
local_notify_xroute(struct xroute *xroute, int kind)
{
    int i;
    for(i = 0; i < num_local_sockets; i++) {
        if(local_sockets[i].monitor)
            local_notify_xroute_1(&local_sockets[i], xroute, kind, NULL);
    }
}

static void
local_notify_route_1(struct local_socket *s, struct babel_route *route, int kind)
{
    char buf[640], tables_buf[384], weight_buf[32];
    int rc, i;
    int weight;
    const unsigned char *via = zeroes;
    const char *ifname = "(none)";
    const char *dst_prefix = format_prefix(route->src->prefix,
                                           route->src->plen);
    const char *src_prefix = format_prefix(route->src->src_prefix,
                                           route->src->src_plen);

    /* Build tables string from array */
    tables_buf[0] = '\0';
    if(route->installed_table_count > 0) {
        int written = 0;
        for(i = 0; i < route->installed_table_count && written < 370; i++) {
            int len = snprintf(tables_buf + written, 384 - written, 
                             "%s%u", i > 0 ? "," : "", 
                             route->installed_tables[i]);
            if(len > 0)
                written += len;
        }
    }

    weight = route_ecmp_weight(route);
    if(weight > 0)
        snprintf(weight_buf, sizeof(weight_buf), "%d", weight);
    else
        snprintf(weight_buf, sizeof(weight_buf), "-");

    if(route->neigh && route->neigh->ifp) {
        via = route->neigh->address;
        ifname = route->neigh->ifp->name;
    }

    rc = snprintf(buf, sizeof(buf),
                  "%s route %lx prefix %s from %s installed %s ecmp %s tables %s "
                  "id %s metric %d refmetric %d installed_rank %d weight %s via %s if %s\n",
                  local_kind(kind),
                  (unsigned long)route,
                  dst_prefix, src_prefix,
                  route->installed ? "yes" : "no",
                  route_ecmp_mode(multipath_ecmp),
                  tables_buf,
                  format_eui64(route->src->id),
                  route_metric(route), route->refmetric,
                  route->installed,
                  weight_buf,
                  format_address(via),
                  ifname);

    if(rc < 0 || rc >= (int)sizeof(buf))
        goto fail;

    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;
    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

void
local_notify_route(struct babel_route *route, int kind)
{
    int i;
    for(i = 0; i < num_local_sockets; i++) {
        if(local_sockets[i].monitor)
            local_notify_route_1(&local_sockets[i], route, kind);
    }
}

static void
local_notify_status_1(struct local_socket *s, int kind)
{
    char buf[512], time_buf[64];
    int rc;
    time_t when;

    when = time(NULL);

    rc = snprintf(buf, sizeof(buf),
                  "%s daemon version %s ecmp %s window %d my-id %s my-seqno %u at %s\n",
                  local_kind(kind), BABELD_VERSION,
                  route_ecmp_mode(multipath_ecmp), ecmp_metric_window,
                  format_eui64(myid), (unsigned int)myseqno,
                  local_dump_timestamp(when, time_buf, sizeof(time_buf)));
    if(rc < 0 || rc >= (int)sizeof(buf))
        goto fail;

    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;
    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

static void
local_notify_all_1(struct local_socket *s)
{
    struct interface *ifp;
    struct neighbour *neigh;
    struct xroute_stream *xroutes;
    struct route_stream *routes;
    struct local_dump_route_counters xroute_counters = {0, 0, 0, 0};
    struct local_dump_route_counters route_counters = {0, 0, 0, 0};
    char buf[256];
    int rc;
    int interface_count = 0;
    int neighbour_count = 0;

    local_notify_status_1(s, LOCAL_ADD);

    FOR_ALL_INTERFACES(ifp) {
        interface_count++;
        local_notify_interface_1(s, ifp, LOCAL_ADD);
    }

    FOR_ALL_NEIGHBOURS(neigh) {
        neighbour_count++;
        local_notify_neighbour_1(s, neigh, LOCAL_ADD);
    }

    xroutes = xroute_stream();
    if(xroutes) {
        while(1) {
            struct xroute *xroute = xroute_stream_next(xroutes);
            if(xroute == NULL)
                break;
            local_notify_xroute_1(s, xroute, LOCAL_ADD, &xroute_counters);
        }
        xroute_stream_done(xroutes);
    }

    routes = route_stream(0);
    if(routes) {
        while(1) {
            struct babel_route *route = route_stream_next(routes);
            if(route == NULL)
                break;
            local_count_dump_route(&route_counters,
                                   route->src->prefix, route->src->plen,
                                   route->installed == 1);
            local_notify_route_1(s, route, LOCAL_ADD);
        }
        route_stream_done(routes);
    }

    rc = snprintf(buf, sizeof(buf),
                  "add counters interfaces %d neighbours %d\n",
                  interface_count, neighbour_count);
    if(rc < 0 || rc >= (int)sizeof(buf) || write_timeout(s->fd, buf, rc) < 0)
        goto fail;

    rc = snprintf(buf, sizeof(buf),
                  "add counters xroutes ipv4 exported %d ipv4 not exported %d "
                  "ipv6 exported %d ipv6 not exported %d\n",
                  xroute_counters.ipv4_inst,
                  xroute_counters.ipv4_not_inst,
                  xroute_counters.ipv6_inst,
                  xroute_counters.ipv6_not_inst);
    if(rc < 0 || rc >= (int)sizeof(buf) || write_timeout(s->fd, buf, rc) < 0)
        goto fail;

    rc = snprintf(buf, sizeof(buf),
                  "add counters routes ipv4 inst %d ipv4 not inst %d "
                  "ipv6 inst %d ipv6 not inst %d\n",
                  route_counters.ipv4_inst, route_counters.ipv4_not_inst,
                  route_counters.ipv6_inst, route_counters.ipv6_not_inst);
    if(rc < 0 || rc >= (int)sizeof(buf) || write_timeout(s->fd, buf, rc) < 0)
        goto fail;

    return;

 fail:
    shutdown(s->fd, 1);
    return;
}

int
local_read(struct local_socket *s)
{
    int rc, n;
    char *eol;
    char reply[100] = "ok\n";
    const char *message = NULL;

    if(s->buf == NULL)
        s->buf = malloc(LOCAL_BUFSIZE);
    if(s->buf == NULL)
        return -1;

    if(s->n >= LOCAL_BUFSIZE) {
        errno = ENOSPC;
        goto fail;
    }

    rc = read(s->fd, s->buf + s->n, LOCAL_BUFSIZE - s->n);
    if(rc <= 0)
        return rc;
    s->n += rc;

    while(s->n > 0) {
        eol = memchr(s->buf, '\n', s->n);
        if(eol == NULL)
            break;
        n = eol + 1 - s->buf;

        rc = parse_config_from_string(s->buf, n, &message);
        switch(rc) {
        case CONFIG_ACTION_DONE:
            break;
        case CONFIG_ACTION_QUIT:
            shutdown(s->fd, 1);
            reply[0] = '\0';
            break;
        case CONFIG_ACTION_DUMP:
            local_notify_all_1(s);
            break;
        case CONFIG_ACTION_MONITOR:
            local_notify_all_1(s);
            s->monitor = 1;
            break;
        case CONFIG_ACTION_UNMONITOR:
            s->monitor = 0;
            break;
        case CONFIG_ACTION_NO:
            snprintf(reply, sizeof(reply), "no%s%s\n",
                     message ? " " : "", message ? message : "");
            break;
        case CONFIG_ACTION_CHECK_XROUTES:
            rc = check_xroutes(1, 0, 1);
            if(rc < 0) {
                snprintf(reply, sizeof(reply), "Warning: couldn't check exported routes.\n");
            }
            break;
        default:
            snprintf(reply, sizeof(reply), "bad\n");
        }

        if(reply[0] != '\0') {
            rc = write_timeout(s->fd, reply, strlen(reply));
            if(rc < 0) {
                goto fail;
            }
        }
        if(s->n > n)
            memmove(s->buf, s->buf + n, s->n - n);
        s->n -= n;
    }

    if(s->n == 0) {
        free(s->buf);
        s->buf = NULL;
    }

    return 1;

 fail:
    shutdown(s->fd, 1);
    return -1;
}

int
local_header(struct local_socket *s)
{
    char buf[512], host[64];
    int rc;

    rc = gethostname(host, 64);
    if(rc < 0)
        strncpy(host, "alamakota", 64);

    rc = snprintf(buf, 512,
                  "BABEL 1.0\nversion %s\necmp %s\necmp-metric-window %d\nhost %s\nmy-id %s\nmy-seqno %u\nok\n",
                  BABELD_VERSION, route_ecmp_mode(multipath_ecmp),
                  ecmp_metric_window, host, format_eui64(myid),
                  (unsigned int)myseqno);
    if(rc < 0 || rc >= 512)
        goto fail;
    rc = write_timeout(s->fd, buf, rc);
    if(rc < 0)
        goto fail;

    return 1;

 fail:
    shutdown(s->fd, 1);
    return -1;
}

struct local_socket *
local_socket_create(int fd)
{
    if(num_local_sockets >= MAX_LOCAL_SOCKETS)
        return NULL;

    memset(&local_sockets[num_local_sockets], 0, sizeof(struct local_socket));
    local_sockets[num_local_sockets].fd = fd;
    num_local_sockets++;

    return &local_sockets[num_local_sockets - 1];
}

void
local_socket_destroy(int i)
{
    if(i < 0 || i >= num_local_sockets) {
        fprintf(stderr, "Internal error: closing unknown local socket.\n");
        return;
    }

    free(local_sockets[i].buf);
    close(local_sockets[i].fd);
    local_sockets[i] = local_sockets[--num_local_sockets];
    VALGRIND_MAKE_MEM_UNDEFINED(local_sockets + num_local_sockets,
                                sizeof(struct local_socket));
}

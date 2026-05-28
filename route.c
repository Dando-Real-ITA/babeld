/*
Copyright (c) 2007-2011 by Juliusz Chroboczek

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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <sys/time.h>

#include "babeld.h"
#include "util.h"
#include "kernel.h"
#include "interface.h"
#include "source.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"
#include "message.h"
#include "resend.h"
#include "configuration.h"
#include "local.h"

struct babel_route **routes = NULL;
static int max_route_slots = 0;
int route_slots = 0;
int kernel_metric = 0, reflect_kernel_metric = 0;
int allow_duplicates = -1;
int has_duplicate_default = 0;
int multipath_ecmp = 0;
int ecmp_metric_window = DEFAULT_ECMP_METRIC_WINDOW;

int smoothing_half_life = 0;
int two_to_the_one_over_hl = 0; /* 2^(1/hl) * 0x10000 */
static int kernel_route_operation_depth = 0;

int
kernel_route_operation_in_progress(void)
{
    return kernel_route_operation_depth > 0;
}

const char *
route_ecmp_mode(int ecmp_mode)
{
    switch(ecmp_mode) {
    case ECMP_EQUAL:
        return "equal";
    case ECMP_WEIGHT:
        return "weight";
    case ECMP_DISABLED:
    default:
        return "disabled";
    }
}

/* We maintain a list of "slots", ordered by prefix.  Every slot
   contains a linked list of the routes to this prefix, with the
   installed route, if any, at the head of the list. */

int
route_compare(const unsigned char *id,
              const unsigned char *prefix, unsigned char plen,
              const unsigned char *src_prefix, unsigned char src_plen,
              struct babel_route *route)
{
    int i;
    int is_ss = !is_default(src_prefix, src_plen);
    int is_ss_rt = !is_default(route->src->src_prefix, route->src->src_plen);

    /* Put all source-specific routes in the front of the list. */
    if(!is_ss && is_ss_rt) {
        return 1;
    } else if(is_ss && !is_ss_rt) {
        return -1;
    }

    i = memcmp(prefix, route->src->prefix, 16);
    if(i != 0)
        return i;

    if(plen < route->src->plen)
        return -1;
    if(plen > route->src->plen)
        return 1;

    if(is_ss) {
        i = memcmp(src_prefix, route->src->src_prefix, 16);
        if(i != 0)
            return i;
        if(src_plen < route->src->src_plen)
            return -1;
        if(src_plen > route->src->src_plen)
            return 1;
    }

    /* With has_duplicate_default, put default route in a slot by id */
    if(has_duplicate_default &&
       is_default(prefix, plen) &&
       is_default(route->src->prefix, route->src->plen) &&
       id) {
        i = memcmp(id, route->src->id, 8);
        if(i != 0)
            return i;
    }

    return 0;
}

/* Performs binary search, returns -1 in case of failure.  In the latter
   case, new_return is the place where to insert the new element. */

int
find_route_slot(const unsigned char *id,
                const unsigned char *prefix, unsigned char plen,
                const unsigned char *src_prefix, unsigned char src_plen,
                int *new_return)
{
    int p, m, g, c;

    if(route_slots < 1) {
        if(new_return)
            *new_return = 0;
        return -1;
    }

    p = 0; g = route_slots - 1;

    do {
        m = (p + g) / 2;
        c = route_compare(id, prefix, plen, src_prefix, src_plen, routes[m]);
        if(c == 0)
            return m;
        else if(c < 0)
            g = m - 1;
        else
            p = m + 1;
    } while(p <= g);

    if(new_return)
        *new_return = p;

    return -1;
}

struct babel_route *
find_route(const unsigned char *id,
           const unsigned char *prefix, unsigned char plen,
           const unsigned char *src_prefix, unsigned char src_plen,
           struct neighbour *neigh)
{
    struct babel_route *head;
    struct babel_route *route;
    int i = find_route_slot(id, prefix, plen, src_prefix, src_plen, NULL);

    if(i < 0)
        return NULL;

    head = routes[i];
    while(head) {
        route = head;
        while(route) {
            if(route->neigh == neigh)
                return route;
            route = route->multipath;
        }
        head = head->next;
    }

    return NULL;
}

static int
route_metric_distance(const struct babel_route *a, const struct babel_route *b)
{
    int am = route_metric(a);
    int bm = route_metric(b);
    int d;

    if(am >= INFINITY || bm >= INFINITY)
        return INFINITY;

    d = am - bm;
    if(d < 0)
        d = -d;
    return d;
}

static int
route_metric_le(const struct babel_route *a, const struct babel_route *b)
{
    return route_metric(a) <= route_metric(b);
}

static struct babel_route *
find_compatible_group_head(struct babel_route *head,
                           const struct babel_route *route,
                           struct babel_route **prev_return)
{
    struct babel_route *prev = NULL;

    debugf("find_compatible_group_head: route metric=%d neigh=%s\n",
           route_metric(route),
           (route->neigh ? format_address(route->neigh->address) : "(null)"));

    while(head) {
        int distance = route_metric_distance(head, route);
        debugf("  checking head metric=%d distance=%d window=%d\n",
               route_metric(head), distance, ecmp_metric_window);
        if(distance <= ecmp_metric_window) {
            if(prev_return)
                *prev_return = prev;
            return head;
        }
        prev = head;
        head = head->next;
    }

    if(prev_return)
        *prev_return = NULL;
    return NULL;
}

static void
insert_multipath_member_sorted(struct babel_route *head,
                               struct babel_route *route)
{
    struct babel_route *prev = head;
    struct babel_route *cur = head->multipath;

    while(cur && route_metric_le(cur, route)) {
        prev = cur;
        cur = cur->multipath;
    }

    prev->multipath = route;
    route->multipath = cur;
    route->next = NULL;
}

static void
insert_group_head_sorted(int slot, struct babel_route *route)
{
    struct babel_route *prev = NULL;
    struct babel_route *cur = routes[slot];

    while(cur && route_metric_le(cur, route)) {
        prev = cur;
        cur = cur->next;
    }

    route->next = cur;
    route->multipath = NULL;
    if(prev)
        prev->next = route;
    else
        routes[slot] = route;
}

static struct babel_route *
find_group_head_for_route(int slot,
                          const struct babel_route *route,
                          struct babel_route **prev_head_return,
                          struct babel_route **prev_mp_return)
{
    struct babel_route *prev_head = NULL;
    struct babel_route *head;

    if(slot < 0 || slot >= route_slots)
        return NULL;

    head = routes[slot];
    while(head) {
        struct babel_route *prev_mp = NULL;
        struct babel_route *r = head;

        while(r) {
            if(r == route) {
                if(prev_head_return)
                    *prev_head_return = prev_head;
                if(prev_mp_return)
                    *prev_mp_return = prev_mp;
                return head;
            }
            prev_mp = r;
            r = r->multipath;
        }

        prev_head = head;
        head = head->next;
    }

    return NULL;
}

static void
rehome_route_to_installed_group(int slot,
                                struct babel_route *installed,
                                struct babel_route *route)
{
    struct babel_route *installed_head;
    struct babel_route *route_head;
    struct babel_route *prev_route_head = NULL;
    struct babel_route *prev_mp = NULL;

    if(slot < 0 || slot >= route_slots || installed == NULL || route == NULL)
        return;

    installed_head = find_group_head_for_route(slot, installed, NULL, NULL);
    route_head = find_group_head_for_route(slot, route, &prev_route_head, &prev_mp);

    if(installed_head == NULL || route_head == NULL)
        return;

    if(route_head == installed_head)
        return;

    if(route_metric_distance(installed_head, route) > ecmp_metric_window)
        return;

    debugf("rehome_route_to_installed_group: moving %p (%s) into group %p\n",
           (void*)route, format_address(route->nexthop), (void*)installed_head);

    if(route == route_head) {
        if(route_head->multipath) {
            struct babel_route *new_head = route_head->multipath;
            new_head->next = route_head->next;
            if(prev_route_head)
                prev_route_head->next = new_head;
            else
                routes[slot] = new_head;
        } else {
            if(prev_route_head)
                prev_route_head->next = route_head->next;
            else
                routes[slot] = route_head->next;
        }
    } else {
        assert(prev_mp != NULL);
        prev_mp->multipath = route->multipath;
    }

    route->multipath = NULL;
    route->next = NULL;
    insert_multipath_member_sorted(installed_head, route);
}

struct babel_route *
find_installed_route(const unsigned char *id,
                     const unsigned char *prefix, unsigned char plen,
                     const unsigned char *src_prefix, unsigned char src_plen,
                     int *index)
{
    int i;

    /* Case of multiple default routes, and no specific id */
    if(has_duplicate_default &&
       is_default(prefix, plen) &&
       !id) {
        /* A valid index pointer */
        if (index) {
            /* First call, find first route */
            if (*index < 0) {
                /* Binary search, will need to backtrack to to find the real first. being id checks the last for route slot, the slots should be adjacent */
                i = find_route_slot(NULL, prefix, plen, src_prefix, src_plen, NULL);
                /* No route found */
                if (i < 0)
                    return NULL;

                /* If index is valid, and match parameters */
                while (i-1 >= 0 && route_compare(NULL, prefix, plen, src_prefix, src_plen, routes[i-1]) == 0) {
                    i--;
                }
                /* End of cycle, either i-1 < 0 or routes[i-1] does not match, but i was a valid match */
                *index = i;
            }

            /* Valid index, match parameters */
            while (*index < route_slots && route_compare(NULL, prefix, plen, src_prefix, src_plen, routes[*index]) == 0) {
                /* The route is valid and installed */
                if (routes[*index]->installed == 1)
                    return routes[(*index)++];
                (*index)++;
            }
            /* No more valid routes */
            return NULL;
        }
    }

    i = find_route_slot(id, prefix, plen, src_prefix, src_plen, NULL);

    if(i >= 0) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->installed == 1 && (!id || memcmp(r->src->id, id, 8) == 0))
                    return r;
                r = r->multipath;
            }
            head = head->next;
        }
    }

    return NULL;
}

/* Returns an overestimate of the number of installed routes. */
int
installed_routes_estimate(void)
{
    return route_slots;
}

static int
resize_route_table(int new_slots)
{
    struct babel_route **new_routes;
    assert(new_slots >= route_slots);

    if(new_slots == 0) {
        new_routes = NULL;
        free(routes);
    } else {
        new_routes = realloc(routes, new_slots * sizeof(struct babel_route*));
        if(new_routes == NULL)
            return -1;
    }

    max_route_slots = new_slots;
    routes = new_routes;
    return 1;
}

/* Insert a route into the table.  If successful, retains the route.
   On failure, caller must free the route. */
struct babel_route *
insert_route(struct babel_route *route)
{
    int i, n;

    assert(!route->installed);

    i = find_route_slot(route->src->id,
                        route->src->prefix, route->src->plen,
                        route->src->src_prefix, route->src->src_plen, &n);

    if(i < 0) {
        if(route_slots >= max_route_slots)
            resize_route_table(max_route_slots < 1 ? 8 : 2 * max_route_slots);
        if(route_slots >= max_route_slots)
            return NULL;
        route->next = NULL;
        route->multipath = NULL;
        if(n < route_slots)
            memmove(routes + n + 1, routes + n,
                    (route_slots - n) * sizeof(struct babel_route*));
        route_slots++;
        routes[n] = route;
    } else if(multipath_ecmp == ECMP_DISABLED) {
        struct babel_route *r;
        r = routes[i];
        while(r->next)
            r = r->next;
        r->next = route;
        route->next = NULL;
        route->multipath = NULL;
    } else {
        struct babel_route *prev_group = NULL;
        struct babel_route *group_head =
            find_compatible_group_head(routes[i], route, &prev_group);

        debugf("insert_route: %s via %s, group_head=%p\n",
               format_prefix(route->src->prefix, route->src->plen),
               format_address(route->nexthop),
               (void*)group_head);

        if(group_head == NULL) {
            debugf("  -> new group head\n");
            insert_group_head_sorted(i, route);
        } else if(route_metric(route) < route_metric(group_head)) {
            struct babel_route *head_next = group_head->next;

            debugf("  -> replacing head (lower metric)\n");
            if(prev_group)
                prev_group->next = route;
            else
                routes[i] = route;

            route->next = head_next;
            route->multipath = group_head;
            group_head->next = NULL;
        } else {
            debugf("  -> multipath member of %p\n", (void*)group_head);
            insert_multipath_member_sorted(group_head, route);
        }
    }

    return route;
}

static void
destroy_route(struct babel_route *route)
{
    free(route);
}

void
flush_route(struct babel_route *route)
{
    int i;
    struct source *src;
    unsigned oldmetric;
    int has_alternate = 0;
    int update_ecmp = 0;
    int lost = 0;

    oldmetric = route_metric(route);
    src = route->src;

    debugf("flush_route: %s via %s installed=%d metric=%d\n",
           format_prefix(route->src->prefix, route->src->plen),
           format_address(route->nexthop),
           route->installed, route_metric(route));

    i = find_route_slot(route->src->id,
                        route->src->prefix, route->src->plen,
                        route->src->src_prefix, route->src->src_plen, NULL);
    assert(i >= 0 && i < route_slots);

    /* Debug: show all routes in this slot */
    {
        struct babel_route *dbg_head = routes[i];
        int group_num = 0;
        while(dbg_head) {
            struct babel_route *dbg_r = dbg_head;
            int member_num = 0;
            while(dbg_r) {
                debugf("  slot[%d] group[%d] member[%d]: installed=%d metric=%d via %s%s\n",
                       i, group_num, member_num,
                       dbg_r->installed, route_metric(dbg_r),
                       format_address(dbg_r->nexthop),
                       dbg_r == route ? " <-- FLUSHING" : "");
                dbg_r = dbg_r->multipath;
                member_num++;
            }
            dbg_head = dbg_head->next;
            group_num++;
        }
    }

    if(route->installed == 1) {
        /* Primary route: check if there are other installed ECMP members
           OR other routes in this slot that could potentially be installed */
        struct babel_route *head = routes[i];
        has_alternate = 0;
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r != route && (r->installed > 0 || route_metric(r) < INFINITY)) {
                    has_alternate = 1;
                    debugf("  found alternate: via %s installed=%d metric=%d\n",
                           format_address(r->nexthop), r->installed, route_metric(r));
                    break;
                }
                r = r->multipath;
            }
            if(has_alternate)
                break;
            head = head->next;
        }
        
        if(has_alternate) {
            update_ecmp = 1;
            debugf("  -> will update_ecmp\n");
        } else {
            debugf("  -> no alternate, uninstalling and marking lost\n");
            uninstall_route(route);
            lost = 1;
        }
    } else if(route->installed > 1) {
        /* Secondary ECMP member: primary exists, need to reprogram */
        update_ecmp = 1;
        debugf("  -> secondary member, will update_ecmp\n");
    } else {
        debugf("  -> not installed, nothing to do for kernel\n");
    }

    local_notify_route(route, LOCAL_FLUSH);

    {
        struct babel_route *prev_head = NULL;
        struct babel_route *head = routes[i];
        int removed = 0;

        while(head) {
            if(head == route) {
                if(head->multipath) {
                    struct babel_route *new_head = head->multipath;
                    new_head->next = head->next;
                    if(prev_head)
                        prev_head->next = new_head;
                    else
                        routes[i] = new_head;
                    head->next = NULL;
                    head->multipath = NULL;
                } else {
                    if(prev_head)
                        prev_head->next = head->next;
                    else
                        routes[i] = head->next;
                    head->next = NULL;
                }
                removed = 1;
                break;
            }

            {
                struct babel_route *prev_mp = head;
                struct babel_route *mp = head->multipath;
                while(mp) {
                    if(mp == route) {
                        prev_mp->multipath = mp->multipath;
                        mp->multipath = NULL;
                        removed = 1;
                        break;
                    }
                    prev_mp = mp;
                    mp = mp->multipath;
                }
            }

            if(removed)
                break;

            prev_head = head;
            head = head->next;
        }

        assert(removed);
        destroy_route(route);

        if(routes[i] == NULL) {
            if(i < route_slots - 1)
                memmove(routes + i, routes + i + 1,
                        (route_slots - i - 1) * sizeof(struct babel_route*));
            routes[route_slots - 1] = NULL;
            route_slots--;
            VALGRIND_MAKE_MEM_UNDEFINED(routes + route_slots, sizeof(struct babel_route *));
        }

        if(route_slots == 0)
            resize_route_table(0);
        else if(max_route_slots > 8 && route_slots < max_route_slots / 4)
            resize_route_table(max_route_slots / 2);
    }

    debugf("flush_route: after removal, update_ecmp=%d, i=%d, route_slots=%d\n",
           update_ecmp, i, route_slots);
    if(i < route_slots && routes[i] != NULL) {
        int cmp = route_compare(NULL, src->prefix, src->plen,
                                src->src_prefix, src->src_plen,
                                routes[i]);
        debugf("  routes[i] compare result=%d (0 means match)\n", cmp);
    }

    if(update_ecmp && i < route_slots && routes[i] != NULL &&
       route_compare(NULL, src->prefix, src->plen,
                     src->src_prefix, src->src_plen,
                     routes[i]) == 0) {
        struct babel_route *primary = routes[i];

        debugf("  -> calling refresh_installed_ranks on routes[%d]\n", i);
        /* An ECMP member was flushed. Let refresh_installed_ranks() handle
           the kernel update - it will detect the changed nexthop set and
           do a single FLUSH+ADD. */
        refresh_installed_ranks(primary);
    } else {
        debugf("  -> NOT calling refresh_installed_ranks (conditions not met)\n");
    }

    if(lost)
        route_lost(src, oldmetric);

    release_source(src);
}

void
flush_all_routes()
{
    int i;

    i = route_slots - 1;
    while(i >= 0) {
        while(i < route_slots) {
            if(routes[i]->installed == 1)
                uninstall_route(routes[i]);
            flush_route(routes[i]);
        }
        i--;
    }

    check_sources_released();
}

void
flush_neighbour_routes(struct neighbour *neigh)
{
    int i;

    i = 0;
    while(i < route_slots) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->neigh == neigh) {
                    flush_route(r);
                    goto again;
                }
                r = r->multipath;
            }
            head = head->next;
        }
        i++;
    again:
        ;
    }
}

void
flush_interface_routes(struct interface *ifp, int v4only)
{
    int i;

    i = 0;
    while(i < route_slots) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->neigh && r->neigh->ifp &&
                   r->neigh->ifp == ifp &&
                   (!v4only || v4mapped(r->nexthop))) {
                    flush_route(r);
                    goto again;
                }
                r = r->multipath;
            }
            head = head->next;
        }
        i++;
    again:
        ;
    }
}

struct route_stream {
    int installed;
    int index;
    struct babel_route *head;
    struct babel_route *next;
};

struct route_stream *
route_stream(int installed)
{
    struct route_stream *stream;

    stream = calloc(1, sizeof(struct route_stream));
    if(stream == NULL)
        return NULL;

    stream->installed = installed;
    stream->index = -1;
    stream->head = NULL;
    stream->next = NULL;

    return stream;
}

static struct babel_route *
route_stream_next_any(struct route_stream *stream)
{
    while(1) {
        if(stream->next == NULL) {
            stream->index++;
            while(stream->index < route_slots && routes[stream->index] == NULL)
                stream->index++;
            if(stream->index >= route_slots)
                return NULL;
            stream->head = routes[stream->index];
            stream->next = stream->head;
            return stream->next;
        }

        if(stream->next->multipath) {
            stream->next = stream->next->multipath;
            return stream->next;
        }

        if(stream->head && stream->head->next) {
            stream->head = stream->head->next;
            stream->next = stream->head;
            return stream->next;
        } else {
            stream->head = NULL;
            stream->next = NULL;
        }
    }
}

struct babel_route *
route_stream_next(struct route_stream *stream)
{
    struct babel_route *next;

    if(stream->installed) {
        while(1) {
            next = route_stream_next_any(stream);
            if(next == NULL)
                return NULL;
            if(next->installed == 1)
                return next;
        }
    }

    return route_stream_next_any(stream);
}

void
route_stream_done(struct route_stream *stream)
{
    free(stream);
}

int
metric_to_kernel(int metric)
{
        if(metric >= INFINITY) {
                return KERNEL_INFINITY;
        } else if(reflect_kernel_metric) {
                int r = kernel_metric + metric;
                return r >= KERNEL_INFINITY ? KERNEL_INFINITY : r;
        } else {
                return kernel_metric;
        }
}

static unsigned int
route_ifindex_or_zero(const struct babel_route *route)
{
    if(route && route->neigh && route->neigh->ifp)
        return route->neigh->ifp->ifindex;
    return 0;
}

static void
move_installed_route(struct babel_route *route, int i)
{
    struct babel_route *prev_head = NULL;
    struct babel_route *head = routes[i];
    struct babel_route *group_head = NULL;
    struct babel_route *group_prev_head = NULL;
    struct babel_route *prev_mp = NULL;

    assert(i >= 0 && i < route_slots);
    assert(route->installed == 1);

    while(head) {
        if(head == route) {
            group_head = head;
            group_prev_head = prev_head;
            break;
        }

        prev_mp = head;
        {
            struct babel_route *mp = head->multipath;
            while(mp) {
                if(mp == route) {
                    group_head = head;
                    group_prev_head = prev_head;
                    break;
                }
                prev_mp = mp;
                mp = mp->multipath;
            }
        }

        if(group_head)
            break;

        prev_head = head;
        head = head->next;
    }

    if(group_head == NULL)
        return;

    if(group_head != route) {
        prev_mp->multipath = route->multipath;
        route->multipath = group_head;
        route->next = group_head->next;
        group_head->next = NULL;

        if(group_prev_head)
            group_prev_head->next = route;
        else
            routes[i] = route;
    }

    if(routes[i] != route) {
        struct babel_route *p = group_prev_head;
        if(p == NULL) {
            p = routes[i];
            while(p && p->next != route)
                p = p->next;
        }
        if(p)
            p->next = route->next;
        route->next = routes[i];
        routes[i] = route;
    }
}

static int
route_old(struct babel_route *route)
{
    return route->time < now.tv_sec - route->hold_time * 7 / 8;
}

static int
route_expired(struct babel_route *route)
{
    return route->time < now.tv_sec - route->hold_time;
}

static int
find_route_slot_for_route(const struct babel_route *route)
{
    return find_route_slot(route->src->id,
                           route->src->prefix, route->src->plen,
                           route->src->src_prefix, route->src->src_plen,
                           NULL);
}

static void
clear_installed_ranks(const struct babel_route *route, int clear_tables)
{
    int i;
    struct babel_route *head;
    struct babel_route *r;

    i = find_route_slot_for_route(route);
    if(i < 0)
        return;

    head = find_group_head_for_route(i, route, NULL, NULL);
    if(head == NULL)
        return;

    r = head;
    while(r) {
        r->installed = 0;
        if(clear_tables)
            r->installed_table_count = 0;
        r = r->multipath;
    }
}

static int
metric_to_ecmp_weight(int metric, int min_metric)
{
    int base = 5;
    int numerator;
    int denominator;
    int weight;

    if(metric < 0)
        metric = 0;
    if(min_metric < 0)
        min_metric = 0;

    numerator = base * (min_metric + 256);
    denominator = metric + 256;
    if(denominator <= 0)
        denominator = 1;

    weight = numerator / denominator;
    if(weight < 1)
        weight = 1;
    if(weight > 256)
        weight = 256;
    return weight;
}

static int
collect_multipath_nexthops(const struct babel_route *route,
                          struct kernel_nexthop *nexthops,
                          int max_nexthops,
                          int *best_metric_out)
{
    int i;
    int best_metric = INFINITY;
    int min_metric = INFINITY;
    int count = 0;
    int collected_metrics[MAX_ECMP_NEXTHOPS];
    struct babel_route *head;
    struct babel_route *r;

    i = find_route_slot(route->src->id,
                        route->src->prefix, route->src->plen,
                        route->src->src_prefix, route->src->src_plen,
                        NULL);
    if(i < 0)
        return 0;

    head = find_group_head_for_route(i, route, NULL, NULL);
    if(head == NULL)
        return 0;

    r = head;
    while(r) {
        int metric;
        if(r->neigh == NULL || r->neigh->ifp == NULL ||
           route_expired(r) || !route_feasible(r)) {
            r = r->multipath;
            continue;
        }

        metric = route_metric(r);
        if(metric >= INFINITY) {
            r = r->multipath;
            continue;
        }

        if(metric < best_metric)
            best_metric = metric;

        r = r->multipath;
    }

    if(best_metric >= INFINITY)
        return 0;
    min_metric = best_metric;

    r = head;
    while(r && count < max_nexthops) {
        int metric;
        int j;
        int duplicate = 0;

        if(r->neigh == NULL || r->neigh->ifp == NULL ||
           route_expired(r) || !route_feasible(r)) {
            r = r->multipath;
            continue;
        }

        metric = route_metric(r);
        if(metric >= INFINITY) {
            r = r->multipath;
            continue;
        }

        if(metric > best_metric + ecmp_metric_window) {
            r = r->multipath;
            continue;
        }

        for(j = 0; j < count; j++) {
            if(nexthops[j].ifindex == r->neigh->ifp->ifindex &&
               memcmp(nexthops[j].gate, r->nexthop, 16) == 0) {
                duplicate = 1;
                break;
            }
        }

        if(!duplicate) {
            memcpy(nexthops[count].gate, r->nexthop, 16);
            nexthops[count].ifindex = r->neigh->ifp->ifindex;
            collected_metrics[count] = metric;
            nexthops[count].weight = 1;
            count++;
        }

        r = r->multipath;
    }

    if(multipath_ecmp == ECMP_WEIGHT) {
        for(i = 0; i < count; i++)
            nexthops[i].weight = metric_to_ecmp_weight(collected_metrics[i],
                                                       min_metric);
    }

    if(best_metric_out)
        *best_metric_out = best_metric;

    return count;
}

void
refresh_installed_ranks(struct babel_route *route)
{
    int slot, i, count;
    struct babel_route *head;
    struct babel_route *r;
    struct babel_route *primary = NULL;
    struct babel_route *old_primary = NULL;
    struct kernel_nexthop nexthops[MAX_ECMP_NEXTHOPS];
    struct kernel_nexthop old_nexthops[MAX_ECMP_NEXTHOPS];
    int old_nexthop_count = 0;
    int set_changed = 0;
    int group_size = 0;

    slot = find_route_slot_for_route(route);
    if(slot < 0)
        return;

    head = find_group_head_for_route(slot, route, NULL, NULL);
    if(head == NULL)
        return;

    /* Count group members for debug */
    r = head;
    while(r) {
        group_size++;
        r = r->multipath;
    }

    debugf("refresh_installed_ranks(%s): slot=%d, head=%p, group_size=%d\n",
           format_prefix(route->src->prefix, route->src->plen),
           slot, (void*)head, group_size);

    /* Record currently installed nexthops before refresh */
    if(multipath_ecmp != ECMP_DISABLED) {
        r = head;
        while(r) {
            const char *ifname = "(null)";
            int ifindex = -1;

            if(r->neigh && r->neigh->ifp) {
                ifname = r->neigh->ifp->name;
                ifindex = r->neigh->ifp->ifindex;
            }

            debugf("  member %p: installed=%d metric=%d via %s if %s\n",
                   (void*)r, r->installed, route_metric(r),
                   format_address(r->nexthop), ifname);
            if(r->installed > 0) {
                if(r->installed == 1 && old_primary == NULL)
                    old_primary = r;

                int duplicate = 0;
                for(int j = 0; j < old_nexthop_count; j++) {
                    if(old_nexthops[j].ifindex == ifindex &&
                       memcmp(old_nexthops[j].gate, r->nexthop, 16) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if(!duplicate && old_nexthop_count < MAX_ECMP_NEXTHOPS) {
                    memcpy(old_nexthops[old_nexthop_count].gate, r->nexthop, 16);
                    old_nexthops[old_nexthop_count].ifindex = ifindex;
                    old_nexthop_count++;
                }
            }
            r = r->multipath;
        }
    }

    r = head;
    while(r) {
        r->installed = 0;
        r = r->multipath;
    }

    if(multipath_ecmp == ECMP_DISABLED) {
        route->installed = 1;
        move_installed_route(route, slot);
        return;
    }

    count = collect_multipath_nexthops(route, nexthops, MAX_ECMP_NEXTHOPS, NULL);
    debugf("refresh_installed_ranks: collect_multipath_nexthops returned %d nexthops\n", count);
    if(count == 0) {
        if(route_metric(route) < INFINITY) {
            route->installed = 1;
            move_installed_route(route, slot);
        }
        /* If we had installed nexthops before and now have none, kernel needs update.
           This includes the case where the route retracted (metric >= INFINITY). */
        if(old_nexthop_count > 0) {
            set_changed = 1;
        }
        goto update_kernel_if_needed;
    }

    if(count == 1) {
        r = head;
        while(r) {
                if(r->neigh && r->neigh->ifp &&
                    !route_expired(r) && route_feasible(r) &&
               route_metric(r) < INFINITY &&
               r->neigh->ifp->ifindex == nexthops[0].ifindex &&
               memcmp(r->nexthop, nexthops[0].gate, 16) == 0) {
                r->installed = 1;
                primary = r;
                break;
            }
            r = r->multipath;
        }

        if(primary == NULL) {
            route->installed = 1;
            primary = route;
        }

        if(primary)
            move_installed_route(primary, slot);
        
        /* Detect transitions that require reprogramming kernel route. */
        if(old_nexthop_count != 1) {
            set_changed = 1;
        } else if(old_primary && primary && old_primary == primary &&
                  old_primary->installed_table_count == 0) {
            /* Bookkeeping was lost: force a one-time resync so stale
               kernel entries (including unreachable siblings) are flushed. */
            debugf("  set_changed: forcing resync (single nexthop, table state lost)\n");
            set_changed = 1;
        }
        goto update_kernel_if_needed;
    }

    for(i = 0; i < count; i++) {
        r = head;
        while(r) {
            if(r->installed == 0 && r->neigh && r->neigh->ifp &&
               !route_expired(r) && route_feasible(r) &&
               route_metric(r) < INFINITY &&
               r->neigh->ifp->ifindex == nexthops[i].ifindex &&
               memcmp(r->nexthop, nexthops[i].gate, 16) == 0) {
                r->installed = i + 1;
                if(r->installed == 1)
                    primary = r;
                break;
            }
            r = r->multipath;
        }
    }

    if(primary)
        move_installed_route(primary, slot);

    /* If the group now has 2+ nexthops, taint every member so that when it
       later shrinks back to 1 it stays in multipath encoding. */
    if(count > 1) {
        r = head;
        while(r) {
            r->multipath_ever = 1;
            r = r->multipath;
        }
    }

    /* Detect if transitioning from single path or different multipath set */
    if(old_nexthop_count != count) {
        debugf("  set_changed: old_nexthop_count (%d) != count (%d)\n",
               old_nexthop_count, count);
        set_changed = 1;
    } else if(old_nexthop_count > 1) {
        /* Check if the multipath members changed */
        for(i = 0; i < count; i++) {
            int found = 0;
            for(int j = 0; j < old_nexthop_count; j++) {
                if(nexthops[i].ifindex == old_nexthops[j].ifindex &&
                   memcmp(nexthops[i].gate, old_nexthops[j].gate, 16) == 0) {
                    found = 1;
                    break;
                }
            }
            if(!found) {
                debugf("  set_changed: nexthop %s not found in old set\n",
                       format_address(nexthops[i].gate));
                set_changed = 1;
                break;
            }
        }
    }

update_kernel_if_needed:
    /* If ECMP set changed, reprogram kernel route regardless of metric delta. */
    debugf("refresh_installed_ranks: old_nexthop_count=%d, count=%d, set_changed=%d, primary=%p, old_primary=%p\n",
           old_nexthop_count, count, set_changed, (void*)primary, (void*)old_primary);
    if(old_primary)
        debugf("  old_primary installed_table_count=%d\n", old_primary->installed_table_count);
    if(set_changed) {
        int rc;

        debugf("ECMP set changed for %s/%u: reprogramming kernel\n",
               format_prefix(route->src->prefix, route->src->plen),
               route->src->plen);

        /* First FLUSH the old route if one was installed */
        if(old_primary) {
            rc = change_route(ROUTE_FLUSH,
                              old_primary,
                              metric_to_kernel(route_metric(old_primary)),
                              NULL, 0, 0,
                              NULL, NULL, NULL);
            if(rc < 0 && errno != ESRCH && errno != ENOENT)
                perror("kernel_route(FLUSH ecmp refresh)");

            old_primary->installed_table_count = 0;
        }

        /* Then ADD the new route if we have a valid primary */
        if(primary && route_metric(primary) < INFINITY) {
            int metric = metric_to_kernel(route_metric(primary));
            rc = change_route(ROUTE_ADD,
                              primary,
                              metric,
                              NULL, 0, 0,
                              NULL,
                              primary->installed_tables,
                              &primary->installed_table_count);
            if(rc < 0 && errno != EEXIST)
                perror("kernel_route(ADD ecmp refresh)");
        }
    }

    /* Debug: show final state */
    r = head;
    while(r) {
        debugf("  final: %p installed=%d tables=%d via %s\n",
               (void*)r, r->installed, r->installed_table_count,
               format_address(r->nexthop));
        r = r->multipath;
    }
}


int
route_ecmp_weight(struct babel_route *route)
{
    int slot;
    int min_metric = INFINITY;
    int metric;
    struct babel_route *head;
    struct babel_route *r;

    if(route == NULL)
        return 0;

    metric = route_metric(route);
    if(metric >= INFINITY)
        return 0;

    slot = find_route_slot_for_route(route);
    if(slot < 0)
        return 0;

    head = find_group_head_for_route(slot, route, NULL, NULL);
    if(head == NULL)
        return 0;

    r = head;
    while(r) {
        int m;

        if(route_expired(r) || !route_feasible(r)) {
            r = r->multipath;
            continue;
        }

        m = route_metric(r);
        if(m < INFINITY && m < min_metric)
            min_metric = m;

        r = r->multipath;
    }

    if(min_metric >= INFINITY)
        return 0;

    return metric_to_ecmp_weight(metric, min_metric);
}

int
change_route(int operation, const struct babel_route *route, int metric,
             const unsigned char *new_next_hop,
             int new_ifindex, int newmetric,
             const struct source *newsrc,
             int *installed_tables, int *installed_table_count)
{
    struct filter_result filter_result, new_filter_result;
    unsigned char *pref_src = NULL;
    unsigned char *newpref_src = NULL;
    unsigned int ifindex = route_ifindex_or_zero(route);
    int m, i, rc, first_rc = 0;
    int tables_to_use[MAX_TABLES_PER_FILTER];
    int num_tables = 0;

    m = install_filter(route->src->id,
                       route->src->prefix, route->src->plen,
                       route->src->src_prefix, route->src->src_plen,
                       ifindex, &filter_result);
    if(m >= INFINITY && operation == ROUTE_ADD) {
        errno = EPERM;
        return -1;
    }

    pref_src = filter_result.pref_src;
    newpref_src = pref_src;
    
    /* Get list of tables to operate on */
    if(operation == ROUTE_FLUSH) {
        /* For flush, use the tables the route is currently installed in */
        if(route->installed_table_count > 0) {
            memcpy(tables_to_use, route->installed_tables, 
                   route->installed_table_count * sizeof(int));
            num_tables = route->installed_table_count;
        } else if(filter_result.table_count > 0) {
            /* Bookkeeping fallback: flush from filtered export tables. */
            memcpy(tables_to_use, filter_result.tables,
                   filter_result.table_count * sizeof(unsigned int));
            num_tables = filter_result.table_count;
        } else if(export_table > 0) {
            tables_to_use[0] = export_table;
            num_tables = 1;
        }
    } else {
        /* For ADD/MODIFY, evaluate filter result */
        if(filter_result.table_count > 0) {
            memcpy(tables_to_use, filter_result.tables, 
                   filter_result.table_count * sizeof(unsigned int));
            num_tables = filter_result.table_count;
        } else if(export_table > 0) {
            tables_to_use[0] = export_table;
            num_tables = 1;
        }
    }

    if(newsrc) {
        m = install_filter(newsrc->id,
                           newsrc->prefix, newsrc->plen,
                           newsrc->src_prefix, newsrc->src_plen,
                           new_ifindex, &new_filter_result);
        newpref_src = new_filter_result.pref_src ? new_filter_result.pref_src : pref_src;
    }

    /* Install, modify, or flush route in each table */
    if(installed_tables && installed_table_count) {
        *installed_table_count = 0;
    }

    for(i = 0; i < num_tables; i++) {
        int table = tables_to_use[i];
        int newtable = table;
        int use_multipath = 0;
        struct kernel_nexthop nexthops[MAX_ECMP_NEXTHOPS];
        int nexthop_count = 0;
        int best_metric = INFINITY;
        int effective_newmetric = newmetric;
        
        if(newsrc && new_filter_result.table_count > 0) {
            /* Use corresponding table from new filter result if available */
            newtable = i < new_filter_result.table_count ? 
                       new_filter_result.tables[i] : new_filter_result.tables[0];
        } else if(newsrc) {
            newtable = table;
        }

        if(multipath_ecmp != ECMP_DISABLED) {
            if(operation != ROUTE_FLUSH) {
                nexthop_count = collect_multipath_nexthops(route,
                                                           nexthops,
                                                           MAX_ECMP_NEXTHOPS,
                                                           &best_metric);
                /* Use multipath encoding if: more than 1 nexthop now, OR the
                   group was ever multipath (tainted) and still has >=1 nexthop.
                   Simple routes that never had >1 nexthop keep normal encoding. */
                if(nexthop_count > 1 || (nexthop_count == 1 && route->multipath_ever))
                    use_multipath = 1;
                
                /* When one route retracts (newmetric = INFINITY) but valid
                   nexthops remain, use the best valid metric instead.
                   This prevents installing an unreachable route when we still
                   have reachable nexthops. */
                if(newmetric >= KERNEL_INFINITY && nexthop_count > 0 &&
                   best_metric < INFINITY) {
                    effective_newmetric = metric_to_kernel(best_metric);
                }
            } else {
                /* For ROUTE_FLUSH in ECMP mode, ALWAYS use kernel_route_multipath()
                   which omits RTA_PRIORITY from the RTM_DELROUTE request.
                   This is essential because by the time we flush, the route's metric
                   may have changed to INFINITY (retraction), but the kernel still
                   stores the route at the original installation metric (e.g. 0).
                   kernel_route() includes RTA_PRIORITY and would get ESRCH on metric
                   mismatch, silently leaving the stale route in the kernel and blocking
                   the subsequent ADD with EEXIST.  kernel_route_multipath FLUSH always
                   deletes by dest+table+protocol only, which is always correct since
                   babeld only ever installs one route per that tuple. */
                nexthop_count = collect_multipath_nexthops(route,
                                                           nexthops,
                                                           MAX_ECMP_NEXTHOPS,
                                                           NULL);
                use_multipath = 1;
            }
        }
        
        kernel_route_operation_depth++;
        if(use_multipath) {
            rc = kernel_route_multipath(operation,
                                        table,
                                        route->src->prefix, route->src->plen,
                                        route->src->src_prefix, route->src->src_plen,
                                        pref_src,
                                        metric,
                                        effective_newmetric,
                                        nexthops,
                                        nexthop_count,
                                        operation == ROUTE_MODIFY ? newtable : 0,
                                        newpref_src);
            if(rc < 0 && errno == ENOSYS) {
                rc = kernel_route(operation, table, route->src->prefix, route->src->plen,
                                  route->src->src_prefix, route->src->src_plen, pref_src,
                                  route->nexthop, ifindex,
                                  metric, new_next_hop, new_ifindex, effective_newmetric,
                                  operation == ROUTE_MODIFY ? newtable : 0,
                                  newpref_src);
            }
        } else {
            rc = kernel_route(operation, table, route->src->prefix, route->src->plen,
                              route->src->src_prefix, route->src->src_plen, pref_src,
                              route->nexthop, ifindex,
                              metric, new_next_hop, new_ifindex, effective_newmetric,
                              operation == ROUTE_MODIFY ? newtable : 0,
                              newpref_src);
        }

        kernel_route_operation_depth--;

        if(rc < 0) {
            /* Continue to attempt other tables, but save first error */
            if(first_rc == 0)
                first_rc = rc;
        } else if(installed_tables && installed_table_count) {
            /* Store successfully installed table */
            if(*installed_table_count < MAX_TABLES_PER_FILTER) {
                installed_tables[(*installed_table_count)++] = newtable;
            }
        }
    }

    return first_rc == 0 ? (num_tables > 0 ? 0 : -1) : first_rc;
}

void
install_route(struct babel_route *route)
{
    int i, rc;

    if(route->installed > 0)
        return;

    if(!route_feasible(route))
        fprintf(stderr, "WARNING: installing unfeasible route "
                "(this shouldn't happen).");

    i = find_route_slot(route->src->id,
                        route->src->prefix, route->src->plen,
                        route->src->src_prefix, route->src->src_plen, NULL);
    assert(i >= 0 && i < route_slots);

    if(routes[i] != route && routes[i]->installed) {
        fprintf(stderr, "WARNING: attempting to install duplicate route "
                "(this shouldn't happen).");
        return;
    }

    debugf("install_route(%s from %s)\n",
           format_prefix(route->src->prefix, route->src->plen),
           format_prefix(route->src->src_prefix, route->src->src_plen));
    rc = change_route(ROUTE_ADD, route, metric_to_kernel(route_metric(route)),
                      NULL, 0, 0, NULL, route->installed_tables, &route->installed_table_count);
    if(rc < 0 && errno != EEXIST) {
        perror("kernel_route(ADD)");
        return;
    }

    refresh_installed_ranks(route);

    local_notify_route(route, LOCAL_CHANGE);
}

void
uninstall_route(struct babel_route *route)
{
    int rc;

    if(route->installed != 1)
        return;

    debugf("uninstall_route(%s from %s)\n",
           format_prefix(route->src->prefix, route->src->plen),
           format_prefix(route->src->src_prefix, route->src->src_plen));
    rc = change_route(ROUTE_FLUSH, route, metric_to_kernel(route_metric(route)),
                      NULL, 0, 0, NULL, NULL, NULL);
    if(rc < 0)
        perror("kernel_route(FLUSH)");

    /* Always clear installed ranks, even if kernel FLUSH failed.
       This prevents flush_route() from trying to reprogram ECMP routes
       that we're abandoning (e.g. during shutdown with EINTR). */
    clear_installed_ranks(route, 1);
    local_notify_route(route, LOCAL_CHANGE);
}

/* This is equivalent to uninstall_route followed with install_route,
   but without the race condition.  The destination of both routes
   must be the same. */
static void
switch_routes(struct babel_route *old, struct babel_route *new)
{
    int rc;
    unsigned int new_ifindex;

    if(!old) {
        install_route(new);
        return;
    }

    if(old->installed != 1)
        return;

    if(!route_feasible(new))
        fprintf(stderr, "WARNING: switching to unfeasible route "
                "(this shouldn't happen).");

    new_ifindex = route_ifindex_or_zero(new);

    debugf("switch_routes(%s from %s)\n",
           format_prefix(old->src->prefix, old->src->plen),
           format_prefix(old->src->src_prefix, old->src->src_plen));
    rc = change_route(ROUTE_MODIFY, old, metric_to_kernel(route_metric(old)),
                 new->nexthop, new_ifindex,
                      metric_to_kernel(route_metric(new)),
                      new->src, new->installed_tables, &new->installed_table_count);
    if(rc < 0) {
        perror("kernel_route(MODIFY)");
        return;
    }

    old->installed_table_count = 0;
    refresh_installed_ranks(new);
    local_notify_route(old, LOCAL_CHANGE);
    local_notify_route(new, LOCAL_CHANGE);
}

void
change_route_metric(struct babel_route *route,
                    unsigned refmetric, unsigned cost, unsigned add)
{
    int oldmetric = metric_to_kernel(route_metric(route)),
        newmetric = metric_to_kernel(MIN(refmetric + cost + add, INFINITY));
    int suppress_hold_down = 0;

    if(route->installed > 0 && newmetric >= KERNEL_INFINITY) {
        struct xroute *xroute;

        xroute = find_xroute(route->src->prefix, route->src->plen,
                             route->src->src_prefix, route->src->src_plen);
        if(xroute && (allow_duplicates < 0 || xroute->metric >= allow_duplicates)) {
            suppress_hold_down = 1;
            debugf("change_route_metric: suppressing unreachable hold-down for %s (local xroute exists)\n",
                   format_prefix(route->src->prefix, route->src->plen));
        }
    }

    if(multipath_ecmp != ECMP_DISABLED && route->installed > 0) {
        /* For ECMP, always refresh when the route is installed, regardless of
           whether oldmetric == newmetric.  A route may have had its metric
           computed as KERNEL_INFINITY already (e.g. neighbour cost blew up)
           while still holding installed > 1 — in that case oldmetric ==
           newmetric but the kernel nexthop set still needs to be corrected.
           refresh_installed_ranks() has its own set_changed guard so spurious
           calls are cheap. */
        route_smoothed_metric(route);

        route->refmetric = refmetric;
        route->cost = cost;
        route->add_metric = add;

        if(smoothing_half_life == 0) {
            route->smoothed_metric = route_metric(route);
            route->smoothed_metric_time = now.tv_sec;
        }

        debugf("change_route_metric(%s from %s, %d -> %d) [ecmp installed=%d]\n",
               format_prefix(route->src->prefix, route->src->plen),
               format_prefix(route->src->src_prefix, route->src->src_plen),
               oldmetric, newmetric, route->installed);

        /* Let refresh_installed_ranks() handle ALL kernel updates for ECMP.
           It computes the correct nexthop set (excluding retracted routes)
           and does a single FLUSH+ADD. Doing ROUTE_MODIFY here would cause
           duplicate/conflicting kernel operations. */
        refresh_installed_ranks(route);

        local_notify_route(route, LOCAL_CHANGE);
        return;
    }

    if(route->installed > 0 && oldmetric != newmetric) {
        int rc;
        unsigned int ifindex = route_ifindex_or_zero(route);

        if(suppress_hold_down) {
            rc = change_route(ROUTE_FLUSH, route, oldmetric,
                              NULL, 0, 0, NULL, NULL, NULL);
            if(rc < 0 && errno != ESRCH && errno != ENOENT)
                perror("kernel_route(FLUSH hold-down suppress)");

            clear_installed_ranks(route, 1);
            goto update_local_state;
        }

        debugf("change_route_metric(%s from %s, %d -> %d)\n",
               format_prefix(route->src->prefix, route->src->plen),
               format_prefix(route->src->src_prefix, route->src->src_plen),
               oldmetric, newmetric);
           rc = change_route(ROUTE_MODIFY, route, oldmetric, route->nexthop,
                     ifindex, newmetric, NULL, NULL, NULL);
        if(rc < 0) {
            perror("kernel_route(MODIFY metric)");
            return;
        }

        refresh_installed_ranks(route);
    }

update_local_state:
    /* Update route->smoothed_metric using the old metric. */
    route_smoothed_metric(route);

    route->refmetric = refmetric;
    route->cost = cost;
    route->add_metric = add;

    if(smoothing_half_life == 0) {
        route->smoothed_metric = route_metric(route);
        route->smoothed_metric_time = now.tv_sec;
    }

    local_notify_route(route, LOCAL_CHANGE);
}

static void
retract_route(struct babel_route *route)
{
    /* We cannot simply remove the route from the kernel, as that might
       cause a routing loop -- see RFC 6126 Sections 2.8 and 3.5.5. */
    change_route_metric(route, INFINITY, INFINITY, 0);
}

int
route_feasible(struct babel_route *route)
{
    return update_feasible(route->src, route->seqno, route->refmetric);
}

int
update_feasible(struct source *src,
                unsigned short seqno, unsigned short refmetric)
{
    if(src == NULL)
        return 1;

    if(src->time < now.tv_sec - SOURCE_GC_TIME)
        /* Never mind what is probably stale data */
        return 1;

    if(refmetric >= INFINITY)
        /* Retractions are always feasible */
        return 1;

    return (seqno_compare(seqno, src->seqno) > 0 ||
            (src->seqno == seqno && refmetric < src->metric));
}

void
change_smoothing_half_life(int half_life)
{
    if(half_life <= 0) {
        smoothing_half_life = 0;
        two_to_the_one_over_hl = 0;
        return;
    }

    smoothing_half_life = half_life;
    switch(smoothing_half_life) {
    case 1: two_to_the_one_over_hl = 131072; break;
    case 2: two_to_the_one_over_hl = 92682; break;
    case 3: two_to_the_one_over_hl = 82570; break;
    case 4: two_to_the_one_over_hl = 77935; break;
    default:
        /* 2^(1/x) is 1 + log(2)/x + O(1/x^2) at infinity. */
        two_to_the_one_over_hl = 0x10000 + 45426 / half_life;
    }
}

/* Update the smoothed metric, return the new value. */
int
route_smoothed_metric(struct babel_route *route)
{
    int metric = route_metric(route);

    if(smoothing_half_life <= 0 ||                 /* no smoothing */
       metric >= INFINITY ||                       /* route retracted */
       route->smoothed_metric_time > now.tv_sec || /* clock stepped */
       route->smoothed_metric == metric) {         /* already converged */
        route->smoothed_metric = metric;
        route->smoothed_metric_time = now.tv_sec;
    } else {
        int diff;
        /* We randomise the computation, to minimise global synchronisation
           and hence oscillations. */
        while(route->smoothed_metric_time <= now.tv_sec - smoothing_half_life) {
            diff = metric - route->smoothed_metric;
            route->smoothed_metric += roughly(diff) / 2;
            route->smoothed_metric_time += smoothing_half_life;
        }
        while(route->smoothed_metric_time < now.tv_sec) {
            diff = metric - route->smoothed_metric;
            route->smoothed_metric +=
                roughly(diff) * (two_to_the_one_over_hl - 0x10000) / 0x10000;
            route->smoothed_metric_time++;
        }

        diff = metric - route->smoothed_metric;
        if(diff > -4 && diff < 4)
            route->smoothed_metric = metric;
    }

    /* change_route_metric relies on this */
    assert(route->smoothed_metric_time == now.tv_sec);
    return route->smoothed_metric;
}

static int
route_acceptable(struct babel_route *route, int feasible,
                 struct neighbour *exclude)
{
    if(route_expired(route))
        return 0;
    if(feasible && !route_feasible(route))
        return 0;
    if(feasible && route_metric(route) >= INFINITY)
        return 0;
    if(exclude && route->neigh == exclude)
        return 0;
    return 1;
}

/* Find the best route according to the weak ordering.  Any
   linearisation of the strong ordering (see consider_route) will do,
   we use sm <= sm'.  We could probably use a lexical ordering, but
   that's probably overkill. */

struct babel_route *
find_best_route(const unsigned char *id,
                const unsigned char *prefix, unsigned char plen,
                const unsigned char *src_prefix, unsigned char src_plen,
                int feasible, struct neighbour *exclude)
{
    struct babel_route *route, *head, *r;
    int i = find_route_slot(id, prefix, plen, src_prefix, src_plen, NULL);

    if(i < 0)
        return NULL;

    route = NULL;
    head = routes[i];
    while(head && !route) {
        r = head;
        while(r) {
            if(route_acceptable(r, feasible, exclude) &&
               (!id || memcmp(r->src->id, id, 8) == 0)) {
                route = r;
                break;
            }
            r = r->multipath;
        }
        head = head->next;
    }

    if(!route)
        return NULL;

    head = routes[i];
    while(head) {
        r = head;
        while(r) {
            if(r != route &&
               route_acceptable(r, feasible, exclude) &&
               (!id || memcmp(r->src->id, id, 8) == 0) &&
               (route_smoothed_metric(r) < route_smoothed_metric(route)))
                route = r;
            r = r->multipath;
        }
        head = head->next;
    }

    return route;
}

void
update_route_metric(struct babel_route *route)
{
    int oldmetric = route_metric(route);
    int old_smoothed_metric = route_smoothed_metric(route);

    if(route_expired(route)) {
        if(route->refmetric < INFINITY) {
            route->seqno = seqno_plus(route->src->seqno, 1);
            retract_route(route);
            if(oldmetric < INFINITY)
                route_changed(route, route->src, oldmetric);
        }
    } else {
        struct neighbour *neigh = route->neigh;
        if(neigh == NULL || neigh->ifp == NULL)
            return;

        int add_metric = input_filter(route->src->id,
                                      route->src->prefix, route->src->plen,
                                      route->src->src_prefix,
                                      route->src->src_plen,
                                      neigh->address,
                                      neigh->ifp->ifindex);
        change_route_metric(route, route->refmetric,
                            neighbour_cost(route->neigh), add_metric);
        if(route_metric(route) != oldmetric ||
           route_smoothed_metric(route) != old_smoothed_metric)
            route_changed(route, route->src, oldmetric);
    }
}

/* Called whenever a neighbour's cost changes, to update the metric of
   all routes through that neighbour.  Calls local_notify_neighbour. */
void
update_neighbour_metric(struct neighbour *neigh, int changed)
{

    if(changed) {
        int i;

        for(i = 0; i < route_slots; i++) {
            struct babel_route *head = routes[i];
            while(head) {
                struct babel_route *r = head;
                while(r) {
                    if(r->neigh == neigh)
                        update_route_metric(r);
                    r = r->multipath;
                }
                head = head->next;
            }
        }
    }

    local_notify_neighbour(neigh, LOCAL_CHANGE);
}

void
update_interface_metric(struct interface *ifp)
{
    int i;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->neigh && r->neigh->ifp && r->neigh->ifp == ifp)
                    update_route_metric(r);
                r = r->multipath;
            }
            head = head->next;
        }
    }
}

/* This is called whenever we receive an update. */
struct babel_route *
update_route(const unsigned char *id,
             const unsigned char *prefix, unsigned char plen,
             const unsigned char *src_prefix, unsigned char src_plen,
             unsigned short seqno, unsigned short refmetric,
             unsigned short interval,
             struct neighbour *neigh, const unsigned char *nexthop,
             int hard_retract)
{
    struct babel_route *route;
    struct source *src;
    int metric, feasible;
    int add_metric;
    int hold_time = MAX((4 * interval) / 100 + interval / 50, 15);
    int is_v4;

    hard_retract = hard_retract && enable_hard_withdraw;

    if(!id) {
        if(refmetric < INFINITY) {
            fprintf(stderr, "Update with no id and finite metric.");
            return NULL;
        }
    } else if(memcmp(id, myid, 8) == 0) {
        return NULL;
    }

    if(martian_prefix(prefix, plen) || martian_prefix(src_prefix, src_plen)) {
        fprintf(stderr, "Rejecting martian route to %s from %s through %s.\n",
                format_prefix(prefix, plen),
                format_prefix(src_prefix, src_plen),
                nexthop ? format_address(nexthop) : "(unknown)");
        return NULL;
    }

    is_v4 = v4mapped(prefix);
    if(is_v4 != v4mapped(src_prefix))
        return NULL;

    if(neigh == NULL || neigh->ifp == NULL)
        return NULL;

    add_metric = input_filter(id, prefix, plen, src_prefix, src_plen,
                              neigh->address, neigh->ifp->ifindex);
    if(add_metric >= INFINITY)
        return NULL;

    route = find_route(id, prefix, plen, src_prefix, src_plen, neigh);

    if(refmetric >= INFINITY && !route) {
        /* Somebody's retracting a route that we've never seen. */
        return NULL;
    } else if(!id) {
        /* Pretend the retraction came from the currently installed source. */
        id = route->src->id;
    }

    if(!id) {
        fprintf(stderr, "No id in update_route -- this shouldn't happen.\n");
        return NULL;
    }

    if(route && memcmp(route->src->id, id, 8) == 0)
        /* Avoid scanning the source table. */
        src = route->src;
    else
        src = find_source(id, prefix, plen, src_prefix, src_plen, 1, seqno);

    if(src == NULL)
        return NULL;

    feasible = update_feasible(src, seqno, refmetric);
    metric = MIN((int)refmetric + neighbour_cost(neigh) + add_metric, INFINITY);

    if(route) {
        struct source *oldsrc;
        unsigned short oldmetric;
        int oldinstalled_primary;
        int lost = 0;

        oldinstalled_primary = (route->installed == 1);
        oldsrc = route->src;
        oldmetric = route_metric(route);

        if( refmetric >= INFINITY && hard_retract &&
            oldinstalled_primary && src == route->src) {

            route->seqno = seqno;
            route->hold_time = hold_time;

            change_route_metric(route,
                                refmetric, neighbour_cost(neigh), add_metric);

            if(route->installed == 1)
                uninstall_route(route);

            route_lost_ext(oldsrc, oldmetric, 1);
            return route;
        }

        /* If a successor switches sources, we must accept their update even
           if it makes a route unfeasible in order to break any routing loops
           in a timely manner.  If the source remains the same, we ignore
           the update. */
        if(!feasible && route->installed == 1) {
            debugf("Unfeasible update for installed route to %s "
                   "(%s %d %d -> %s %d %d).\n",
                   format_prefix(src->prefix, src->plen),
                   format_eui64(route->src->id),
                   route->seqno, route->refmetric,
                   format_eui64(src->id), seqno, refmetric);
            if(src != route->src) {
                uninstall_route(route);
                lost = 1;
            }
        }

        route->src = retain_source(src);
        if(refmetric < INFINITY)
            route->time = now.tv_sec;
        route->seqno = seqno;

        change_route_metric(route,
                            refmetric, neighbour_cost(neigh), add_metric);
        route->hold_time = hold_time;

        route_changed(route, oldsrc, oldmetric);
        if(!lost) {
            lost = oldinstalled_primary &&
                find_installed_route(NULL, prefix, plen, src_prefix, src_plen, NULL) == NULL;
        }
        if(lost)
            route_lost_ext(oldsrc, oldmetric, 0);
        else if(!feasible)
            send_unfeasible_request(neigh, route_old(route), seqno, metric, src);
        release_source(oldsrc);
    } else {
        struct babel_route *new_route;

        if(refmetric >= INFINITY)
            /* Somebody's retracting a route we never saw. */
            return NULL;
        if(nexthop == NULL) {
            fprintf(stderr,
                    "No nexthop in update_route, this shouldn't happen.\n");
            return NULL;
        }

        if(!feasible) {
            send_unfeasible_request(neigh, 0, seqno, metric, src);
        }

        route = calloc(1, sizeof(struct babel_route));
        if(route == NULL) {
            perror("malloc(route)");
            return NULL;
        }

        route->src = retain_source(src);
        route->refmetric = refmetric;
        route->cost = neighbour_cost(neigh);
        route->add_metric = add_metric;
        route->seqno = seqno;
        route->neigh = neigh;
        memcpy(route->nexthop, nexthop, 16);
        route->time = now.tv_sec;
        route->hold_time = hold_time;
        route->smoothed_metric = MAX(route_metric(route), INFINITY / 2);
        route->smoothed_metric_time = now.tv_sec;
        route->next = NULL;
        route->multipath = NULL;
        new_route = insert_route(route);
        if(new_route == NULL) {
            fprintf(stderr, "Couldn't insert route.\n");
            destroy_route(route);
            return NULL;
        }
        local_notify_route(route, LOCAL_ADD);
        consider_route(route);
    }
    return route;
}

/* We just received an unfeasible update.  If it's any good, send
   a request for a new seqno. */
void
send_unfeasible_request(struct neighbour *neigh, int force,
                        unsigned short seqno, unsigned short metric,
                        struct source *src)
{
    struct babel_route *route = find_best_route(src->id,
                                                src->prefix, src->plen,
                                                src->src_prefix, src->src_plen,
                                                0, NULL);

    if(seqno_minus(src->seqno, seqno) > 100) {
        /* Probably a source that lost its seqno.  Let it time-out. */
        return;
    }

    if(force || !route || route_metric(route) >= metric + 512) {
        send_unicast_multihop_request(neigh, src->prefix, src->plen,
                                      src->src_prefix, src->src_plen,
                                      src->metric >= INFINITY ?
                                      src->seqno :
                                      seqno_plus(src->seqno, 1),
                                      src->id, 127);
    }
}

/* This takes a feasible route and decides whether to install it.
   This uses the strong ordering, which is defined by sm <= sm' AND
   m <= m'.  This ordering is not total, which is what causes
   hysteresis. */

void
consider_route(struct babel_route *route)
{
    struct babel_route *installed;
    struct xroute *xroute;

    if(route->installed == 1)
        return;

    if(!route_feasible(route))
        return;

    xroute = find_xroute(route->src->prefix, route->src->plen,
                         route->src->src_prefix, route->src->src_plen);
    if(xroute && (allow_duplicates < 0 || xroute->metric >= allow_duplicates))
        return;

    installed = find_installed_route(NULL,
                                     route->src->prefix, route->src->plen,
                                     route->src->src_prefix, route->src->src_plen,
                                     NULL);

    if(installed == NULL)
        goto install;

    if(route_metric(route) >= INFINITY)
        return;

    if(route_metric(installed) >= INFINITY)
        goto install;

    if(route_metric(installed) >= route_metric(route) &&
       route_smoothed_metric(installed) > route_smoothed_metric(route))
        goto install;

    if(multipath_ecmp != ECMP_DISABLED &&
       route_metric(installed) < INFINITY &&
       route_metric(route) < INFINITY) {
          int slot = find_route_slot_for_route(installed);

                rehome_route_to_installed_group(slot, installed, route);

        /* A new route appeared that could join the ECMP group.
           Let refresh_installed_ranks() handle the kernel update.
           It will detect the new nexthop and do a single FLUSH+ADD. */
        debugf("consider_route: ECMP refresh for %s via %s (installed via %s)\n",
               format_prefix(route->src->prefix, route->src->plen),
               format_address(route->nexthop),
               format_address(installed->nexthop));
        refresh_installed_ranks(installed);
    }

    return;

 install:
    debugf("consider_route: INSTALLING %s via %s (was installed=%p)\n",
           format_prefix(route->src->prefix, route->src->plen),
           format_address(route->nexthop),
           (void*)installed);
    switch_routes(installed, route);
    if(installed && route->installed == 1)
        send_triggered_update(route, installed->src, route_metric(installed));
    else
        send_update(NULL, 1, route->src->prefix, route->src->plen,
                    route->src->src_prefix, route->src->src_plen);
    return;
}

void
retract_neighbour_routes(struct neighbour *neigh)
{
    int i;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->neigh == neigh) {
                    if(r->refmetric != INFINITY) {
                        unsigned short oldmetric = route_metric(r);
                        retract_route(r);
                        if(oldmetric != INFINITY)
                            route_changed(r, r->src, oldmetric);
                    }
                }
                r = r->multipath;
            }
            head = head->next;
        }
    }
}

void
send_triggered_update(struct babel_route *route, struct source *oldsrc,
                      unsigned oldmetric)
{
    unsigned newmetric, diff;
    /* 1 means send speedily, 2 means resend */
    int urgent;

    if(route->installed != 1)
        return;

    newmetric = route_metric(route);
    diff =
        newmetric >= oldmetric ? newmetric - oldmetric : oldmetric - newmetric;

    if(route->src != oldsrc || (oldmetric < INFINITY && newmetric >= INFINITY))
        /* Switching sources can cause transient routing loops.
           Retractions can cause blackholes. */
        urgent = 2;
    else if(newmetric > oldmetric && oldmetric < 6 * 256 && diff >= 512)
        /* Route getting significantly worse */
        urgent = 1;
    else if(unsatisfied_request(route->src->prefix, route->src->plen,
                                route->src->src_prefix, route->src->src_plen,
                                route->seqno, route->src->id))
        /* Make sure that requests are satisfied speedily */
        urgent = 1;
    else if(oldmetric >= INFINITY && newmetric < INFINITY)
        /* New route */
        urgent = 0;
    else if(newmetric < oldmetric && diff < 1024)
        /* Route getting better.  This may be a transient fluctuation, so
           don't advertise it to avoid making routes unfeasible later on. */
        return;
    else if(diff < 384)
        /* Don't fret about trivialities */
        return;
    else
        urgent = 0;

    if(urgent >= 2)
        send_update_resend(NULL, route->src->prefix, route->src->plen,
                           route->src->src_prefix, route->src->src_plen);
    else
        send_update(NULL, urgent, route->src->prefix, route->src->plen,
                    route->src->src_prefix, route->src->src_plen);

    if(oldmetric < INFINITY) {
        if(newmetric >= oldmetric + 288) {
            send_multicast_request(NULL, route->src->prefix, route->src->plen,
                                   route->src->src_prefix, route->src->src_plen);
        }
    }
}

/* A route has just changed.  Decide whether to switch to a different route or
   send an update. */
void
route_changed(struct babel_route *route,
              struct source *oldsrc, unsigned short oldmetric)
{
    if(route->installed == 1) {
        struct babel_route *better_route;
        /* Do this unconditionally -- microoptimisation is not worth it. */
        better_route =
            find_best_route(NULL,
                            route->src->prefix, route->src->plen,
                            route->src->src_prefix, route->src->src_plen,
                            1, NULL);
        if(better_route && route_metric(better_route) < route_metric(route))
            consider_route(better_route);
    }

    if(route->installed == 1) {
        /* We didn't change routes after all. */
        send_triggered_update(route, oldsrc, oldmetric);
    } else {
        /* Reconsider routes even when their metric didn't decrease,
           they may not have been feasible before. */
        consider_route(route);
    }
}

/* We just lost the installed route to a given destination. */
void
route_lost_ext(struct source *src, unsigned oldmetric, int hard_retract)
{
    struct babel_route *new_route;
    struct babel_route *installed;

    hard_retract = hard_retract && enable_hard_withdraw;

    if(hard_retract) {
        int i;
        struct babel_route *r;

        new_route = NULL;
        i = find_route_slot(NULL,
                            src->prefix, src->plen,
                            src->src_prefix, src->src_plen,
                            NULL);
        if(i >= 0) {
            struct babel_route *head = routes[i];
            while(head) {
                r = head;
                while(r) {
                    if(!route_expired(r) && route_feasible(r) &&
                       route_metric(r) < INFINITY &&
                       (!new_route || route_metric(r) < route_metric(new_route)))
                        new_route = r;
                    r = r->multipath;
                }
                head = head->next;
            }
        }
    } else {
        new_route = find_best_route(NULL,
                                    src->prefix, src->plen,
                                    src->src_prefix, src->src_plen,
                                    1,
                                    NULL);
    }
    if(new_route &&
       ((hard_retract && route_metric(new_route) < INFINITY) ||
        (!hard_retract && route_feasible(new_route)))) {
        if(hard_retract) {
            installed = find_installed_route(NULL,
                                             src->prefix, src->plen,
                                             src->src_prefix, src->src_plen,
                                             NULL);
            switch_routes(installed, new_route);
        } else {
            consider_route(new_route);
        }
    } else if(oldmetric < INFINITY) {
        /* Avoid creating a blackhole. */
        if(hard_retract) {
            send_update_resend_with_id(NULL,
                                       src->prefix, src->plen,
                                       src->src_prefix, src->src_plen,
                                       src->seqno, src->id,
                                       UPDATE_FLAG_HARD_WITHDRAW);
        } else {
            send_update_resend(NULL, src->prefix, src->plen,
                               src->src_prefix, src->src_plen);
        }
        /* If the route was usable enough, try to get an alternate one.
           If it was not, we could be dealing with oscillations around
           the value of INFINITY. */
        if(!hard_retract && oldmetric <= INFINITY / 2)
            send_request_resend(src->prefix, src->plen,
                                src->src_prefix, src->src_plen,
                                src->metric >= INFINITY ?
                                src->seqno : seqno_plus(src->seqno, 1),
                                src->id);
    }
}

void
route_lost(struct source *src, unsigned oldmetric)
{
    route_lost_ext(src, oldmetric, 0);
}

/* This is called periodically to flush old routes.  It will also send
   requests for routes that are about to expire. */
void
expire_routes(void)
{
    struct babel_route *r;
    int i;

    debugf("Expiring old routes.\n");

    i = 0;
    while(i < route_slots) {
        struct babel_route *head = routes[i];
        while(head) {
            r = head;
            while(r) {
                if(r->time > now.tv_sec || route_old(r)) {
                    flush_route(r);
                    goto again;
                }

                update_route_metric(r);

                if(r->installed && r->refmetric < INFINITY) {
                    if(route_old(r))
                        send_unicast_request(r->neigh,
                                             r->src->prefix, r->src->plen,
                                             r->src->src_prefix, r->src->src_plen);
                }
                r = r->multipath;
            }
            head = head->next;
        }
        i++;
    again:
        ;
    }
}

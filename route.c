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
int route_metric_coalesce_msec = DEFAULT_ROUTE_METRIC_COALESCE_MSEC;
int ecmp_coalesce_msec = DEFAULT_ECMP_COALESCE_MSEC;

int smoothing_half_life = 0;
int two_to_the_one_over_hl = 0; /* 2^(1/hl) * 0x10000 */
static int kernel_route_operation_depth = 0;
static struct timeval route_metric_coalesce_next_due = {0, 0};
static struct timeval ecmp_coalesce_next_due = {0, 0};

/* Intrusive singly-linked lists of routes with a pending coalesce action.
   Each route has an on_metric_list / on_ecmp_list flag that doubles as
   an O(1) membership test: schedule_*() just checks the flag and skips
   the enqueue if already set, with no list walk needed. */
static struct babel_route *metric_pending_head = NULL;
static struct babel_route *ecmp_pending_head = NULL;

/* Remove route from the metric pending list.  O(pending_count), called
   only on infrequent cancel paths (install/uninstall/switch/destroy). */
static void
metric_pending_remove(struct babel_route *route)
{
    struct babel_route **pp = &metric_pending_head;
    while(*pp) {
        if(*pp == route) {
            *pp = route->metric_pending_next;
            route->metric_pending_next = NULL;
            route->on_metric_list = 0;
            return;
        }
        pp = &(*pp)->metric_pending_next;
    }
}

static void
ecmp_pending_remove(struct babel_route *route)
{
    struct babel_route **pp = &ecmp_pending_head;
    while(*pp) {
        if(*pp == route) {
            *pp = route->ecmp_pending_next;
            route->ecmp_pending_next = NULL;
            route->on_ecmp_list = 0;
            return;
        }
        pp = &(*pp)->ecmp_pending_next;
    }
}

/* Cancel any pending coalesce for route and remove it from the lists.
   Call this before any operation that makes a pending coalesce stale
   (install, uninstall, switch, destroy). */
static void
cancel_coalesce_pending(struct babel_route *route)
{
    if(route->on_metric_list) {
        metric_pending_remove(route);
        route->metric_update_pending = 0;
        route->metric_update_started.tv_sec = 0;
        route->metric_update_started.tv_usec = 0;
        route->metric_update_due.tv_sec = 0;
        route->metric_update_due.tv_usec = 0;
        route->metric_update_base_kernel_metric = 0;
        route->metric_update_base_ifindex = 0;
        memset(route->metric_update_base_nexthop, 0,
               sizeof(route->metric_update_base_nexthop));
    }
    if(route->on_ecmp_list) {
        ecmp_pending_remove(route);
        route->ecmp_reprogram_pending = 0;
        route->ecmp_reprogram_started.tv_sec = 0;
        route->ecmp_reprogram_started.tv_usec = 0;
        route->ecmp_reprogram_due.tv_sec = 0;
        route->ecmp_reprogram_due.tv_usec = 0;
    }
}

static void refresh_installed_ranks_ext(struct babel_route *route,
                                        int force_reprogram);
static int route_expired(struct babel_route *route);
static unsigned int route_ifindex_or_zero(const struct babel_route *route);
static void clear_installed_ranks(const struct babel_route *route,
                                  int clear_tables);

static int
retryable_kernel_add_errno(int err)
{
    return err == ENETDOWN || err == ENODEV || err == EINTR ||
           err == EAGAIN || err == ENOBUFS || err == EBUSY;
}

static int
allow_reconsider_retry_now(void)
{
    static struct timeval retry_not_before = {0, 0};

    if((retry_not_before.tv_sec != 0 || retry_not_before.tv_usec != 0) &&
       timeval_compare(&now, &retry_not_before) < 0)
        return 0;

    timeval_add_msec(&retry_not_before, &now, 250);
    return 1;
}

static void
schedule_route_metric_coalesce(struct babel_route *route)
{
    int coalesce_ms;
    struct timeval due;
    struct timeval max_due;

    if(route == NULL)
        return;

    /* Enforce minimum 200ms and keep millisecond precision. */
    coalesce_ms = MAX(route_metric_coalesce_msec, 200);

    /* Arm the coalescing window start only on the first call. */
    if(route->metric_update_started.tv_sec == 0 &&
       route->metric_update_started.tv_usec == 0) {
        unsigned int base_ifindex = route_ifindex_or_zero(route);
        route->metric_update_started = now;
        route->metric_update_base_kernel_metric =
            metric_to_kernel(route_metric(route));
        route->metric_update_base_ifindex = base_ifindex;
        memcpy(route->metric_update_base_nexthop, route->nexthop,
               sizeof(route->metric_update_base_nexthop));
    }

    timeval_add_msec(&due, &now, coalesce_ms);
    timeval_add_msec(&max_due, &route->metric_update_started,
                     MAX_ROUTE_METRIC_COALESCE_MSEC);

    route->metric_update_due =
        (timeval_compare(&due, &max_due) > 0) ? max_due : due;
    route->metric_update_pending = 1;

    /* O(1) membership test: the flag tells us instantly whether this route
       is already in the list, so no list walk is needed. */
    if(!route->on_metric_list) {
        route->on_metric_list = 1;
        route->metric_pending_next = metric_pending_head;
        metric_pending_head = route;
    }

    if((route_metric_coalesce_next_due.tv_sec == 0 &&
        route_metric_coalesce_next_due.tv_usec == 0) ||
       timeval_compare(&route->metric_update_due,
                       &route_metric_coalesce_next_due) < 0)
        route_metric_coalesce_next_due = route->metric_update_due;
}

void
route_flush_coalesced_metric_updates(void)
{
    struct babel_route *pending;
    struct timeval next_due = {0, 0};
    struct babel_route *metric_requeue_tail = NULL;

    if((route_metric_coalesce_next_due.tv_sec == 0 &&
        route_metric_coalesce_next_due.tv_usec == 0) ||
       timeval_compare(&now, &route_metric_coalesce_next_due) < 0)
        return;

    /* Detach the entire list at once; we rebuild it below for entries not
       yet due.  This is safe: schedule_*() may run concurrently in the
       same event-loop iteration and will simply push new entries onto the
       now-empty head, which we pick up on the next call. */
    pending = metric_pending_head;
    metric_pending_head = NULL;

    while(pending) {
        struct babel_route *r = pending;
        struct timeval batch_deadline;
        int processed_in_batch = 0;

        /* Process batch of up to 500 routes to avoid overwhelming the system.
           After 500 routes, enforce at least 200ms before next batch. */
        while(pending && processed_in_batch < 500) {
            r = pending;
            pending = r->metric_pending_next;
            r->metric_pending_next = NULL;
            r->on_metric_list = 0;       /* fully removed from list */

            if(!r->metric_update_pending)
                continue;  /* cancelled via cancel_coalesce_pending while listed */

            if(r->src == NULL) {
                r->metric_update_pending = 0;
                r->metric_update_started.tv_sec = 0;
                r->metric_update_started.tv_usec = 0;
                r->metric_update_due.tv_sec = 0;
                r->metric_update_due.tv_usec = 0;
                continue;
            }

            if(r->installed != 1 || route_metric(r) >= INFINITY || route_expired(r)) {
                /* Route became invalid between schedule and flush: discard silently.
                   The right kernel state is handled by uninstall/flush/retract. */
                r->metric_update_pending = 0;
                r->metric_update_started.tv_sec = 0;
                r->metric_update_started.tv_usec = 0;
                r->metric_update_due.tv_sec = 0;
                r->metric_update_due.tv_usec = 0;
                continue;
            }

            if(timeval_compare(&now, &r->metric_update_due) < 0) {
                /* Deadline not reached yet: re-enqueue. */
                r->on_metric_list = 1;
                r->metric_pending_next = metric_pending_head;
                metric_pending_head = r;
                if(metric_requeue_tail == NULL)
                    metric_requeue_tail = r;
                if((next_due.tv_sec == 0 && next_due.tv_usec == 0) ||
                   timeval_compare(&r->metric_update_due, &next_due) < 0)
                    next_due = r->metric_update_due;
                continue;
            }

            /* Deadline reached: flush old metric, then re-add with current metric.
               Save installed_tables before FLUSH: change_route(FLUSH) reads
               r->installed_table_count to know which tables to remove from, but
               does not clear it.  We save/restore explicitly to be defensive
               against any future change to that contract. */
            {
                int rc;
                int rc2;
                int saved_count = r->installed_table_count;
                int saved_tables[MAX_TABLES_PER_FILTER];
                int current_metric = metric_to_kernel(route_metric(r));
                int flush_metric = r->metric_update_base_kernel_metric;
                unsigned int current_ifindex = route_ifindex_or_zero(r);

                if(r->metric_update_base_kernel_metric == current_metric &&
                   r->metric_update_base_ifindex == current_ifindex &&
                   memcmp(r->metric_update_base_nexthop, r->nexthop,
                          sizeof(r->metric_update_base_nexthop)) == 0) {
                    r->metric_update_pending = 0;
                    r->metric_update_started.tv_sec = 0;
                    r->metric_update_started.tv_usec = 0;
                    r->metric_update_due.tv_sec = 0;
                    r->metric_update_due.tv_usec = 0;
                    r->metric_update_base_kernel_metric = 0;
                    r->metric_update_base_ifindex = 0;
                    memset(r->metric_update_base_nexthop, 0,
                           sizeof(r->metric_update_base_nexthop));
                    continue;
                }

                if(saved_count < 0 || saved_count > MAX_TABLES_PER_FILTER)
                    saved_count = 0;

                if(saved_count > 0)
                    memcpy(saved_tables, r->installed_tables,
                           saved_count * sizeof(int));

                rc = change_route(ROUTE_FLUSH, r,
                                  flush_metric,
                                  NULL, 0, 0, NULL, NULL, NULL);
                if(rc < 0 && errno != ESRCH && errno != ENOENT)
                    perror("kernel_route(FLUSH coalesced)");

                if(flush_metric != current_metric) {
                    rc2 = change_route(ROUTE_FLUSH, r,
                                       current_metric,
                                       NULL, 0, 0, NULL, NULL, NULL);
                    if(rc2 < 0 && errno != ESRCH && errno != ENOENT)
                        perror("kernel_route(FLUSH coalesced current)");
                }

                if(saved_count > 0 && r->installed_table_count == 0) {
                    memcpy(r->installed_tables, saved_tables,
                           saved_count * sizeof(int));
                    r->installed_table_count = saved_count;
                }

                rc = change_route(ROUTE_ADD, r,
                                  current_metric,
                                  NULL, 0, 0, NULL,
                                  r->installed_tables,
                                  &r->installed_table_count);
                if(rc < 0 && errno != EEXIST) {
                          int saved_errno = errno;
                    perror("kernel_route(ADD coalesced)");
                    /* FLUSH succeeded but re-ADD failed: keep local state
                       aligned with kernel so we can retry via normal logic. */
                    clear_installed_ranks(r, 1);
                    local_notify_route(r, LOCAL_CHANGE);
                          if(retryable_kernel_add_errno(saved_errno) &&
                              allow_reconsider_retry_now())
                                consider_route(r);
                }

                r->metric_update_pending = 0;
                r->metric_update_started.tv_sec = 0;
                r->metric_update_started.tv_usec = 0;
                r->metric_update_due.tv_sec = 0;
                r->metric_update_due.tv_usec = 0;
                r->metric_update_base_kernel_metric = 0;
                r->metric_update_base_ifindex = 0;
                memset(r->metric_update_base_nexthop, 0,
                       sizeof(r->metric_update_base_nexthop));
            }
            processed_in_batch++;
        }

        /* If we have more routes pending and we've processed a full batch,
           re-attach remaining routes and enforce 200ms minimum delay. */
        if(pending && processed_in_batch >= 500) {
            if(metric_pending_head == NULL) {
                metric_pending_head = pending;
            } else if(metric_requeue_tail != NULL) {
                metric_requeue_tail->metric_pending_next = pending;
            } else {
                struct babel_route *tail = metric_pending_head;
                while(tail->metric_pending_next)
                    tail = tail->metric_pending_next;
                tail->metric_pending_next = pending;
            }
            pending = NULL;
            /* Enforce 200ms minimum before next batch */
            timeval_add_msec(&batch_deadline, &now, 200);
            if((next_due.tv_sec == 0 && next_due.tv_usec == 0) ||
               timeval_compare(&batch_deadline, &next_due) > 0)
                next_due = batch_deadline;
        } else {
            pending = NULL;  /* ensure pending is NULL when loop exits normally */
        }
    }

    route_metric_coalesce_next_due = next_due;
}

static void
schedule_ecmp_reprogram(struct babel_route *route)
{
    int coalesce_ms;
    struct timeval due;
    struct timeval max_due;

    if(route == NULL)
        return;

    /* Enforce minimum 200ms and keep millisecond precision. */
    coalesce_ms = MAX(ecmp_coalesce_msec, 200);

    if(route->ecmp_reprogram_started.tv_sec == 0 &&
       route->ecmp_reprogram_started.tv_usec == 0)
        route->ecmp_reprogram_started = now;

    timeval_add_msec(&due, &now, coalesce_ms);
    timeval_add_msec(&max_due, &route->ecmp_reprogram_started,
                     MAX_ECMP_COALESCE_MSEC);

    route->ecmp_reprogram_due =
        (timeval_compare(&due, &max_due) > 0) ? max_due : due;
    route->ecmp_reprogram_pending = 1;

    /* O(1) membership test: only enqueue if not already in the list. */
    if(!route->on_ecmp_list) {
        route->on_ecmp_list = 1;
        route->ecmp_pending_next = ecmp_pending_head;
        ecmp_pending_head = route;
    }

    if((ecmp_coalesce_next_due.tv_sec == 0 &&
        ecmp_coalesce_next_due.tv_usec == 0) ||
       timeval_compare(&route->ecmp_reprogram_due, &ecmp_coalesce_next_due) < 0)
        ecmp_coalesce_next_due = route->ecmp_reprogram_due;
}

void
route_flush_deferred_ecmp_reprograms(void)
{
    struct babel_route *pending;
    struct timeval next_due = {0, 0};
    struct babel_route *ecmp_requeue_tail = NULL;

    if((ecmp_coalesce_next_due.tv_sec == 0 &&
        ecmp_coalesce_next_due.tv_usec == 0) ||
       timeval_compare(&now, &ecmp_coalesce_next_due) < 0)
        return;

    pending = ecmp_pending_head;
    ecmp_pending_head = NULL;

    while(pending) {
        struct babel_route *r = pending;
        struct timeval batch_deadline;
        int processed_in_batch = 0;

        /* Process batch of up to 500 routes to avoid overwhelming the system.
           After 500 routes, enforce at least 200ms before next batch. */
        while(pending && processed_in_batch < 500) {
            r = pending;
            pending = r->ecmp_pending_next;
            r->ecmp_pending_next = NULL;
            r->on_ecmp_list = 0;

            if(!r->ecmp_reprogram_pending)
                continue;  /* cancelled via cancel_coalesce_pending while listed */

            if(r->src == NULL || r->installed <= 0 ||
               route_metric(r) >= INFINITY || route_expired(r)) {
                r->ecmp_reprogram_pending = 0;
                r->ecmp_reprogram_started.tv_sec = 0;
                r->ecmp_reprogram_started.tv_usec = 0;
                r->ecmp_reprogram_due.tv_sec = 0;
                r->ecmp_reprogram_due.tv_usec = 0;
                continue;
            }

            if(timeval_compare(&now, &r->ecmp_reprogram_due) < 0) {
                /* Not yet due: re-enqueue. */
                r->on_ecmp_list = 1;
                r->ecmp_pending_next = ecmp_pending_head;
                ecmp_pending_head = r;
                if(ecmp_requeue_tail == NULL)
                    ecmp_requeue_tail = r;
                if((next_due.tv_sec == 0 && next_due.tv_usec == 0) ||
                   timeval_compare(&r->ecmp_reprogram_due, &next_due) < 0)
                    next_due = r->ecmp_reprogram_due;
                continue;
            }

            debugf("ecmp_reprogram: flushing deferred reprogram for %s\n",
                   format_prefix(r->src->prefix, r->src->plen));
            refresh_installed_ranks_ext(r, 1);
            r->ecmp_reprogram_pending = 0;
            r->ecmp_reprogram_started.tv_sec = 0;
            r->ecmp_reprogram_started.tv_usec = 0;
            r->ecmp_reprogram_due.tv_sec = 0;
            r->ecmp_reprogram_due.tv_usec = 0;
            processed_in_batch++;
        }

        /* If we have more routes pending and we've processed a full batch,
           re-attach remaining routes and enforce 200ms minimum delay. */
        if(pending && processed_in_batch >= 500) {
            if(ecmp_pending_head == NULL) {
                ecmp_pending_head = pending;
            } else if(ecmp_requeue_tail != NULL) {
                ecmp_requeue_tail->ecmp_pending_next = pending;
            } else {
                struct babel_route *tail = ecmp_pending_head;
                while(tail->ecmp_pending_next)
                    tail = tail->ecmp_pending_next;
                tail->ecmp_pending_next = pending;
            }
            pending = NULL;
            /* Enforce 200ms minimum before next batch */
            timeval_add_msec(&batch_deadline, &now, 200);
            if((next_due.tv_sec == 0 && next_due.tv_usec == 0) ||
               timeval_compare(&batch_deadline, &next_due) > 0)
                next_due = batch_deadline;
        } else {
            pending = NULL;  /* ensure pending is NULL when loop exits normally */
        }
    }

    ecmp_coalesce_next_due = next_due;
}


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

    /* Add to per-neighbor route list */
    if(route->neigh) {
        route->neigh_route_next = route->neigh->routes;
        route->neigh_route_prev = NULL;
        if(route->neigh->routes)
            route->neigh->routes->neigh_route_prev = route;
        route->neigh->routes = route;
    }

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
    /* Remove from per-neighbor route list */
    if(route->neigh) {
        if(route->neigh_route_prev)
            route->neigh_route_prev->neigh_route_next = route->neigh_route_next;
        else {
            if(route->neigh->routes == route)
                route->neigh->routes = route->neigh_route_next;
        }

        if(route->neigh_route_next)
            route->neigh_route_next->neigh_route_prev = route->neigh_route_prev;
    }

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

    /* Debug: show all routes in this slot (only in verbose mode to avoid overhead) */
    if(debug >= 2) {
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
        cancel_coalesce_pending(route);
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
           do a single FLUSH+ADD.
           Force reprogramming here: the removed member is already detached
           from the in-memory group, so normal set-diff detection may not see
           the old kernel nexthop set. */
        refresh_installed_ranks_ext(primary, 1);
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
    /* Use per-neighbor route list: avoids O(route_slots) full-table scan. */
    while(neigh->routes) {
        /* flush_route -> destroy_route unlinks from neigh->routes, so
           always flush the current head until the list is empty. */
        flush_route(neigh->routes);
    }
}

void
flush_interface_routes(struct interface *ifp, int v4only)
{
    struct neighbour *neigh;

    /* Iterate only neighbors on this interface, then use their route lists.
       This avoids an O(route_slots) full-table scan. */
    FOR_ALL_NEIGHBOURS(neigh) {
        if(neigh->ifp != ifp)
            continue;
    again:
        {
            struct babel_route *r = neigh->routes;
            while(r) {
                if(!v4only || v4mapped(r->nexthop)) {
                    flush_route(r);
                    /* flush_route -> destroy_route modifies neigh->routes;
                       restart the scan from the new head. */
                    goto again;
                }
                r = r->neigh_route_next;
            }
        }
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

static int
group_has_installed_member(const struct babel_route *route)
{
    int i;
    struct babel_route *head;
    struct babel_route *r;

    i = find_route_slot_for_route(route);
    if(i < 0)
        return 0;

    head = find_group_head_for_route(i, route, NULL, NULL);
    if(head == NULL)
        return 0;

    r = head;
    while(r) {
        if(r->installed > 0)
            return 1;
        r = r->multipath;
    }

    return 0;
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

static void
refresh_installed_ranks_ext(struct babel_route *route, int force_reprogram)
{
    int slot, i, count;
    struct babel_route *slot_head;
    struct babel_route *head;
    struct babel_route *r;
    struct babel_route *primary = NULL;
    struct babel_route *old_primary = NULL;
    struct kernel_nexthop nexthops[MAX_ECMP_NEXTHOPS];
    struct kernel_nexthop old_nexthops[MAX_ECMP_NEXTHOPS];
    int old_tables[MAX_TABLES_PER_FILTER];
    int old_table_count = 0;
    int old_nexthop_count = 0;
    int set_changed = 0;
    int group_size = 0;
    int installed_primary_count = 0;
    int installed_member_count = 0;

    slot = find_route_slot_for_route(route);
    if(slot < 0)
        return;

    head = NULL;

    /* Canonicalise recalculation target:
       1) scan group heads first to find the best candidate group;
       2) scan only that group's multipath members to pick the best route.
       This prevents suboptimal callers from anchoring refresh on the wrong
       group when a better group exists in the same slot. */
    if(multipath_ecmp != ECMP_DISABLED) {
        struct babel_route *best_head = NULL;
        int best_head_metric = INFINITY;

        slot_head = routes[slot];
        while(slot_head) {
            int m = route_metric(slot_head);
            if(slot_head->neigh && slot_head->neigh->ifp &&
               !route_expired(slot_head) && route_feasible(slot_head) &&
               m < INFINITY && m < best_head_metric) {
                best_head = slot_head;
                best_head_metric = m;
            }
            slot_head = slot_head->next;
        }

        if(best_head) {
            struct babel_route *best_member = best_head;
            int best_member_metric = best_head_metric;

            r = best_head->multipath;
            while(r) {
                int m = route_metric(r);
                if(r->neigh && r->neigh->ifp &&
                   !route_expired(r) && route_feasible(r) &&
                   m < INFINITY && m < best_member_metric) {
                    best_member = r;
                    best_member_metric = m;
                }
                r = r->multipath;
            }

            route = best_member;
            head = best_head;
            debugf("refresh_installed_ranks: canonicalised to best group head=%p via %s metric=%d\n",
                   (void*)head, format_address(route->nexthop), best_member_metric);
        }
    }

    if(head == NULL)
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

    /* Record currently installed nexthops before refresh.
       Use installed_table_count > 0 as the kernel-presence signal rather
       than installed > 0: the installed flags may have already been zeroed
       by a prior stale bookkeeping path while the kernel route still lives. */
    if(multipath_ecmp != ECMP_DISABLED) {
        slot_head = routes[slot];
        while(slot_head) {
            r = slot_head;
            while(r) {
                int ifindex = -1;

                if(r->neigh && r->neigh->ifp) {
                    ifindex = r->neigh->ifp->ifindex;
                }

                if(r->installed > 0)
                    installed_member_count++;
                if(r->installed == 1)
                    installed_primary_count++;

                /* Primary: prefer an explicit installed primary,
                   fall back to any member with kernel presence. */
                if(old_primary == NULL && r->installed == 1)
                    old_primary = r;
                else if(old_primary == NULL && r->installed_table_count > 0)
                    old_primary = r;

                if(old_table_count == 0 && r->installed_table_count > 0) {
                    int copy_count = r->installed_table_count;
                    if(copy_count > MAX_TABLES_PER_FILTER)
                        copy_count = MAX_TABLES_PER_FILTER;
                    memcpy(old_tables, r->installed_tables, copy_count * sizeof(int));
                    old_table_count = copy_count;
                }

                /* Debug output: only compute and log ifname in verbose mode */
                if(debug >= 2) {
                    const char *ifname = "(null)";
                    if(r->neigh && r->neigh->ifp) {
                        ifname = r->neigh->ifp->name;
                    }

                    debugf("  member %p: installed=%d tables=%d metric=%d via %s if %s\n",
                           (void*)r, r->installed, r->installed_table_count,
                           route_metric(r), format_address(r->nexthop), ifname);
                }

                     /* Collect old nexthops for change detection.
                         Only trust entries that are actually marked installed and
                         have a valid outgoing interface; stale table bookkeeping
                         alone is not a reliable old-set signal. */
                     if(r->installed > 0 && ifindex > 0) {
                    int duplicate = 0;
                    int j;
                    for(j = 0; j < old_nexthop_count; j++) {
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
            slot_head = slot_head->next;
        }

        if(installed_primary_count > 1) {
            debugf("BUG: slot %d has %d installed primaries (%d installed members) for %s\n",
                   slot, installed_primary_count, installed_member_count,
                   format_prefix(route->src->prefix, route->src->plen));
#ifdef BABELD_ECMP_STRICT_ASSERT
            assert(installed_primary_count <= 1);
#endif
        }
    }

    /* Installed rank bookkeeping is slot-wide (single kernel route per slot). */
    slot_head = routes[slot];
    while(slot_head) {
        r = slot_head;
        while(r) {
            r->installed = 0;
            r = r->multipath;
        }
        slot_head = slot_head->next;
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

        if(primary && primary->installed_table_count == 0 && old_table_count > 0) {
            memcpy(primary->installed_tables, old_tables,
                   old_table_count * sizeof(int));
            primary->installed_table_count = old_table_count;
        }
        
        /* Detect transitions that require reprogramming kernel route. */
        if(old_nexthop_count != 1) {
            if(old_nexthop_count == 0 && old_table_count > 0 &&
               old_primary && primary &&
               old_primary->neigh && old_primary->neigh->ifp &&
               primary->neigh && primary->neigh->ifp &&
               old_primary->neigh->ifp->ifindex == primary->neigh->ifp->ifindex &&
               memcmp(old_primary->nexthop, primary->nexthop, 16) == 0) {
                debugf("  set_changed: preserving bookkeeping-only single nexthop state\n");
            } else {
            /* Count changed: 0->1, 2+->1, etc. */
                set_changed = 1;
            }
        } else if(primary && old_nexthop_count == 1) {
            /* Count stayed 1 — but the actual nexthop may have changed
               (e.g. A retracted, B remains: old gate=A, new gate=B). */
            if(primary->neigh && primary->neigh->ifp) {
                unsigned int new_ifindex = primary->neigh->ifp->ifindex;
                if(old_nexthops[0].ifindex != new_ifindex ||
                   memcmp(old_nexthops[0].gate, primary->nexthop, 16) != 0) {
                    debugf("  set_changed: single nexthop address changed\n");
                    set_changed = 1;
                }
            }
            if(!set_changed && old_primary &&
               old_primary->installed_table_count == 0) {
                /* Bookkeeping was lost: force a one-time resync */
                debugf("  set_changed: forcing resync (table state lost)\n");
                set_changed = 1;
            }
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

    if(primary && primary->installed_table_count == 0 && old_table_count > 0) {
        memcpy(primary->installed_tables, old_tables, old_table_count * sizeof(int));
        primary->installed_table_count = old_table_count;
    }

    if(old_table_count > 0) {
        r = head;
        while(r) {
            if(r != primary && r->installed > 1 && r->installed_table_count == 0) {
                memcpy(r->installed_tables, old_tables, old_table_count * sizeof(int));
                r->installed_table_count = old_table_count;
            }
            r = r->multipath;
        }
    }

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
    /* Keep kernel-presence metadata only on currently installed members. */
    slot_head = routes[slot];
    while(slot_head) {
        r = slot_head;
        while(r) {
            if(r->installed == 0)
                r->installed_table_count = 0;
            r = r->multipath;
        }
        slot_head = slot_head->next;
    }

    if(!set_changed && primary && route_metric(primary) < INFINITY &&
       primary->installed_table_count == 0) {
        if(old_table_count > 0) {
            memcpy(primary->installed_tables, old_tables,
                   old_table_count * sizeof(int));
            primary->installed_table_count = old_table_count;
        } else {
            debugf("  set_changed: forcing resync (installed primary has no tables)\n");
            set_changed = 1;
        }
    }

    if(!set_changed && force_reprogram && old_nexthop_count > 0) {
        debugf("  force_reprogram requested but nexthop set unchanged; skipping kernel reprogram\n");
    }

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

            /* Clear table bookkeeping for every member — they all shared
               the same kernel entry so they are all gone now. */
            r = head;
            while(r) {
                r->installed_table_count = 0;
                r = r->multipath;
            }
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
            if(rc < 0 && errno != EEXIST) {
                     int saved_errno = errno;
                perror("kernel_route(ADD ecmp refresh)");
                /* FLUSH succeeded but ECMP re-ADD failed: clear stale
                   installed marks and let normal selection retry. */
                clear_installed_ranks(primary, 1);
                local_notify_route(primary, LOCAL_CHANGE);
                     if(retryable_kernel_add_errno(saved_errno) &&
                         allow_reconsider_retry_now())
                          consider_route(primary);
            }

            /* Propagate the installed table list to all secondary ECMP members.
               They all share the same kernel multipath entry so they must
               report the same tables — this is also what lets Bug-1 detection
               work correctly on the next refresh cycle. */
            if(primary->installed_table_count > 0) {
                r = head;
                while(r) {
                    if(r != primary && r->installed > 1) {
                        memcpy(r->installed_tables, primary->installed_tables,
                               primary->installed_table_count * sizeof(int));
                        r->installed_table_count = primary->installed_table_count;
                    }
                    r = r->multipath;
                }
            }
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

void
refresh_installed_ranks(struct babel_route *route)
{
    refresh_installed_ranks_ext(route, 0);
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

        if(operation == ROUTE_FLUSH || operation == ROUTE_MODIFY) {
            note_self_kernel_route_delete(route->src->prefix, route->src->plen,
                                          route->src->src_prefix,
                                          route->src->src_plen,
                                          RTPROT_BABEL,
                                          table, metric);
            /* The Linux flush path also sweeps any unreachable hold-down for
               the same destination.  Suppress reconciliation of that delete
               too when the notification arrives asynchronously. */
            note_self_kernel_route_delete(route->src->prefix, route->src->plen,
                                          route->src->src_prefix,
                                          route->src->src_plen,
                                          RTPROT_BABEL,
                                          table, KERNEL_INFINITY);
        }

        if(operation == ROUTE_FLUSH) {
            int rc_multi, rc_single;
            int errno_multi = 0, errno_single = 0;

            /* Generic delete pass: try both multipath-style and single-hop
               deletes so we remove whichever encoding the kernel currently has. */
            kernel_route_operation_depth++;

            rc_multi = kernel_route_multipath(ROUTE_FLUSH,
                                              table,
                                              route->src->prefix, route->src->plen,
                                              route->src->src_prefix, route->src->src_plen,
                                              pref_src,
                                              metric,
                                              metric,
                                              NULL,
                                              0,
                                              0,
                                              NULL);
            if(rc_multi < 0)
                errno_multi = errno;

            rc_single = kernel_route(ROUTE_FLUSH, table,
                                     route->src->prefix, route->src->plen,
                                     route->src->src_prefix, route->src->src_plen, pref_src,
                                     route->nexthop, ifindex,
                                     metric, NULL, 0, 0,
                                     0, NULL);
            if(rc_single < 0)
                errno_single = errno;

            kernel_route_operation_depth--;

            if(rc_multi >= 0 || rc_single >= 0) {
                rc = 0;
            } else if((errno_multi == ESRCH || errno_multi == ENOENT || errno_multi == ENOSYS) &&
                      (errno_single == ESRCH || errno_single == ENOENT)) {
                /* Route not present in either encoding: treat as successful flush. */
                rc = 0;
            } else {
                rc = -1;
                errno = (errno_multi != 0 && errno_multi != ENOSYS) ?
                        errno_multi : errno_single;
            }

            if(rc < 0) {
                if(first_rc == 0)
                    first_rc = rc;
            }
            continue;
        }

        if(multipath_ecmp != ECMP_DISABLED) {
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

    cancel_coalesce_pending(route);

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
    cancel_coalesce_pending(route);
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
    cancel_coalesce_pending(old);
    cancel_coalesce_pending(new);
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
    int should_refresh_ecmp = 0;
    int suppress_hold_down = 0;
    int force_reprogram = 0;
    int expired = route_expired(route);

    if(multipath_ecmp != ECMP_DISABLED)
        should_refresh_ecmp = (route->installed > 0 || group_has_installed_member(route));

    if(route->installed > 0 && newmetric >= KERNEL_INFINITY) {
        struct xroute *xroute;

        if(multipath_ecmp != ECMP_DISABLED && route->installed == 1)
            force_reprogram = 1;

        xroute = find_xroute(route->src->prefix, route->src->plen,
                             route->src->src_prefix, route->src->src_plen);
        if(xroute && (allow_duplicates < 0 || xroute->metric >= allow_duplicates)) {
            suppress_hold_down = 1;
            debugf("change_route_metric: suppressing unreachable hold-down for %s (local xroute exists)\n",
                   format_prefix(route->src->prefix, route->src->plen));
        }
    }

    if(multipath_ecmp != ECMP_DISABLED && should_refresh_ecmp &&
       (expired || newmetric >= KERNEL_INFINITY)) {
        force_reprogram = 1;
    }

    if(multipath_ecmp != ECMP_DISABLED && should_refresh_ecmp) {
        if(force_reprogram &&
           oldmetric == KERNEL_INFINITY &&
           newmetric == KERNEL_INFINITY &&
           route->refmetric == refmetric &&
           route->cost == cost &&
           route->add_metric == add) {
            goto update_local_state;
        }

        if(!force_reprogram &&
           oldmetric == newmetric &&
           route->refmetric == refmetric &&
           route->cost == cost &&
           route->add_metric == add) {
            goto update_local_state;
        }

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

         debugf("change_route_metric(%s from %s, %d -> %d) [ecmp installed=%d group_installed=%d]\n",
               format_prefix(route->src->prefix, route->src->plen),
               format_prefix(route->src->src_prefix, route->src->src_plen),
             oldmetric, newmetric, route->installed, should_refresh_ecmp);

        /* Defer ECMP reprogram unless force_reprogram is set */
        if(!force_reprogram) {
            debugf("change_route_metric: deferring ECMP reprogram for %s by %dms\n",
                   format_prefix(route->src->prefix, route->src->plen),
                   ecmp_coalesce_msec);
            schedule_ecmp_reprogram(route);
        } else {
            /* Let refresh_installed_ranks() handle ALL kernel updates for ECMP.
               It computes the correct nexthop set (excluding retracted routes)
               and does a single FLUSH+ADD. Doing ROUTE_MODIFY here would cause
               duplicate/conflicting kernel operations. */
            refresh_installed_ranks_ext(route, force_reprogram);
        }

        local_notify_route(route, LOCAL_CHANGE);
        return;
    }

    if(multipath_ecmp == ECMP_DISABLED && route->installed > 0 &&
       oldmetric == KERNEL_INFINITY &&
       newmetric == KERNEL_INFINITY &&
       route->refmetric == refmetric &&
       route->cost == cost &&
       route->add_metric == add) {
        goto update_local_state;
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
            cancel_coalesce_pending(route);
            goto update_local_state;
        }

        if(oldmetric < KERNEL_INFINITY && newmetric < KERNEL_INFINITY) {
            debugf("change_route_metric: deferring kernel update for %s by %dms\n",
                   format_prefix(route->src->prefix, route->src->plen),
                   route_metric_coalesce_msec);
            schedule_route_metric_coalesce(route);
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

        cancel_coalesce_pending(route);

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
        struct babel_route *r = neigh->routes;

        /* Use per-neighbor route list instead of full-table scan */
        while(r) {
            struct babel_route *next_r = r->neigh_route_next;
            update_route_metric(r);
            r = next_r;
        }
    }

    local_notify_neighbour(neigh, LOCAL_CHANGE);
}

void
update_interface_metric(struct interface *ifp)
{
    struct neighbour *neigh;

    /* Iterate all neighbors on this interface and use their route lists */
    FOR_ALL_NEIGHBOURS(neigh) {
        if(neigh->ifp == ifp && neigh->routes) {
            struct babel_route *r = neigh->routes;

            while(r) {
                struct babel_route *next_r = r->neigh_route_next;
                update_route_metric(r);
                r = next_r;
            }
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

    /* Fast path for duplicate finite updates from the same neighbour.
       If nothing relevant changed, only refresh liveness and avoid
       expensive route processing (change_route_metric/route_changed/
       ECMP refresh checks). */
     if(route && feasible && refmetric < INFINITY &&
         !hard_retract && nexthop != NULL &&
       route->src == src &&
       route->seqno == seqno &&
       route->refmetric == refmetric &&
       route->cost == neighbour_cost(neigh) &&
       route->add_metric == add_metric &&
       memcmp(route->nexthop, nexthop, 16) == 0) {
        route->time = now.tv_sec;
        route->hold_time = hold_time;
        return route;
    }

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
                if(r->time > now.tv_sec || route_expired(r)) {
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

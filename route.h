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

#ifndef MAX_TABLES_PER_FILTER
#define MAX_TABLES_PER_FILTER 32
#endif

struct babel_route {
    struct source *src;
    unsigned short refmetric;
    unsigned short cost;
    unsigned short add_metric;
    unsigned short seqno;
    struct neighbour *neigh;
    unsigned char nexthop[16];
    time_t time;
    unsigned short hold_time;    /* in seconds */
    unsigned short smoothed_metric; /* for route selection */
    time_t smoothed_metric_time;
    short installed; /* 0: not installed, 1: primary installed, >1: ECMP nexthop rank */
    unsigned char multipath_ever; /* 1 if this group ever had 2+ nexthops installed */
    unsigned char metric_update_pending; /* deferred kernel metric update pending */
    struct timeval metric_update_started; /* start of current coalescing window */
    struct timeval metric_update_due;    /* deferred update deadline */
    unsigned char ecmp_reprogram_pending; /* deferred ECMP reprogram pending */
    struct timeval ecmp_reprogram_started; /* start of current ECMP coalescing window */
    struct timeval ecmp_reprogram_due;   /* deferred ECMP reprogram deadline */
    /* Intrusive pending-list membership flags.
       A flag is 1 while the route pointer lives in the corresponding
       singly-linked list (metric_pending_head / ecmp_pending_head).
       Checking the flag is a single load — O(1) — so schedule_*() can
       skip the enqueue without scanning the list. */
    unsigned char on_metric_list;        /* route is in the metric coalesce pending list */
    unsigned char on_ecmp_list;          /* route is in the ECMP reprogram pending list */
    /* Intrusive next pointers for the two pending lists.
       Using the route struct itself as the list node avoids any malloc. */
    struct babel_route *metric_pending_next;
    struct babel_route *ecmp_pending_next;
    unsigned int nexthop_hash;           /* fingerprint of current nexthop set (for caching) */
    int installed_tables[MAX_TABLES_PER_FILTER];  /* Array of kernel routing tables */
    int installed_table_count;                     /* Number of tables route is installed in */
    struct babel_route *next;
    struct babel_route *multipath;
    struct babel_route *neigh_route_next;        /* per-neighbor route list */
    struct babel_route *neigh_route_prev;        /* per-neighbor route list */
};

#ifndef MAX_ECMP_NEXTHOPS
#define MAX_ECMP_NEXTHOPS 32
#endif

#ifndef DEFAULT_ECMP_METRIC_WINDOW
#define DEFAULT_ECMP_METRIC_WINDOW 300
#endif

#ifndef DEFAULT_ROUTE_METRIC_COALESCE_MSEC
#define DEFAULT_ROUTE_METRIC_COALESCE_MSEC 4000
#endif

#ifndef MAX_ROUTE_METRIC_COALESCE_MSEC
#define MAX_ROUTE_METRIC_COALESCE_MSEC 300000
#endif

#ifndef DEFAULT_ECMP_COALESCE_MSEC
#define DEFAULT_ECMP_COALESCE_MSEC 4000
#endif

#ifndef MAX_ECMP_COALESCE_MSEC
#define MAX_ECMP_COALESCE_MSEC 300000
#endif

/* multipath_ecmp values */
#define ECMP_DISABLED 0  /* no multipath */
#define ECMP_EQUAL    1  /* equal-cost multipath, uniform weights;
                            kernel only updated on nexthop-set changes or retractions */
#define ECMP_WEIGHT   2  /* metric-proportional weights;
                            kernel updated on all metric changes */

struct route_stream;

extern struct babel_route **routes;
extern int kernel_metric, allow_duplicates, reflect_kernel_metric, has_duplicate_default;
extern int multipath_ecmp;
extern int ecmp_metric_window;
extern int route_metric_coalesce_msec;
extern int ecmp_coalesce_msec;
extern int route_slots;
extern int smoothing_half_life;
extern int two_to_the_one_over_hl; /* 2^(1/hl) * 0x10000 */

const char *route_ecmp_mode(int ecmp_mode);

static inline int
route_metric(const struct babel_route *route)
{
    int m = (int)route->refmetric + route->cost + route->add_metric;
    return MIN(m, INFINITY);
}

int route_compare(const unsigned char *id,
                  const unsigned char *prefix, unsigned char plen,
                  const unsigned char *src_prefix, unsigned char src_plen,
                  struct babel_route *route);
int find_route_slot(const unsigned char *id,
                    const unsigned char *prefix, unsigned char plen,
                    const unsigned char *src_prefix, unsigned char src_plen,
                    int *new_return);
struct babel_route *find_route(const unsigned char *id,
                        const unsigned char *prefix, unsigned char plen,
                        const unsigned char *src_prefix, unsigned char src_plen,
                        struct neighbour *neigh);
struct babel_route *find_installed_route(const unsigned char *id,
                        const unsigned char *prefix, unsigned char plen,
                        const unsigned char *src_prefix, unsigned char src_plen,
                        int *index);
int installed_routes_estimate(void);
int kernel_route_operation_in_progress(void);
struct babel_route * insert_route(struct babel_route *route);
void flush_route(struct babel_route *route);
void flush_all_routes(void);
void flush_neighbour_routes(struct neighbour *neigh);
void flush_interface_routes(struct interface *ifp, int v4only);
struct route_stream *route_stream(int which);
struct babel_route *route_stream_next(struct route_stream *stream);
void route_stream_done(struct route_stream *stream);
int metric_to_kernel(int metric);
void install_route(struct babel_route *route);
void uninstall_route(struct babel_route *route);
int change_route(int operation, const struct babel_route *route, int metric,
                 const unsigned char *new_next_hop,
                 int new_ifindex, int newmetric,
                 const struct source *newsrc,
                 int *installed_tables, int *installed_table_count);
void change_route_metric(struct babel_route *route,
                         unsigned refmetric,
                         unsigned cost,
                         unsigned add);
void refresh_installed_ranks(struct babel_route *route);
int route_feasible(struct babel_route *route);
int update_feasible(struct source *src,
                    unsigned short seqno, unsigned short refmetric);
void change_smoothing_half_life(int half_life);
int route_smoothed_metric(struct babel_route *route);
int route_ecmp_weight(struct babel_route *route);
struct babel_route *find_best_route(const unsigned char *id,
                                    const unsigned char *prefix,
                                    unsigned char plen,
                                    const unsigned char *src_prefix,
                                    unsigned char src_plen,
                                    int feasible, struct neighbour *exclude);
void update_neighbour_metric(struct neighbour *neigh, int changed);
void update_interface_metric(struct interface *ifp);
void update_route_metric(struct babel_route *route);
struct babel_route *update_route(const unsigned char *id,
                                 const unsigned char *prefix, unsigned char plen,
                                 const unsigned char *src_prefix,
                                 unsigned char src_plen,
                                 unsigned short seqno, unsigned short refmetric,
                                 unsigned short interval,
                                 struct neighbour *neigh,
                                 const unsigned char *nexthop,
                                 int hard_retract);
void retract_neighbour_routes(struct neighbour *neigh);
void send_unfeasible_request(struct neighbour *neigh, int force,
                             unsigned short seqno, unsigned short metric,
                             struct source *src);
void consider_route(struct babel_route *route);
void send_triggered_update(struct babel_route *route,
                           struct source *oldsrc, unsigned oldmetric);
void route_changed(struct babel_route *route,
                   struct source *oldsrc, unsigned short oldmetric);
void route_lost(struct source *src, unsigned oldmetric);
void route_lost_ext(struct source *src, unsigned oldmetric, int hard_retract);
void expire_routes(void);
void route_flush_coalesced_metric_updates(void);
void route_flush_deferred_ecmp_reprograms(void);

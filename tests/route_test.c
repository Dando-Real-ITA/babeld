/*
Copyright (c) 2024 by Tomaz Mascarenhas

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

#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_utilities.h"

#include "../babeld.h"
#include "../configuration.h"
#include "../interface.h"
#include "../kernel.h"
#include "../neighbour.h"
#include "../route.h"
#include "../source.h"
#include "../util.h"
#include "../message.h"
#include "../xroute.h"

#define N_ROUTES 6

struct neighbour *ns[N_ROUTES];

extern int mocked_kernel_route_calls;
extern int mocked_kernel_multipath_calls;
extern int mocked_kernel_last_multipath_count;
extern int mocked_kernel_last_operation;
extern int mocked_kernel_last_new_metric;

int sign(int x) {
    if(x > 0)
        return 1;
    if(x < 0)
        return -1;
    return 0;
}

int route_list_length(struct babel_route *r) {
    int length = 0;
    while(r != NULL) {
        length++;
        r = r->next;
    }
    return length;
}

void route_compare_test(void)
{
    int i, num_of_cases, rc_sign;
    unsigned char *prefix, *src_prefix;
    unsigned char plen, src_plen;
    struct babel_route route;

    typedef struct test_case {
        unsigned char *prefix_val;
        unsigned char plen_val;
        unsigned char *src_prefix_val;
        unsigned char src_plen_val;
        unsigned char *route_src_prefix_val;
        unsigned char route_src_plen_val;
        unsigned char *route_prefix_val;
        unsigned char route_plen_val;
        int expected_rc_sign;
    } test_case;

    test_case tcs[] =
    {
        {
            .prefix_val = (unsigned char[])
                { 204, 191, 204, 17, 179, 148, 97, 201, 24, 33, 133, 32, 138, 138, 104, 235 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 167, 145, 127, 130, 201, 185, 216, 226, 87, 1, 78, 203, 236, 64, 33, 184 },
            .src_plen_val = 96,
            .route_src_prefix_val = (unsigned char[])
                { 0, 237, 201, 179, 130, 42, 124, 154, 75, 1, 186, 213, 139, 34, 192, 50 },
            .route_src_plen_val = 96,
            .route_prefix_val = (unsigned char[])
                { 180, 64, 181, 125, 249, 141, 95, 81, 142, 173, 28, 122, 238, 61, 50, 238 },
            .route_plen_val = 128,
            .expected_rc_sign = 24
        },
        {
            .prefix_val = (unsigned char[])
                { 204, 191, 204, 17, 179, 148, 97, 201, 24, 33, 133, 32, 138, 138, 104, 235 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 167, 145, 127, 130, 201, 185, 216, 226, 87, 1, 78, 203, 236, 64, 33, 184 },
            .src_plen_val = 0,
            .route_src_prefix_val = (unsigned char[])
                { 0, 237, 201, 179, 130, 42, 124, 154, 75, 1, 186, 213, 139, 34, 192, 50 },
            .route_src_plen_val = 96,
            .route_prefix_val = (unsigned char[])
                { 180, 64, 181, 125, 249, 141, 95, 81, 142, 173, 28, 122, 238, 61, 50, 238 },
            .route_plen_val = 128,
            .expected_rc_sign = 1
        },
        {
            .prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 234, 209, 73, 225, 36, 213, 61, 230, 152, 59, 215, 238, 134, 233, 23, 140 },
            .src_plen_val = 96,
            .route_src_prefix_val = (unsigned char[])
                { 5, 224, 238, 168, 213, 155, 140, 95, 208, 200, 219, 162, 95, 201, 94, 65 },
            .route_src_plen_val = 0,
            .route_prefix_val = (unsigned char[])
                { 225, 33, 114, 8, 246, 83, 140, 92, 194, 195, 254, 241, 86, 75, 18, 40 },
            .route_plen_val = 128,
            .expected_rc_sign = -1
        },
        {
            .prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .plen_val = 10,
            .src_prefix_val = (unsigned char[])
                { 234, 209, 73, 225, 36, 213, 61, 230, 152, 59, 215, 238, 134, 233, 23, 140 },
            .src_plen_val = 96,
            .route_src_prefix_val = (unsigned char[])
                { 5, 224, 238, 168, 213, 155, 140, 95, 208, 200, 219, 162, 95, 201, 94, 65 },
            .route_src_plen_val = 96,
            .route_prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .route_plen_val = 128,
            .expected_rc_sign = -1
        },
        {
            .prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 234, 209, 73, 225, 36, 213, 61, 230, 152, 59, 215, 238, 134, 233, 23, 140 },
            .src_plen_val = 96,
            .route_src_prefix_val = (unsigned char[])
                { 5, 224, 238, 168, 213, 155, 140, 95, 208, 200, 219, 162, 95, 201, 94, 65 },
            .route_src_plen_val = 96,
            .route_prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .route_plen_val = 10,
            .expected_rc_sign = 1
        },
        {
            .prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 234, 209, 73, 225, 36, 213, 61, 230, 152, 59, 215, 238, 134, 233, 23, 140 },
            .src_plen_val = 96,
            .route_src_prefix_val = (unsigned char[])
                { 5, 224, 238, 168, 213, 155, 140, 95, 208, 200, 219, 162, 95, 201, 94, 65 },
            .route_src_plen_val = 96,
            .route_prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .route_plen_val = 128,
            .expected_rc_sign = 1
        },
        {
            .prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .plen_val = 128,
            .src_prefix_val = (unsigned char[])
                { 234, 209, 73, 225, 36, 213, 61, 230, 152, 59, 215, 238, 134, 233, 23, 140 },
            .src_plen_val = 0,
            .route_src_prefix_val = (unsigned char[])
                { 5, 224, 238, 168, 213, 155, 140, 95, 208, 200, 219, 162, 95, 201, 94, 65 },
            .route_src_plen_val = 0,
            .route_prefix_val = (unsigned char[])
                { 201, 5, 52, 158, 160, 192, 253, 113, 137, 217, 19, 232, 162, 114, 41, 141 },
            .route_plen_val = 128,
            .expected_rc_sign = 0
        },
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);
    route.src = malloc(sizeof(struct source));
    for(i = 0; i < num_of_cases; ++i) {
        prefix = tcs[i].prefix_val;
        plen = tcs[i].plen_val;
        src_prefix = tcs[i].src_prefix_val;
        src_plen = tcs[i].src_plen_val;
        route.src->plen = tcs[i].route_plen_val;
        memcpy(route.src->prefix, tcs[i].route_prefix_val, 16);
        route.src->src_plen = tcs[i].route_src_plen_val;
        memcpy(route.src->src_prefix, tcs[i].route_src_prefix_val, 16);

        rc_sign = route_compare(NULL, prefix, plen, src_prefix, src_plen, &route);

        // The magnitude of the result of memcmp is implementation-dependent, so we can only check
        // if we got the right sign
        if(!babel_check(sign(rc_sign) == sign(tcs[i].expected_rc_sign))) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on route_compare\n", i);
            fprintf(stderr, "prefix: %s\n", format_prefix(prefix, plen));
            fprintf(stderr, "src_prefix: %s\n", format_prefix(src_prefix, src_plen));
            fprintf(stderr, "route->src->prefix: %s\n", format_prefix(route.src->prefix, route.src->plen));
            fprintf(stderr, "route->src->src_prefix: %s\n", format_prefix(route.src->src_prefix, route.src->src_plen));
            fprintf(stderr, "expected rc: %d\n", tcs[i].expected_rc_sign);
            fprintf(stderr, "computed rc: %d\n", rc_sign);
        }
    }
    free(route.src);
}

void find_route_slot_test(void)
{
    int i, num_of_cases, rc, new_return, test_ok;
    unsigned char *prefix, *src_prefix;
    unsigned char plen, src_plen;

    typedef struct test_case {
        unsigned char *prefix_val;
        unsigned char plen_val;
        unsigned char *src_prefix_val;
        unsigned char src_plen_val;
        int expected_rc;
        int expected_new_return;
    } test_case;

    test_case tcs[] =
    {
        {
            .prefix_val = (unsigned char[])
                { 145, 103, 214, 219, 183, 36, 182, 66, 11, 175, 199, 131, 227, 198, 7, 136 },
            .plen_val = 54,
            .src_prefix_val = (unsigned char[])
                { 97, 114, 138, 89, 89, 22, 41, 71, 180, 179, 225, 48, 49, 80, 170, 194 },
            .src_plen_val = 99,
            .expected_rc = -1,
            .expected_new_return = 4
        },
        {
            .prefix_val = (unsigned char[])
                { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .expected_rc = 1,
            .expected_new_return = -1
        },
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);
    for(i = 0; i < num_of_cases; ++i) {
        prefix = tcs[i].prefix_val;
        plen = tcs[i].plen_val;
        src_prefix = tcs[i].src_prefix_val;
        src_plen = tcs[i].src_plen_val;
        new_return = -1;

        rc = find_route_slot(NULL, prefix, plen, src_prefix, src_plen, &new_return);

        test_ok = (tcs[i].expected_rc == -1 && new_return == tcs[i].expected_new_return) ||
                  (tcs[i].expected_rc == rc);
        if (!babel_check(test_ok)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on route_compare\n", i);
            fprintf(stderr, "prefix: %s\n", format_prefix(prefix, plen));
            fprintf(stderr, "src_prefix: %s\n", format_prefix(src_prefix, src_plen));
            fprintf(stderr, "expected rc: %d\n", tcs[i].expected_rc);
            fprintf(stderr, "computed rc: %d\n", rc);
            fprintf(stderr, "expected new_return: %d\n", tcs[i].expected_new_return);
            fprintf(stderr, "computed new_return: %d\n", new_return);
        }
    }
}

void find_route_test(void)
{
    int i, num_of_cases;
    unsigned char *prefix, *src_prefix;
    unsigned char plen, src_plen;
    struct babel_route *route, *expected_route;
    struct neighbour *neigh;

    typedef struct test_case {
        unsigned char *prefix_val;
        unsigned char plen_val;
        unsigned char *src_prefix_val;
        unsigned char src_plen_val;
        int neigh_index_val;
        int expected_route_index;
    } test_case;

    test_case tcs[] =
    {
        {
            .prefix_val = (unsigned char[])
                { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .neigh_index_val = 1,
            .expected_route_index = 1
        },
        {
            .prefix_val = (unsigned char[])
                { 68, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .neigh_index_val = -1,
            .expected_route_index = -1
        },
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);

    for(i = 0; i < num_of_cases; ++i) {
        prefix = tcs[i].prefix_val;
        plen = tcs[i].plen_val;
        src_prefix = tcs[i].src_prefix_val;
        src_plen = tcs[i].src_plen_val;
        neigh = ns[tcs[i].neigh_index_val];

        route = find_route(NULL, prefix, plen, src_prefix, src_plen, neigh);

        expected_route =
            tcs[i].expected_route_index == -1 ? NULL : routes[tcs[i].expected_route_index];
        if(!babel_check(route == expected_route)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on find_route\n", i);
            fprintf(stderr, "prefix: %s\n", format_prefix(prefix, plen));
            fprintf(stderr, "src_prefix: %s\n", format_prefix(src_prefix, src_plen));
            fprintf(stderr, "neighbour: ns[%d]\n", tcs[i].neigh_index_val);
            fprintf(stderr, "expected route: routes[%d]\n", tcs[i].expected_route_index);
        }
    }
}

void find_installed_route_test(void)
{

    unsigned char prefix[] =
      { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 };
    unsigned char src_prefix[] =
      { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 };
    unsigned char plen = 101;
    unsigned char src_plen = 115;

    struct babel_route *route = find_installed_route(NULL, prefix, plen, src_prefix, src_plen, NULL);

    if(route == NULL) {
        struct babel_route *candidate;
        candidate = find_route(NULL, prefix, plen, src_prefix, src_plen, ns[1]);
        if(candidate) {
            install_route(candidate);
            route = find_installed_route(NULL, prefix, plen, src_prefix, src_plen, NULL);
        }
    }

    if(!babel_check(route != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on find_installed_route\n");
        fprintf(stderr, "prefix: %s\n", format_prefix(prefix, plen));
        fprintf(stderr, "src_prefix: %s\n", format_prefix(src_prefix, src_plen));
        fprintf(stderr, "expected a non-NULL installed route.\n");
    }

    if(route == NULL)
        return;

    uninstall_route(route);

    route = find_installed_route(NULL, prefix, plen, src_prefix, src_plen, NULL);
    if(!babel_check(route == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on find_installed_route (after uninstall_route)\n");
        fprintf(stderr, "prefix: %s\n", format_prefix(prefix, plen));
        fprintf(stderr, "src_prefix: %s\n", format_prefix(src_prefix, src_plen));
        fprintf(stderr, "expected NULL.\n");
    }
}

void installed_routes_estimate_test(void)
{
    struct route_stream *stream = route_stream(1);
    struct babel_route *r;
    int installed_routes = 0, estimate = installed_routes_estimate();

    while(1) {
        r = route_stream_next(stream);
        if(r == NULL)
            break;
        else
            installed_routes++;
    }

    if(!babel_check(installed_routes <= estimate)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on installed_routes_estimate.\n");
        fprintf(stderr, "Expected that the estimated number would be greater or equal to the number of actually installed routes.\n");
        fprintf(stderr, "Installed routes: %d\nEstimate: %d\n", installed_routes, estimate);
    }
}

void insert_route_test(void)
{
    int i, num_of_cases, test_ok;
    struct babel_route *route, *returned_route, *r;

    typedef struct test_case {
        unsigned char *prefix_val;
        unsigned char plen_val;
        unsigned char *src_prefix_val;
        unsigned char src_plen_val;
        int expected_pos;
    } test_case;

    test_case tcs[] =
    {
        {
            .prefix_val = (unsigned char[])
                { 88, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .expected_pos = 2
        },
        {
            .prefix_val = (unsigned char[])
                { 68, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .expected_pos = 0
        },
        {
            .prefix_val = (unsigned char[])
                { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
            .plen_val = 101,
            .src_prefix_val = (unsigned char[])
                { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
            .src_plen_val = 115,
            .expected_pos = 2
        },
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);
    struct babel_route *added_routes[num_of_cases];

    for(i = 0; i < num_of_cases; ++i) {
        route = malloc(sizeof(struct babel_route));
        route->installed = 0;
        route->src = malloc(sizeof(struct source));
        route->src->plen = tcs[i].plen_val;
        memcpy(route->src->prefix, tcs[i].prefix_val, 16);
        route->src->src_plen = tcs[i].src_plen_val;
        route->src->route_count = 1;
        memcpy(route->src->src_prefix, tcs[i].src_prefix_val, 16);

        returned_route = insert_route(route);

        r = routes[tcs[i].expected_pos];
        while(r->next)
            r = r->next;

        test_ok = returned_route != NULL;
        test_ok &= r == route;
        if(!babel_check(test_ok)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on insert_route\n", i);
            fprintf(stderr, "routes[%d] is not equal to the route being inserted.\n", tcs[i].expected_pos);
        }
        added_routes[i] = r;
    }
    for(i = 0; i < num_of_cases; i++)
        flush_route(added_routes[i]);
}

void flush_route_test(void) {
    int i, j, num_of_cases, test_ok, prev_slots, prev_length, curr_length;
    struct babel_route *r, *to_insert;

    // Insert some routes before running the test, so we can test slots with size > 1.
    unsigned char p1[] = { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 };
    unsigned char src_p1[] = { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 };
    to_insert = malloc(sizeof(struct babel_route));
    to_insert->installed = 0;
    to_insert->src = malloc(sizeof(struct source));
    to_insert->src->plen = 101;
    memcpy(to_insert->src->prefix, p1, 16);
    to_insert->src->src_plen = 115;
    memcpy(to_insert->src->src_prefix, src_p1, 16);
    to_insert->src->route_count = 1;
    insert_route(to_insert);

    // Select one of the routes stored in the global variable `routes` to be flushed in the test.
    typedef struct test_case {
        int slot; // slot where the route is located
        int pos; // position inside that slot of the route
        short last_route_in_slot;
    } test_case;

    test_case tcs[] =
    {
        {
            .slot = 1,
            .pos = 1,
            .last_route_in_slot = 0
        },
        {
            .slot = 0,
            .pos = 0,
            .last_route_in_slot = 1
        }
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);

    for(i = 0; i < num_of_cases; ++i) {
        r = routes[tcs[i].slot];
        for(j = 0; j < tcs[i].pos; j++)
            r = r->next;

        prev_slots = route_slots;
        prev_length = route_list_length(routes[tcs[i].slot]);

        flush_route(r);

        curr_length = route_list_length(routes[tcs[i].slot]);
        if(tcs[i].last_route_in_slot)
            test_ok = route_slots == prev_slots - 1;
        else
            test_ok = curr_length == prev_length - 1;

        if(!babel_check(test_ok)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on flush_route.\n", i);
            fprintf(stderr, "Trying to flush %d-th route from %d-th slot:\n", tcs[i].pos, tcs[i].slot);
            if(!tcs[i].last_route_in_slot && curr_length != prev_length - 1)
                fprintf(stderr, "Route list length was not updated. Previous: %d; Current: %d.\n", prev_length, curr_length);
            if(tcs[i].last_route_in_slot && route_slots != prev_slots - 1)
                fprintf(stderr, "Number of route slots was not updated. Previous: %d; Current: %d.\n", prev_slots, route_slots);
        }
    }
}

void flush_all_routes_test()
{
    flush_all_routes();
    if(!babel_check(route_slots == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on flush_all_routes.\n");
        fprintf(stderr, "Expected route_slots = 0, got %d.\n", route_slots);
    }
}

void flush_neighbour_route_test(void)
{
    int prev_route_slots = route_slots;
    flush_neighbour_routes(ns[1]);
    if(!babel_check(prev_route_slots == route_slots + 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on flush_neighbour_route_test.\n");
        fprintf(stderr, "Expected route_slots = %d, got %d.\n", prev_route_slots - 1, route_slots);
    }
}

void route_stream_test(void) {
    struct route_stream *stream;
    int which;
    for(which = 0; which <= 1; which++) {
        stream = route_stream(which);
        if(!babel_check(stream != NULL)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test: route_stream(%d) was NULL.", which);
        }
    }
}

void route_stream_next_test(void) {
    int i, j, num_of_cases;
    struct route_stream *stream;
    struct babel_route *route = NULL;

    typedef struct test_case {
        int installed_val;
        int number_of_calls;
        int expected_route_index;
    } test_case;

    test_case tcs[] = {
        {
            .installed_val = 0,
            .number_of_calls = 2,
            .expected_route_index = 1,
        },
        {
            .installed_val = 1,
            .number_of_calls = 1,
            .expected_route_index = 0,
        }
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);

    for(i = 0; i < num_of_cases; ++i) {
        stream = route_stream(tcs[i].installed_val);
        j = tcs[i].number_of_calls;
        while(j) {
            route = route_stream_next(stream);
            j--;
        }

        if(tcs[i].installed_val) {
            if(!babel_check(route != NULL && route->installed)) {
                fprintf(stderr, "-----------------------------------------------\n");
                fprintf(stderr, "Failed test (%d) on route_stream_next.\n", i);
                fprintf(stderr, "Expected an installed route after %d iteration(s).\n",
                        tcs[i].number_of_calls);
            }
            continue;
        }

        if(!babel_check(routes[tcs[i].expected_route_index] == route)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test (%d) on route_stream_next.\n", i);
            fprintf(stderr, "Expected routes[%d] after %d iteration(s) (", tcs[i].expected_route_index, tcs[i].number_of_calls);
            if(tcs[i].installed_val)
                fprintf(stderr, "only installed routes).\n");
            else
                fprintf(stderr, "all routes).\n");
        }
    }
}

void metric_to_kernel_test(void) {
    int m;
    m = metric_to_kernel(2 * INFINITY);
    if(!babel_check(m == KERNEL_INFINITY)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test: metric_to_kernel(2 * INFINITY) = %d, expected %d\n", m, KERNEL_INFINITY);
    }
    m = metric_to_kernel(INFINITY - 1);
    if(!babel_check(m == kernel_metric)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test: metric_to_kernel(INFINITY - 1) = %d, expected %d\n", m, kernel_metric);
    }
    reflect_kernel_metric = 1;
    m = metric_to_kernel(KERNEL_INFINITY - 1);
    if(!babel_check(m == KERNEL_INFINITY - 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test: metric_to_kernel(KERNEL_INFINITY - 1) = %d, expected %d.\n", m, KERNEL_INFINITY - 1);
    }
    kernel_metric = 2;
    m = metric_to_kernel(KERNEL_INFINITY - 1);
    if(!babel_check(m == KERNEL_INFINITY)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test: metric_to_kernel(KERNEL_INFINITY - 1) = %d, expected %d.\n", m, KERNEL_INFINITY);
    }
}

void update_feasible_test(void)
{
    int i, num_of_cases, rc;
    struct source src;

    gettime(&now);

    rc = update_feasible(NULL, 0, 0);
    if(!babel_check(rc == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on update_feasible.\n");
        fprintf(stderr, "update_feasible(NULL, 0, 0) = %d, expected 1.\n", rc);
    }

    typedef struct test_case {
        time_t src_time_val;
        unsigned short src_seqno_val;
        unsigned short src_metric_val;
        unsigned short seqno_val;
        unsigned short refmetric_val;
        int expected_rc;
    } test_case;

    test_case tcs[] =
    {
        {
            .src_time_val = now.tv_sec - SOURCE_GC_TIME - 10,
            .src_seqno_val = 0,
            .src_metric_val = 0,
            .seqno_val = 0,
            .refmetric_val = 0,
            .expected_rc = 1,
        },
        {
            .src_time_val = now.tv_sec,
            .src_seqno_val = 0,
            .src_metric_val = 0,
            .seqno_val = 0,
            .refmetric_val = INFINITY,
            .expected_rc = 1
        },
        {
            .src_time_val = now.tv_sec,
            .src_seqno_val = 0,
            .src_metric_val = 0,
            .seqno_val = 0x8000,
            .refmetric_val = 0,
            .expected_rc = 1
        },
        {
            .src_time_val = now.tv_sec,
            .src_seqno_val = 0x8000,
            .src_metric_val = 50,
            .seqno_val = 0x8000,
            .refmetric_val = 10,
            .expected_rc = 1
        },
        {
            .src_time_val = now.tv_sec,
            .src_seqno_val = 0x8000,
            .src_metric_val = 10,
            .seqno_val = 0x8000,
            .refmetric_val = 50,
            .expected_rc = 0
        },
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);
    for(i = 0; i < num_of_cases; ++i) {
        src.time = tcs[i].src_time_val;
        src.seqno = tcs[i].src_seqno_val;
        src.metric = tcs[i].src_metric_val;

        rc = update_feasible(&src, tcs[i].seqno_val, tcs[i].refmetric_val);
        if(!babel_check(rc == tcs[i].expected_rc)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test on update_feasible.\n");
            fprintf(stderr, "src->time = %jd\n", src.time);
            fprintf(stderr, "src->seqno = %d\n", src.seqno);
            fprintf(stderr, "src->metric = %d\n", src.metric);
            fprintf(stderr, "seqno = %d\n", tcs[i].seqno_val);
            fprintf(stderr, "refmetric = %d\n", tcs[i].refmetric_val);
            fprintf(stderr, "expected rc: %d\n", tcs[i].expected_rc);
            fprintf(stderr, "computed rc: %d\n", rc);
        }
    }
}

void change_smoothing_half_life_test(void)
{
    int half_life;
    int expected_values[] = {0, 131072, 92682, 82570, 77935, 74621};

    change_smoothing_half_life(-1);
    if(!babel_check(two_to_the_one_over_hl == 0 && smoothing_half_life == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on change_smoothing_half_life.\n");
        fprintf(stderr, "change_smoothing_half_life(-1) resulted in:\n");
        fprintf(stderr, "two_to_the_one_over_hl = %d and smoothing_half_life = %d.\n",
                        two_to_the_one_over_hl, smoothing_half_life);
        fprintf(stderr, "Expected two_to_the_one_over_hl = 0 and smoothing_half_life = 0.\n");
    }
    for(half_life = 0; half_life <= 5; half_life++) {
        change_smoothing_half_life(half_life);
        if(!babel_check(smoothing_half_life == half_life && two_to_the_one_over_hl == expected_values[half_life])) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test on change_smoothing_half_life.\n");
            fprintf(stderr, "change_smoothing_half_life(%d) resulted in:\n", half_life);
            fprintf(stderr, "two_to_the_one_over_hl = %d and smoothing_half_life = %d.\n",
                            two_to_the_one_over_hl, smoothing_half_life);
            fprintf(stderr, "Expected two_to_the_one_over_hl = %d and smoothing_half_life = %d.\n",
                             expected_values[half_life], half_life);
        }
    }
}

void change_route_metric_test(void)
{
    int i, num_of_cases, test_ok;
    struct babel_route *route;

    typedef struct test_case {
        int index_of_route_to_change;
        unsigned refmetric_val;
        unsigned cost_val;
        unsigned add_val;
    } test_case;

    test_case tcs[] =
    {
        {
            .index_of_route_to_change = 1,
            .refmetric_val = 10,
            .cost_val = 20,
            .add_val = 30
        }
    };

    num_of_cases = sizeof(tcs) / sizeof(test_case);
    for(i = 0; i < num_of_cases; ++i) {
        route = routes[tcs[i].index_of_route_to_change];

        change_route_metric(route, tcs[i].refmetric_val, tcs[i].cost_val, tcs[i].add_val);

        test_ok = route->refmetric == tcs[i].refmetric_val;
        test_ok &= route->cost == tcs[i].cost_val;
        test_ok &= route->add_metric == tcs[i].add_val;
        if(!babel_check(test_ok)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr, "Failed test on change_route_metric\n");
            fprintf(stderr, "Route used: routes[%d]\n", tcs[i].index_of_route_to_change);
            fprintf(stderr, "Call was: change_route_metric(routes[%d], %u, %u, %u)\n",
                            tcs[i].index_of_route_to_change,
                            tcs[i].refmetric_val,
                            tcs[i].cost_val,
                            tcs[i].add_val);
            fprintf(stderr, "Expected route->refmetric = %u, "
                            "route->cost = %u, "
                            "route->add_metric = %u.\n",
                            tcs[i].refmetric_val, tcs[i].cost_val, tcs[i].add_val);
            fprintf(stderr, "Got: route->refmetric = %u, "
                            "route->cost = %u, "
                            "route->add_metric = %u.\n",
                            route->refmetric, route->cost, route->add_metric);
        }
    }
}

void flush_xroute_reannounces_more_specific_test(void)
{
    struct interface *ifp;
    struct xroute *xroute;
    unsigned char prefix104[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x02,
         0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
    unsigned char prefix112[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x02,
         0x00, 0x01, 0x00, 0x01, 0x01, 0xaa, 0x00, 0x00};
    unsigned char zeroes[16] = {0};
    int i, saw_104 = 0, saw_112 = 0;

    ifp = add_interface("test_if", NULL);
    ifp->flags |= IF_UP;
    ifp->hello_interval = 4000;
    ifp->update_interval = 4000;
    ifp->buf.size = 512;

    if(add_xroute(prefix104, 104, zeroes, 0, 32, ifp->ifindex,
                  RTPROT_BABEL_LOCAL, 254) <= 0 ||
       add_xroute(prefix112, 112, zeroes, 0, 32, ifp->ifindex,
                  RTPROT_BABEL_LOCAL, 254) <= 0) {
        babel_check(0);
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on flush_xroute_reannounces_more_specific_test setup.\n");
        goto cleanup;
    }

    xroute = find_xroute(prefix104, 104, zeroes, 0);
    if(!babel_check(xroute != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on flush_xroute_reannounces_more_specific_test: missing /104 xroute.\n");
        goto cleanup;
    }

    flush_xroute(xroute, 1);

    for(i = 0; i < ifp->num_buffered_updates; i++) {
        struct buffered_update *u = &ifp->buffered_updates[i];
        if(u->plen == 104 && memcmp(u->prefix, prefix104, 16) == 0)
            saw_104 = 1;
        if(u->plen == 112 && memcmp(u->prefix, prefix112, 16) == 0)
            saw_112 = 1;
    }

    if(!babel_check(saw_104 && saw_112)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_xroute_reannounces_more_specific_test.\n");
        fprintf(stderr,
                "Expected buffered updates for both withdrawn /104 and covered /112.\n");
        fprintf(stderr, "Buffered updates: %d, saw /104: %d, saw /112: %d.\n",
                ifp->num_buffered_updates, saw_104, saw_112);
    }

cleanup:
    xroute = find_xroute(prefix112, 112, zeroes, 0);
    if(xroute)
        flush_xroute(xroute, 0);
    free(ifp->buffered_updates);
    ifp->buffered_updates = NULL;
    ifp->num_buffered_updates = 0;
    ifp->update_bufsize = 0;
    ifp->update_flush_timeout.tv_sec = 0;
    ifp->update_flush_timeout.tv_usec = 0;
}

void kernel_delete_retracted_route_clears_installed_state_test(void)
{
    int i;
    struct babel_route *route = NULL;
    struct kernel_route kroute;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *r = routes[i];
        while(r) {
            if(r->neigh == ns[1]) {
                route = r;
                break;
            }
            r = r->next;
        }
        if(route)
            break;
    }

    if(!babel_check(route != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_retracted_route_clears_installed_state_test setup (missing route).\n");
        return;
    }

    retract_neighbour_routes(ns[1]);

    if(!route->installed)
        install_route(route);

    if(!babel_check(route->installed && route->installed_table_count > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_retracted_route_clears_installed_state_test setup (route not installed before delete).\n");
        return;
    }

    memset(&kroute, 0, sizeof(kroute));
    memcpy(kroute.prefix, route->src->prefix, 16);
    kroute.plen = route->src->plen;
    memcpy(kroute.src_prefix, route->src->src_prefix, 16);
    kroute.src_plen = route->src->src_plen;
    kroute.table = 0;
    kroute.proto = RTPROT_BABEL;
    kroute.metric = KERNEL_INFINITY;

    kernel_route_notify(0, &kroute, NULL);

    if(!babel_check(!route->installed && route->installed_table_count == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_retracted_route_clears_installed_state_test.\n");
        fprintf(stderr,
                "Expected route to be uninstalled after kernel delete notification; installed=%d tables=%d.\n",
                route->installed, route->installed_table_count);
    }
}

void kernel_delete_old_reachable_state_during_retraction_is_ignored_test(void)
{
    int i;
    struct babel_route *route = NULL;
    struct kernel_route kroute;
    int current_metric;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *r = routes[i];
        while(r) {
            if(r->neigh == ns[1]) {
                route = r;
                break;
            }
            r = r->next;
        }
        if(route)
            break;
    }

    if(!babel_check(route != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_old_reachable_state_during_retraction_is_ignored_test setup (missing route).\n");
        return;
    }

    retract_neighbour_routes(ns[1]);

    if(!route->installed)
        install_route(route);

    current_metric = metric_to_kernel(route_metric(route));
    if(!babel_check(route->installed && route->installed_table_count > 0 &&
                    current_metric == KERNEL_INFINITY)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_old_reachable_state_during_retraction_is_ignored_test setup (route not retracted/installed).\n");
        return;
    }

    memset(&kroute, 0, sizeof(kroute));
    memcpy(kroute.prefix, route->src->prefix, 16);
    kroute.plen = route->src->plen;
    memcpy(kroute.src_prefix, route->src->src_prefix, 16);
    kroute.src_plen = route->src->src_plen;
    kroute.table = route->installed_tables[0];
    kroute.proto = RTPROT_BABEL;
    kroute.metric = kernel_metric;

    kernel_route_notify(0, &kroute, NULL);

    if(!babel_check(route->installed && route->installed_table_count > 0 &&
                    metric_to_kernel(route_metric(route)) == KERNEL_INFINITY)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_old_reachable_state_during_retraction_is_ignored_test.\n");
        fprintf(stderr,
                "Expected stale reachable delete notify to be ignored while route stays installed/unreachable; installed=%d tables=%d metric=%d.\n",
                route->installed, route->installed_table_count,
                route_metric(route));
    }
}

void explicit_retraction_uninstalls_route_test(void)
{
    int i;
    struct babel_route *route = NULL;
    unsigned short retract_seqno;
    int old_enable_hard_withdraw;

    old_enable_hard_withdraw = enable_hard_withdraw;
    enable_hard_withdraw = 1;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *r = routes[i];
        while(r) {
            if(r->neigh == ns[1]) {
                route = r;
                break;
            }
            r = r->next;
        }
        if(route)
            break;
    }

    if(!babel_check(route != NULL)) {
        enable_hard_withdraw = old_enable_hard_withdraw;
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_uninstalls_route_test setup (missing route).\n");
        return;
    }

    if(!babel_check(route->installed)) {
        install_route(route);
    }

    if(!babel_check(route->installed)) {
        enable_hard_withdraw = old_enable_hard_withdraw;
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_uninstalls_route_test setup (route not installed).\n");
        return;
    }

    retract_seqno = seqno_plus(route->seqno, 1);

    update_route(route->src->id,
                 route->src->prefix, route->src->plen,
                 route->src->src_prefix, route->src->src_plen,
                 retract_seqno, INFINITY, 0,
                 route->neigh, route->nexthop,
                 1);

    if(!babel_check(!route->installed && route->installed_table_count == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_uninstalls_route_test.\n");
        fprintf(stderr,
                "Expected explicit retraction to uninstall route; installed=%d tables=%d.\n",
                route->installed, route->installed_table_count);
    }

    enable_hard_withdraw = old_enable_hard_withdraw;
}

void explicit_retraction_without_tlv_keeps_installed_test(void)
{
    int i;
    struct babel_route *route = NULL;
    unsigned short retract_seqno;
    int old_enable_hard_withdraw;

    old_enable_hard_withdraw = enable_hard_withdraw;
    enable_hard_withdraw = 1;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *r = routes[i];
        while(r) {
            if(r->neigh == ns[1]) {
                route = r;
                break;
            }
            r = r->next;
        }
        if(route)
            break;
    }

    if(!babel_check(route != NULL)) {
        enable_hard_withdraw = old_enable_hard_withdraw;
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_without_tlv_keeps_installed_test setup (missing route).\n");
        return;
    }

    if(!route->installed)
        install_route(route);

    if(!babel_check(route->installed)) {
        enable_hard_withdraw = old_enable_hard_withdraw;
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_without_tlv_keeps_installed_test setup (route not installed).\n");
        return;
    }

    retract_seqno = seqno_plus(route->seqno, 1);

    update_route(route->src->id,
                 route->src->prefix, route->src->plen,
                 route->src->src_prefix, route->src->src_plen,
                 retract_seqno, INFINITY, 0,
                 route->neigh, route->nexthop,
                 0);

    if(!babel_check(route->installed && route->installed_table_count > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on explicit_retraction_without_tlv_keeps_installed_test.\n");
        fprintf(stderr,
                "Expected normal retraction to keep route installed/unreachable; installed=%d tables=%d metric=%d.\n",
                route->installed, route->installed_table_count,
                route_metric(route));
    }

    enable_hard_withdraw = old_enable_hard_withdraw;
}

void kernel_delete_babel_proto_flushes_matching_xroute_test(void)
{
    struct interface *ifp;
    struct kernel_route kroute;
    struct xroute *xroute;
    unsigned char prefix[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x10,
         0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00};
    unsigned char zeroes[16] = {0};

    ifp = add_interface("test_if", NULL);
    if(!babel_check(ifp != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_babel_proto_flushes_matching_xroute_test setup (missing interface).\n");
        return;
    }

    if(add_xroute(prefix, 96, zeroes, 0, 32, ifp->ifindex,
                  RTPROT_BABEL_LOCAL, 254) <= 0) {
        babel_check(0);
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_babel_proto_flushes_matching_xroute_test setup (add_xroute failed).\n");
        return;
    }

    xroute = find_xroute(prefix, 96, zeroes, 0);
    if(!babel_check(xroute != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_babel_proto_flushes_matching_xroute_test setup (xroute not found).\n");
        return;
    }

    memset(&kroute, 0, sizeof(kroute));
    memcpy(kroute.prefix, prefix, 16);
    kroute.plen = 96;
    memcpy(kroute.src_prefix, zeroes, 16);
    kroute.src_plen = 0;
    kroute.table = 0;
    kroute.proto = RTPROT_BABEL;

    kernel_route_notify(0, &kroute, NULL);

    xroute = find_xroute(prefix, 96, zeroes, 0);
    if(!babel_check(xroute == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on kernel_delete_babel_proto_flushes_matching_xroute_test.\n");
        fprintf(stderr,
                "Expected matching xroute to be flushed on delete notify with proto RTPROT_BABEL.\n");

        /* Cleanup in failure path. */
        flush_xroute(xroute, 0);
    }
}

void format_xroute_metrics_labels_test(void)
{
    char ifname[IF_NAMESIZE];
    unsigned int ifindex;
    struct interface *ifp;
    struct xroute xr;
    struct filter *generic_filter = NULL;
    struct filter *per_if_filter = NULL;
    char metrics_buf[256];
    int rc;
    int expected_generic;
    int expected_per_if;
    char expected_generic_str[64];
    char expected_per_if_str[96];

    ifname[0] = '\0';
    ifindex = 0;
    for(unsigned int i = 1; i < 256; i++) {
        if(if_indextoname(i, ifname) != NULL) {
            ifindex = i;
            break;
        }
    }

    if(ifindex == 0) {
        if(!babel_check(0)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr,
                    "Failed test on format_xroute_metrics_labels_test setup (no interface found).\n");
        }
        return;
    }

    ifp = add_interface(ifname, NULL);
    if(ifp == NULL) {
        if(!babel_check(0)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr,
                    "Failed test on format_xroute_metrics_labels_test setup (add_interface failed).\n");
        }
        return;
    }
    ifp->ifindex = ifindex;

    memset(&xr, 0, sizeof(xr));
    xr.plen = 64;
    xr.src_plen = 0;
    xr.metric = 100;

    generic_filter = calloc(1, sizeof(struct filter));
    per_if_filter = calloc(1, sizeof(struct filter));
    if(generic_filter == NULL || per_if_filter == NULL) {
        free(generic_filter);
        free(per_if_filter);
        if(!babel_check(0)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr,
                    "Failed test on format_xroute_metrics_labels_test setup (calloc filter failed).\n");
        }
        return;
    }

    generic_filter->plen_le = 128;
    generic_filter->src_plen_le = 128;
    generic_filter->prefix = malloc(16);
    generic_filter->src_prefix = malloc(16);
    if(generic_filter->prefix == NULL || generic_filter->src_prefix == NULL) {
        free(generic_filter->prefix);
        free(generic_filter->src_prefix);
        free(generic_filter);
        free(per_if_filter);
        if(!babel_check(0)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr,
                    "Failed test on format_xroute_metrics_labels_test setup (malloc generic filter prefix failed).\n");
        }
        return;
    }
    memset(generic_filter->prefix, 0, 16);
    memset(generic_filter->src_prefix, 0, 16);
    generic_filter->plen = xr.plen;
    generic_filter->src_plen = xr.src_plen;
    generic_filter->action.add_metric = 10;
    add_filter(generic_filter, FILTER_TYPE_OUTPUT);

    per_if_filter->plen_le = 128;
    per_if_filter->src_plen_le = 128;
    per_if_filter->prefix = malloc(16);
    per_if_filter->src_prefix = malloc(16);
    per_if_filter->ifname = strdup(ifname);
    if(per_if_filter->prefix == NULL || per_if_filter->src_prefix == NULL ||
       per_if_filter->ifname == NULL) {
        free(per_if_filter->prefix);
        free(per_if_filter->src_prefix);
        free(per_if_filter->ifname);
        free(per_if_filter);
        if(!babel_check(0)) {
            fprintf(stderr, "-----------------------------------------------\n");
            fprintf(stderr,
                    "Failed test on format_xroute_metrics_labels_test setup (malloc per-if filter fields failed).\n");
        }
        return;
    }
    memset(per_if_filter->prefix, 0, 16);
    memset(per_if_filter->src_prefix, 0, 16);
    per_if_filter->plen = xr.plen;
    per_if_filter->src_plen = xr.src_plen;
    per_if_filter->ifindex = ifindex;
    per_if_filter->action.add_metric = 42;
    add_filter(per_if_filter, FILTER_TYPE_OUTPUT);

    rc = format_xroute_metrics(&xr, metrics_buf, sizeof(metrics_buf));

    expected_generic = MIN((int)xr.metric + 10, INFINITY);
    expected_per_if = MIN((int)xr.metric + 42, INFINITY);
    snprintf(expected_generic_str, sizeof(expected_generic_str),
             "metric-generic %d", expected_generic);
    snprintf(expected_per_if_str, sizeof(expected_per_if_str),
             "metric-if %s %d", ifname, expected_per_if);

    if(!babel_check(rc >= 0 &&
                    strstr(metrics_buf, expected_generic_str) != NULL &&
                    strstr(metrics_buf, expected_per_if_str) != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr, "Failed test on format_xroute_metrics_labels_test.\n");
        fprintf(stderr, "Expected fragment: %s\n", expected_generic_str);
        fprintf(stderr, "Expected fragment: %s\n", expected_per_if_str);
        fprintf(stderr, "Actual metrics string: %s\n", metrics_buf);
    }
}

static void
prepare_anycast_neighbour(struct neighbour *neigh)
{
    neigh->ifp->flags |= IF_UP;
    neigh->ifp->flags |= IF_LQ;
    if(neigh->ifp->cost == 0)
        neigh->ifp->cost = 96;
    if(neigh->ifp->buf.size < 512)
        neigh->ifp->buf.size = 512;
    neigh->txcost = 96;
    neigh->hello.reach = 0xFFFF;
    neigh->uhello.reach = 0xFFFF;
    neigh->hello.time = now;
    neigh->uhello.time = now;
}

static int
buffered_updates_for_prefix(struct interface *ifp,
                           const unsigned char *prefix,
                           unsigned char plen,
                           unsigned char ids[][8],
                           int max_ids)
{
    int i, j;
    int id_count = 0;

    for(i = 0; i < ifp->num_buffered_updates; i++) {
        struct buffered_update *u = &ifp->buffered_updates[i];
        int seen = 0;

        if(u->plen != plen || memcmp(u->prefix, prefix, 16) != 0)
            continue;

        for(j = 0; j < id_count; j++) {
            if(memcmp(ids[j], u->id, 8) == 0) {
                seen = 1;
                break;
            }
        }

        if(!seen && id_count < max_ids) {
            memcpy(ids[id_count], u->id, 8);
            id_count++;
        }
    }

    return id_count;
}

void anycast_delayed_second_source_enables_multipath_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xA1};
    unsigned char id2[8] = {0xB2};
    unsigned short seq = 100;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1000;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400,
                 n1, nh1, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    if(!babel_check(r1 != NULL && r1->installed)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_delayed_second_source_enables_multipath_test setup (first route not installed).\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;

    now.tv_sec += 20;
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400,
                 n2, nh2, 0);

    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_delayed_second_source_enables_multipath_test (second route missing).\n");
    }

    if(!babel_check(mocked_kernel_multipath_calls > base_mp_calls &&
                mocked_kernel_last_multipath_count >= 2 &&
                r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_delayed_second_source_enables_multipath_test.\n");
        fprintf(stderr,
            "Expected multipath programming with both routes marked installed participants (calls %d->%d, last nh count %d, r1 rank %d, r2 rank %d).\n",
                base_mp_calls, mocked_kernel_multipath_calls,
            mocked_kernel_last_multipath_count,
            r1->installed, r2->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void anycast_send_update_buffers_multiple_source_ids_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x33, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x44, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xC1};
    unsigned char id2[8] = {0xD2};
    unsigned char seen_ids[8][8];
    int seen;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1200;
    update_route(id1, pfx, 128, zeroes, 0, 200, 10, 400,
                 n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, 201, 12, 400,
                 n2, nh2, 0);

    ifp->flags |= IF_UP;
    free(ifp->buffered_updates);
    ifp->buffered_updates = NULL;
    ifp->num_buffered_updates = 0;
    ifp->update_bufsize = 0;

    send_update(ifp, 1, pfx, 128, zeroes, 0);

    memset(seen_ids, 0, sizeof(seen_ids));
    seen = buffered_updates_for_prefix(ifp, pfx, 128, seen_ids, 8);

    if(!babel_check(seen >= 2)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_send_update_buffers_multiple_source_ids_test.\n");
        fprintf(stderr,
                "Expected at least 2 distinct source IDs in buffered updates for anycast prefix, got %d.\n",
                seen);
    }
}

void anycast_hard_retraction_keeps_route_via_other_source_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xE1};
    unsigned char id2[8] = {0xF2};
    unsigned short retract_seqno;
    unsigned char seen_ids[8][8];
    int seen;
    int old_multipath_ecmp = multipath_ecmp;
    int old_enable_hard_withdraw = enable_hard_withdraw;

    multipath_ecmp = ECMP_EQUAL;
    enable_hard_withdraw = 1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1400;
    update_route(id1, pfx, 128, zeroes, 0, 300, 10, 400,
                 n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, 301, 12, 400,
                 n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_hard_retraction_keeps_route_via_other_source_test setup (missing anycast routes).\n");
        multipath_ecmp = old_multipath_ecmp;
        enable_hard_withdraw = old_enable_hard_withdraw;
        return;
    }

    retract_seqno = seqno_plus(r1->seqno, 1);
    update_route(id1, pfx, 128, zeroes, 0,
                 retract_seqno, INFINITY, 0,
                 n1, nh1, 1);

        ifp->flags |= IF_UP;
        free(ifp->buffered_updates);
        ifp->buffered_updates = NULL;
        ifp->num_buffered_updates = 0;
        ifp->update_bufsize = 0;
        send_update(ifp, 1, pfx, 128, zeroes, 0);

        memset(seen_ids, 0, sizeof(seen_ids));
        seen = buffered_updates_for_prefix(ifp, pfx, 128, seen_ids, 8);

        if(!babel_check(r2 != NULL && !r1->installed && seen >= 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_hard_retraction_keeps_route_via_other_source_test.\n");
        fprintf(stderr,
            "Expected hard retraction to remove primary install while preserving alternate source visibility; r2=%p r1_installed=%d buffered_ids=%d.\n",
            (void*)r2, r1->installed, seen);
    }

    multipath_ecmp = old_multipath_ecmp;
    enable_hard_withdraw = old_enable_hard_withdraw;
}

void anycast_ecmp_metric_change_updates_kernel_metric_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x01, 0x01,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x0a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x0a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xaa, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xaa, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xA3};
    unsigned char id2[8] = {0xA4};
    unsigned short seq = 600;
    int old_multipath_ecmp = multipath_ecmp;
    int old_reflect = reflect_kernel_metric;
    int new_kernel_metric;
    int base_mp_calls;

    /* ECMP_WEIGHT mode: metric changes always push updated weights to kernel. */
    multipath_ecmp = ECMP_WEIGHT;
    reflect_kernel_metric = 1;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;
    mocked_kernel_last_new_metric = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1700;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    if(!babel_check(r1 != NULL && r1->installed)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_metric_change_updates_kernel_metric_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        reflect_kernel_metric = old_reflect;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;

    /* Increase r1 refmetric: should trigger a multipath MODIFY with new_metric. */
    new_kernel_metric = metric_to_kernel(MIN(20 + neighbour_cost(n1), INFINITY));
    change_route_metric(r1, 20, neighbour_cost(n1), 0);

    if(!babel_check(mocked_kernel_multipath_calls > base_mp_calls &&
                    mocked_kernel_last_new_metric == new_kernel_metric)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_metric_change_updates_kernel_metric_test.\n");
        fprintf(stderr,
                "Expected multipath MODIFY with new_metric=%d, got mp_calls=%d->%d last_new_metric=%d.\n",
                new_kernel_metric,
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_new_metric);
    }

    multipath_ecmp = old_multipath_ecmp;
    reflect_kernel_metric = old_reflect;
}

void anycast_prefix96_delayed_second_source_enables_multipath_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x01, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x99, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x99, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0x93};
    unsigned char id2[8] = {0x94};
    unsigned short seq = 500;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1600;
    update_route(id1, pfx, 96, zeroes, 0, seq, 10, 400,
                 n1, nh1, 0);

    r1 = find_route(id1, pfx, 96, zeroes, 0, n1);
    if(!babel_check(r1 != NULL && r1->installed)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_prefix96_delayed_second_source_enables_multipath_test setup (first /96 route not installed).\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;

    now.tv_sec += 15;
    update_route(id2, pfx, 96, zeroes, 0, seqno_plus(seq, 1), 12, 400,
                 n2, nh2, 0);

    r2 = find_route(id2, pfx, 96, zeroes, 0, n2);
    if(!babel_check(r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_prefix96_delayed_second_source_enables_multipath_test (second /96 route missing).\n");
    }

    if(!babel_check(mocked_kernel_multipath_calls > base_mp_calls &&
                    mocked_kernel_last_multipath_count >= 2)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_prefix96_delayed_second_source_enables_multipath_test.\n");
        fprintf(stderr,
                "Expected multipath programming for same /96 prefix with delayed second source (calls %d->%d, last nh count %d).\n",
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_multipath_count);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void anycast_second_source_without_ecmp_keeps_singlepath_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x77, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0x91};
    unsigned char id2[8] = {0x92};
    unsigned short seq = 400;
    int old_multipath_ecmp = multipath_ecmp;

    multipath_ecmp = 0;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1500;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400,
                 n1, nh1, 0);
    now.tv_sec += 10;
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400,
                 n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);

    if(!babel_check(r1 != NULL && r2 != NULL &&
                    mocked_kernel_multipath_calls == 0 &&
                    mocked_kernel_route_calls > 0 &&
                ((r1->installed == 1 && r2->installed == 0) ||
                 (r1->installed == 0 && r2->installed == 1)))) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_second_source_without_ecmp_keeps_singlepath_test.\n");
        fprintf(stderr,
            "Expected non-ECMP single-path ranks (primary=1, other=0) with no multipath kernel calls; route_calls=%d mp_calls=%d r1_rank=%d r2_rank=%d.\n",
                mocked_kernel_route_calls,
                mocked_kernel_multipath_calls,
                r1 ? r1->installed : -1,
                r2 ? r2->installed : -1);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void anycast_ecmp_with_unfeasible_alternates_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x02, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xbb, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xbb, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xB1};
    unsigned char id2[8] = {0xB2};
    unsigned short seq = 700;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;
    int mp_calls_after_unfeasible;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1800;
    /* Create first feasible route */
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    
    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    if(!babel_check(r1 != NULL && r1->installed == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_with_unfeasible_alternates_test setup (first route not installed as primary).\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    /* Create second feasible route to enable ECMP */
    now.tv_sec += 15;
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);
    
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r2 != NULL && r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_with_unfeasible_alternates_test setup (ECMP not enabled).\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;

    /* Make r2 unfeasible by sending explicit retraction (infinity metric with TLV) */
    unsigned short unfeasible_seqno = seqno_plus(r2->seqno, 1);
    update_route(id2, pfx, 128, zeroes, 0,
                 unfeasible_seqno, INFINITY, 0,
                 n2, nh2, 1);

    mp_calls_after_unfeasible = mocked_kernel_multipath_calls;
    
    /* After making r2 unfeasible via explicit retraction, it should be excluded from multipath.
       r1 should remain rank 1 (primary), r2 should be rank 0 (not in multipath) */
    if(!babel_check(r2->installed == 0 && r1->installed == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_with_unfeasible_alternates_test.\n");
        fprintf(stderr,
            "Expected unfeasible route excluded from multipath (r2_rank=0, r1_rank=1); got r1_rank=%d r2_rank=%d mp_calls=%d->%d.\n",
                r1->installed, r2->installed,
                base_mp_calls, mp_calls_after_unfeasible);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void anycast_ecmp_shrink_to_single_nexthop_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x03, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xcc, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xcc, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xC1};
    unsigned char id2[8] = {0xC2};
    unsigned short seq = 800;
    unsigned short retract_seqno;
    int old_multipath_ecmp = multipath_ecmp;
    int old_enable_hard_withdraw = enable_hard_withdraw;
    int base_mp_calls;
    int mp_calls_after_retract;

    multipath_ecmp = ECMP_EQUAL;
    enable_hard_withdraw = 1;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_multipath_count = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 1900;
    /* Create first route */
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    
    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    if(!babel_check(r1 != NULL && r1->installed == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_shrink_to_single_nexthop_test setup (first route not installed).\n");
        multipath_ecmp = old_multipath_ecmp;
        enable_hard_withdraw = old_enable_hard_withdraw;
        return;
    }

    /* Create second route to enable ECMP multipath */
    now.tv_sec += 15;
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);
    
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r2 != NULL && r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_shrink_to_single_nexthop_test setup (ECMP not established).\n");
        multipath_ecmp = old_multipath_ecmp;
        enable_hard_withdraw = old_enable_hard_withdraw;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;

    /* Hard retract one route via explicit retraction (sequence bump + INFINITY) */
    retract_seqno = seqno_plus(r1->seqno, 1);
    update_route(id1, pfx, 128, zeroes, 0,
                 retract_seqno, INFINITY, 0,
                 n1, nh1, 1);

    mp_calls_after_retract = mocked_kernel_multipath_calls;

    /* After hard retraction of r1, it should be uninstalled (rank 0),
       r2 should become primary-only (rank 1), not ECMP anymore */
    if(!babel_check(r1->installed == 0 && r2->installed == 1 &&
                    mp_calls_after_retract >= base_mp_calls)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_shrink_to_single_nexthop_test.\n");
        fprintf(stderr,
            "Expected hard retract to shrink ECMP to single-path (r1_rank=0, r2_rank=1); got r1_rank=%d r2_rank=%d mp_calls=%d->%d.\n",
                r1->installed, r2->installed,
                base_mp_calls, mp_calls_after_retract);
    }

    /* Cleanup: retract the remaining finite route so route_tear_down()
       doesn't trigger route_lost() request generation on a live route. */
    if(r2 && r2->refmetric < INFINITY) {
        retract_seqno = seqno_plus(r2->seqno, 1);
        update_route(id2, pfx, 128, zeroes, 0,
                     retract_seqno, INFINITY, 0,
                     n2, nh2, 1);
    }

    multipath_ecmp = old_multipath_ecmp;
    enable_hard_withdraw = old_enable_hard_withdraw;
}

void anycast_ecmp_collapse_to_none_clears_installed_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *installed;
    unsigned char pfx[16] =
           {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x03, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xcc, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xcc, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
        unsigned char id1[8] = {0xC1};
        unsigned char id2[8] = {0xC2};
    unsigned short seq = 3000;
    int old_multipath_ecmp = multipath_ecmp;
    int old_enable_hard_withdraw = enable_hard_withdraw;

    multipath_ecmp = ECMP_EQUAL;
    enable_hard_withdraw = 1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 3000;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_collapse_to_none_clears_installed_test setup (routes missing).\n");
        multipath_ecmp = old_multipath_ecmp;
        enable_hard_withdraw = old_enable_hard_withdraw;
        return;
    }

    r1->installed = 1;
    r2->installed = 0;
    r2->src->metric = INFINITY;

    update_route(id1, pfx, 128, zeroes, 0,
                 seqno_plus(r1->seqno, 1), INFINITY, 0,
                 n1, nh1, 1);

            if(!babel_check(r1->installed == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
            "Failed test on anycast_ecmp_collapse_to_none_clears_installed_test (count=1 promotion).\n");
        fprintf(stderr,
                "Expected retracted route to be uninstalled after first hard retract; got r1_rank=%d r2_rank=%d.\n",
                r1->installed, r2->installed);
        multipath_ecmp = old_multipath_ecmp;
        enable_hard_withdraw = old_enable_hard_withdraw;
        return;
    }

    update_route(id2, pfx, 128, zeroes, 0,
                 seqno_plus(r2->seqno, 1), INFINITY, 0,
                 n2, nh2, 1);

    installed = find_installed_route(NULL, pfx, 128, zeroes, 0, NULL);
    if(!babel_check(r2->installed == 0 && installed == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
            "Failed test on anycast_ecmp_collapse_to_none_clears_installed_test (count=0 collapse).\n");
        fprintf(stderr,
                "Expected no installed route after second hard retract; got r1_rank=%d r2_rank=%d installed=%p.\n",
                r1->installed, r2->installed, (void*)installed);
    }

    multipath_ecmp = old_multipath_ecmp;
    enable_hard_withdraw = old_enable_hard_withdraw;
}

void flush_ecmp_secondary_member_reprograms_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *primary, *secondary;
    struct neighbour *secondary_neigh;
    unsigned char secondary_id[8];
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x03, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xcc, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xcc, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xC1};
    unsigned char id2[8] = {0xC2};
    unsigned short seq = 800;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2300;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_ecmp_secondary_member_reprograms_kernel_test setup.\n");
        fprintf(stderr,
                "r1=%p r2=%p r1_rank=%d r2_rank=%d route_calls=%d mp_calls=%d.\n",
                (void*)r1, (void*)r2,
                r1 ? r1->installed : -1,
                r2 ? r2->installed : -1,
                mocked_kernel_route_calls,
                mocked_kernel_multipath_calls);
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    /* Seed the exact state flush_route needs to exercise: one installed
       primary route and one installed ECMP secondary member in the same slot. */
    primary = r1;
    secondary = r2;
    primary->installed = 1;
    secondary->installed = 2;

    memcpy(secondary_id, secondary->src->id, 8);
    secondary_neigh = secondary->neigh;

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    flush_route(secondary);

    if(!babel_check((mocked_kernel_route_calls > base_route_calls ||
                     mocked_kernel_multipath_calls > base_mp_calls) &&
                    mocked_kernel_last_operation == ROUTE_MODIFY &&
                    primary->installed == 1 &&
                    find_route(secondary_id, pfx, 128, zeroes, 0,
                               secondary_neigh) == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_ecmp_secondary_member_reprograms_kernel_test.\n");
        fprintf(stderr,
                "Expected flushing an installed ECMP secondary to rebuild kernel route and leave only the primary installed; route_calls=%d->%d mp_calls=%d->%d last_op=%d primary_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_operation,
                primary->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void flush_primary_with_alternate_reprograms_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *primary, *alternate;
    struct neighbour *primary_neigh;
    unsigned char primary_id[8];
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x0a, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x64, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x64, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa4, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa4, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xA1};
    unsigned char id2[8] = {0xA2};
    unsigned short seq = 1500;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2600;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_primary_with_alternate_reprograms_kernel_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    primary = r1;
    alternate = r2;
    primary->installed = 1;
    alternate->installed = 2;

    memcpy(primary_id, primary->src->id, 8);
    primary_neigh = primary->neigh;

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    flush_route(primary);

        if(!babel_check((mocked_kernel_route_calls > base_route_calls ||
                 mocked_kernel_multipath_calls > base_mp_calls) &&
                mocked_kernel_last_operation == ROUTE_MODIFY &&
                find_route(primary_id, pfx, 128, zeroes, 0,
                       primary_neigh) == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_primary_with_alternate_reprograms_kernel_test.\n");
        fprintf(stderr,
            "Expected flushing installed primary with an alternate to reprogram kernel and remove primary route; route_calls=%d->%d mp_calls=%d->%d last_op=%d alternate_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_operation,
                alternate->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void flush_neighbour_routes_ecmp_secondary_member_reprograms_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *primary, *secondary;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x08, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x62, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x62, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa2, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa2, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0x81};
    unsigned char id2[8] = {0x82};
    unsigned short seq = 1300;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2400;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_neighbour_routes_ecmp_secondary_member_reprograms_kernel_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    primary = r1;
    secondary = r2;
    primary->installed = 1;
    secondary->installed = 2;

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    flush_neighbour_routes(secondary->neigh);

    if(!babel_check((mocked_kernel_route_calls > base_route_calls ||
                     mocked_kernel_multipath_calls > base_mp_calls) &&
                    mocked_kernel_last_operation == ROUTE_MODIFY &&
                    primary->installed == 1 &&
                    find_route(id2, pfx, 128, zeroes, 0, n2) == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_neighbour_routes_ecmp_secondary_member_reprograms_kernel_test.\n");
        fprintf(stderr,
                "Expected neighbour flush of an ECMP secondary to rebuild kernel route and remove only that route; route_calls=%d->%d mp_calls=%d->%d last_op=%d primary_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_operation,
                primary->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void flush_interface_routes_ecmp_secondary_member_reprograms_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct interface *ifp2 = add_interface("test_if_ecmp_if2", NULL);
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *primary, *secondary;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x09, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x63, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x63, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa3, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xa3, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0x91};
    unsigned char id2[8] = {0x92};
    unsigned short seq = 1400;
    int old_multipath_ecmp = multipath_ecmp;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;
    mocked_kernel_last_operation = -1;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp2);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2500;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_interface_routes_ecmp_secondary_member_reprograms_kernel_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        return;
    }

    primary = r1;
    secondary = r2;
    primary->installed = 1;
    secondary->installed = 2;

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    flush_interface_routes(ifp2, 0);

    if(!babel_check((mocked_kernel_route_calls > base_route_calls ||
                     mocked_kernel_multipath_calls > base_mp_calls) &&
                    mocked_kernel_last_operation == ROUTE_MODIFY &&
                    primary->installed == 1 &&
                    find_route(id2, pfx, 128, zeroes, 0, n2) == NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on flush_interface_routes_ecmp_secondary_member_reprograms_kernel_test.\n");
        fprintf(stderr,
                "Expected interface flush of an ECMP secondary to rebuild kernel route and remove only that route; route_calls=%d->%d mp_calls=%d->%d last_op=%d primary_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                mocked_kernel_last_operation,
                primary->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
}

void anycast_ecmp_equal_metric_change_does_not_reprogram_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x04, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xdd, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xdd, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xD1};
    unsigned char id2[8] = {0xD2};
    unsigned short seq = 900;
    int old_multipath_ecmp = multipath_ecmp;
    int old_reflect = reflect_kernel_metric;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    reflect_kernel_metric = 1;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2000;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL && r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_metric_change_does_not_reprogram_kernel_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        reflect_kernel_metric = old_reflect;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    /* Finite metric change, same nexthop set: ECMP_EQUAL must not reprogram kernel. */
    change_route_metric(r1, 20, neighbour_cost(n1), 0);

    if(!babel_check(mocked_kernel_multipath_calls == base_mp_calls &&
                    mocked_kernel_route_calls == base_route_calls &&
                    r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_metric_change_does_not_reprogram_kernel_test.\n");
        fprintf(stderr,
                "Expected no kernel reprogramming for finite metric-only change in ECMP_EQUAL; route_calls=%d->%d mp_calls=%d->%d r1_rank=%d r2_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                r1->installed, r2->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
    reflect_kernel_metric = old_reflect;
}

void anycast_ecmp_equal_infinity_retraction_reprograms_kernel_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x05, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x41, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x41, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xee, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xee, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xE1};
    unsigned char id2[8] = {0xE2};
    unsigned short seq = 1000;
    int old_multipath_ecmp = multipath_ecmp;
    int old_reflect = reflect_kernel_metric;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    reflect_kernel_metric = 1;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2100;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL && r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_infinity_retraction_reprograms_kernel_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        reflect_kernel_metric = old_reflect;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    /* INFINITY retraction must reprogram kernel in ECMP_EQUAL. */
    change_route_metric(r1, INFINITY, INFINITY, 0);

        if(!babel_check((mocked_kernel_multipath_calls > base_mp_calls ||
                 mocked_kernel_route_calls > base_route_calls) &&
                (r1->installed == 1 || r2->installed == 1) &&
                !(r1->installed > 0 && r2->installed > 0))) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_infinity_retraction_reprograms_kernel_test.\n");
        fprintf(stderr,
            "Expected kernel reprogramming on INFINITY retraction in ECMP_EQUAL and no dual-active ECMP set; route_calls=%d->%d mp_calls=%d->%d r1_rank=%d r2_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                r1->installed, r2->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
    reflect_kernel_metric = old_reflect;
}

void anycast_ecmp_equal_repeated_finite_metric_changes_skip_reprogram_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x06, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x51, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x51, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xff, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xff, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xF1};
    unsigned char id2[8] = {0xF2};
    unsigned short seq = 1100;
    int old_multipath_ecmp = multipath_ecmp;
    int old_reflect = reflect_kernel_metric;
    int base_mp_calls;
    int base_route_calls;

    multipath_ecmp = ECMP_EQUAL;
    reflect_kernel_metric = 1;
    mocked_kernel_route_calls = 0;
    mocked_kernel_multipath_calls = 0;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2200;
    update_route(id1, pfx, 128, zeroes, 0, seq, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, seqno_plus(seq, 1), 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    if(!babel_check(r1 != NULL && r2 != NULL && r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_repeated_finite_metric_changes_skip_reprogram_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        reflect_kernel_metric = old_reflect;
        return;
    }

    base_mp_calls = mocked_kernel_multipath_calls;
    base_route_calls = mocked_kernel_route_calls;

    change_route_metric(r1, 20, neighbour_cost(n1), 0);
    change_route_metric(r1, 24, neighbour_cost(n1), 0);
    change_route_metric(r1, 28, neighbour_cost(n1), 0);

    if(!babel_check(mocked_kernel_multipath_calls == base_mp_calls &&
                    mocked_kernel_route_calls == base_route_calls &&
                    r1->installed > 0 && r2->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on anycast_ecmp_equal_repeated_finite_metric_changes_skip_reprogram_test.\n");
        fprintf(stderr,
                "Expected repeated finite metric changes to avoid kernel reprogramming in ECMP_EQUAL; route_calls=%d->%d mp_calls=%d->%d r1_rank=%d r2_rank=%d.\n",
                base_route_calls, mocked_kernel_route_calls,
                base_mp_calls, mocked_kernel_multipath_calls,
                r1->installed, r2->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
    reflect_kernel_metric = old_reflect;
}

    void singlepath_metric_change_is_coalesced_test(void)
    {
        struct babel_route *route;
        int old_multipath_ecmp = multipath_ecmp;
        int old_coalesce = route_metric_coalesce_msec;
        int old_reflect = reflect_kernel_metric;
        int base_route_calls;

        multipath_ecmp = ECMP_DISABLED;
        route_metric_coalesce_msec = 1000;
        reflect_kernel_metric = 1;

        route = routes[0];
        if(!babel_check(route != NULL && route->installed == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
            "Failed test on singlepath_metric_change_is_coalesced_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        route_metric_coalesce_msec = old_coalesce;
        reflect_kernel_metric = old_reflect;
        return;
        }

        mocked_kernel_route_calls = 0;
        base_route_calls = mocked_kernel_route_calls;

        now.tv_sec = 3200;
        change_route_metric(route,
                MIN((unsigned)route->refmetric + 8, INFINITY - 1),
                route->cost,
                route->add_metric);

        if(!babel_check(mocked_kernel_route_calls == base_route_calls &&
                route->metric_update_pending == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
            "Failed test on singlepath_metric_change_is_coalesced_test immediate phase.\n");
        fprintf(stderr,
            "Expected deferred update with no immediate kernel call; route_calls=%d pending=%d.\n",
            mocked_kernel_route_calls, route->metric_update_pending);
        multipath_ecmp = old_multipath_ecmp;
        route_metric_coalesce_msec = old_coalesce;
        reflect_kernel_metric = old_reflect;
        return;
        }

        now.tv_sec = 3201;
        route_flush_coalesced_metric_updates();

        if(!babel_check(mocked_kernel_route_calls == base_route_calls + 2 &&
                route->metric_update_pending == 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
            "Failed test on singlepath_metric_change_is_coalesced_test flush phase.\n");
        fprintf(stderr,
            "Expected deferred flush+add after 1s; route_calls=%d pending=%d.\n",
            mocked_kernel_route_calls, route->metric_update_pending);
        }

        multipath_ecmp = old_multipath_ecmp;
        route_metric_coalesce_msec = old_coalesce;
        reflect_kernel_metric = old_reflect;
    }

void route_stream_traverses_multipath_members_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct route_stream *stream;
    struct babel_route *r;
    int seen1 = 0, seen2 = 0;
    int old_multipath_ecmp = multipath_ecmp;
    int old_window = ecmp_metric_window;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x0c, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x72, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x72, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xc1, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xc1, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xC1};
    unsigned char id2[8] = {0xC2};

    multipath_ecmp = ECMP_EQUAL;
    ecmp_metric_window = 200;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2800;
    update_route(id1, pfx, 128, zeroes, 0, 1800, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, 1801, 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    stream = route_stream(0);
    if(stream) {
        while((r = route_stream_next(stream)) != NULL) {
            if(r == r1)
                seen1 = 1;
            else if(r == r2)
                seen2 = 1;
        }
        route_stream_done(stream);
    }

    if(!babel_check(r1 != NULL && r2 != NULL && seen1 && seen2)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on route_stream_traverses_multipath_members_test.\n");
    }

    multipath_ecmp = old_multipath_ecmp;
    ecmp_metric_window = old_window;
}

void refresh_promotes_member_preserving_group_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2;
    struct babel_route *r1, *r2;
    struct babel_route *head, *member;
    int slot;
    int old_multipath_ecmp = multipath_ecmp;
    int old_window = ecmp_metric_window;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x0d, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xd1, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xd1, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char id1[8] = {0xD1};
    unsigned char id2[8] = {0xD2};

    multipath_ecmp = ECMP_EQUAL;
    ecmp_metric_window = 200;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);

    now.tv_sec = 2900;
    update_route(id1, pfx, 128, zeroes, 0, 1900, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, 1901, 12, 400, n2, nh2, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    slot = find_route_slot(NULL, pfx, 128, zeroes, 0, NULL);

    if(!babel_check(r1 != NULL && r2 != NULL && slot >= 0 && routes[slot] != NULL)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on refresh_promotes_member_preserving_group_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        ecmp_metric_window = old_window;
        return;
    }

    head = routes[slot];
    member = (head == r1) ? r2 : r1;

    head->refmetric = INFINITY;
    refresh_installed_ranks(member);

    if(!babel_check(routes[slot] == member &&
                    member->multipath == head &&
                    head->next == NULL &&
                    member->installed == 1)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on refresh_promotes_member_preserving_group_test.\n");
    }

    /* Restore finite metric state to keep teardown behaviour deterministic. */
    head->refmetric = 10;

    multipath_ecmp = old_multipath_ecmp;
    ecmp_metric_window = old_window;
}

void refresh_installed_ranks_preserves_other_groups_test(void)
{
    struct interface *ifp = ns[0]->ifp;
    struct neighbour *n1, *n2, *n3;
    struct babel_route *r1, *r2, *r3;
    int slot;
    int old_multipath_ecmp = multipath_ecmp;
    int old_window = ecmp_metric_window;
    unsigned char pfx[16] =
        {0xfd, 0x72, 0x1f, 0xdc, 0x5c, 0x49, 0x0d, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char nh1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11};
    unsigned char nh2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x22};
    unsigned char nh3[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x33};
    unsigned char a1[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xe1, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    unsigned char a2[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xe1, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    unsigned char a3[16] =
        {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0xe1, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    unsigned char id1[8] = {0xE1};
    unsigned char id2[8] = {0xE2};
    unsigned char id3[8] = {0xE3};

    multipath_ecmp = ECMP_EQUAL;
    ecmp_metric_window = 50;

    n1 = find_neighbour(a1, ifp);
    n2 = find_neighbour(a2, ifp);
    n3 = find_neighbour(a3, ifp);
    prepare_anycast_neighbour(n1);
    prepare_anycast_neighbour(n2);
    prepare_anycast_neighbour(n3);

    now.tv_sec = 3000;
    update_route(id1, pfx, 128, zeroes, 0, 2000, 10, 400, n1, nh1, 0);
    update_route(id2, pfx, 128, zeroes, 0, 2001, 12, 420, n2, nh2, 0);
    update_route(id3, pfx, 128, zeroes, 0, 2002, 14, 900, n3, nh3, 0);

    r1 = find_route(id1, pfx, 128, zeroes, 0, n1);
    r2 = find_route(id2, pfx, 128, zeroes, 0, n2);
    r3 = find_route(id3, pfx, 128, zeroes, 0, n3);
    slot = find_route_slot(NULL, pfx, 128, zeroes, 0, NULL);

    if(!babel_check(r1 != NULL && r2 != NULL && r3 != NULL && slot >= 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on refresh_installed_ranks_preserves_other_groups_test setup.\n");
        multipath_ecmp = old_multipath_ecmp;
        ecmp_metric_window = old_window;
        return;
    }

    routes[slot] = r1;
    r1->next = r3;
    r1->multipath = r2;
    r2->next = NULL;
    r2->multipath = NULL;
    r3->next = NULL;
    r3->multipath = NULL;

    r3->installed = 7;
    refresh_installed_ranks(r1);

        if(!babel_check(r3->installed == 7 &&
                r1->installed > 0)) {
        fprintf(stderr, "-----------------------------------------------\n");
        fprintf(stderr,
                "Failed test on refresh_installed_ranks_preserves_other_groups_test.\n");
        fprintf(stderr,
            "installed values: r1=%d r2=%d r3=%d\n",
            r1->installed, r2->installed, r3->installed);
    }

    multipath_ecmp = old_multipath_ecmp;
    ecmp_metric_window = old_window;
}

void route_setup(void) {
    static unsigned short setup_seqno = 0;
    unsigned short seqno;
    int i;
    struct interface *ifp = add_interface("test_if", NULL);
    unsigned char next_hops[][16] =
      {
        { 116, 183, 7, 94, 183, 40, 143, 20, 251, 193, 125, 15, 37, 226, 212, 149 },
        { 221, 72, 210, 3, 227, 190, 71, 159, 76, 55, 112, 69, 199, 37, 117, 59 },
        { 220, 124, 153, 147, 164, 40, 167, 160, 234, 37, 175, 15, 7, 131, 164, 228 },
        { 204, 118, 231, 175, 52, 46, 78, 128, 102, 190, 197, 45, 227, 59, 104, 191 },
        { 183, 2, 83, 92, 42, 250, 252, 20, 31, 171, 35, 38, 47, 200, 11, 251 },
        { 62, 242, 170, 115, 33, 249, 243, 135, 183, 185, 180, 155, 244, 28, 90, 171 },
      };
    unsigned char neigh_addresses[][16] =
      {
        { 11, 192, 14, 226, 201, 183, 167, 80, 75, 132, 129, 96, 129, 53, 20, 225 },
        { 166, 108, 155, 153, 212, 135, 74, 110, 123, 32, 24, 125, 212, 248, 2, 223 },
        { 184, 16, 193, 129, 199, 104, 209, 18, 236, 82, 114, 110, 135, 135, 79, 45 },
        { 243, 235, 198, 200, 114, 17, 54, 237, 49, 78, 107, 5, 70, 109, 228, 255 },
        { 125, 165, 128, 69, 13, 82, 87, 250, 164, 202, 104, 44, 81, 183, 89, 68 },
        { 162, 32, 12, 21, 49, 67, 2, 98, 145, 109, 103, 216, 218, 75, 215, 88 },
      };
    unsigned char prefixes[][16] =
      {
        { 69, 198, 228, 78, 253, 128, 30, 115, 115, 189, 34, 209, 203, 126, 38, 62 },
        { 78, 162, 240, 49, 189, 24, 46, 203, 201, 107, 41, 160, 213, 182, 197, 23 },
        { 93, 135, 206, 145, 214, 232, 94, 9, 247, 22, 71, 251, 157, 3, 77, 167 },
        { 118, 204, 77, 156, 52, 93, 35, 51, 137, 29, 164, 158, 179, 101, 255, 252 },
        { 160, 175, 139, 76, 149, 129, 138, 109, 209, 43, 127, 92, 8, 202, 53, 182 },
        { 227, 216, 75, 160, 38, 254, 131, 189, 88, 42, 56, 139, 244, 255, 11, 82 },
      };
    int plens[] = {77, 101, 105, 12, 40, 25};
    unsigned char src_prefixes[][16] =
      {
        { 24, 27, 163, 100, 57, 21, 220, 196, 63, 155, 246, 218, 80, 49, 160, 174 },
        { 26, 137, 255, 238, 199, 6, 224, 128, 87, 142, 8, 197, 49, 142, 106, 113 },
        { 107, 103, 113, 193, 138, 153, 175, 32, 159, 28, 70, 247, 160, 25, 204, 190 },
        { 153, 214, 219, 41, 222, 82, 207, 131, 155, 79, 202, 239, 25, 208, 233, 179 },
        { 157, 105, 76, 111, 96, 98, 35, 253, 235, 49, 69, 120, 108, 140, 34, 198 },
        { 173, 76, 71, 184, 21, 200, 70, 185, 15, 19, 223, 62, 165, 179, 210, 92 },
      };
    int src_plens[] = {100, 115, 96, 50, 37, 81};

    // Install artificial filter
    struct filter *filter = calloc(1, sizeof(struct filter));
    filter->plen_le = 128;
    filter->src_plen_le = 128;
    filter->action.table_count = 1;
    filter->action.tables[0] = 254;
    add_filter(filter, FILTER_TYPE_INSTALL);

    setup_seqno = seqno_plus(setup_seqno, 1);
    seqno = setup_seqno;

    for(i = 0; i < N_ROUTES; i++) {
        const unsigned char id[] = {i};
        struct neighbour *n = find_neighbour(neigh_addresses[i], ifp);
        update_route(id, prefixes[i], plens[i], src_prefixes[i], src_plens[i],
                     seqno, 10, 0, n, next_hops[i], 0);
        ns[i] = n;
    }
}

void route_tear_down(void) {
    int i;

    for(i = 0; i < route_slots; i++) {
        struct babel_route *head = routes[i];
        while(head) {
            struct babel_route *r = head;
            while(r) {
                if(r->refmetric < INFINITY)
                    change_route_metric(r, INFINITY, INFINITY, 0);
                r = r->multipath;
            }
            head = head->next;
        }
    }

    flush_all_routes();
}

void run_route_test(void (*test)(void), char *test_name) {
    route_setup();
    run_test(test, test_name);
    route_tear_down();
}

void route_test_suite(void)
{
    run_test(route_compare_test, "route_compare_test");
    run_test(format_xroute_metrics_labels_test,
             "format_xroute_metrics_labels_test");
    run_route_test(find_route_slot_test, "find_route_slot_test");
    run_route_test(find_route_test, "find_route_test");
    run_route_test(find_installed_route_test, "find_installed_route_test");
    run_route_test(installed_routes_estimate_test, "installed_routes_estimate_test");
    run_route_test(insert_route_test, "insert_route_test");
    run_route_test(flush_route_test, "flush_route_test");
    run_route_test(flush_all_routes_test, "flush_all_routes_test");
    run_route_test(flush_neighbour_route_test, "flush_neighbour_route_test");
    run_test(route_stream_test, "route_stream_test");
    run_route_test(route_stream_next_test, "route_stream_next_test");
    run_test(metric_to_kernel_test, "metric_to_kernel_test");
    run_test(update_feasible_test, "update_feasible_test");
    run_test(change_smoothing_half_life_test, "change_smoothing_half_life_test");
    run_route_test(change_route_metric_test, "change_route_metric_test");
    run_test(flush_xroute_reannounces_more_specific_test,
             "flush_xroute_reannounces_more_specific_test");
    run_route_test(kernel_delete_retracted_route_clears_installed_state_test,
                   "kernel_delete_retracted_route_clears_installed_state_test");
    run_route_test(kernel_delete_old_reachable_state_during_retraction_is_ignored_test,
                   "kernel_delete_old_reachable_state_during_retraction_is_ignored_test");
    run_route_test(explicit_retraction_uninstalls_route_test,
                   "explicit_retraction_uninstalls_route_test");
    run_route_test(explicit_retraction_without_tlv_keeps_installed_test,
                   "explicit_retraction_without_tlv_keeps_installed_test");
    run_test(kernel_delete_babel_proto_flushes_matching_xroute_test,
             "kernel_delete_babel_proto_flushes_matching_xroute_test");
    run_route_test(anycast_delayed_second_source_enables_multipath_test,
                   "anycast_delayed_second_source_enables_multipath_test");
    run_route_test(anycast_send_update_buffers_multiple_source_ids_test,
                   "anycast_send_update_buffers_multiple_source_ids_test");
    run_route_test(anycast_hard_retraction_keeps_route_via_other_source_test,
                   "anycast_hard_retraction_keeps_route_via_other_source_test");
    run_route_test(anycast_second_source_without_ecmp_keeps_singlepath_test,
                   "anycast_second_source_without_ecmp_keeps_singlepath_test");
    run_route_test(anycast_prefix96_delayed_second_source_enables_multipath_test,
                   "anycast_prefix96_delayed_second_source_enables_multipath_test");
    run_route_test(anycast_ecmp_metric_change_updates_kernel_metric_test,
                   "anycast_ecmp_metric_change_updates_kernel_metric_test");
    run_route_test(anycast_ecmp_with_unfeasible_alternates_test,
                   "anycast_ecmp_with_unfeasible_alternates_test");
    run_route_test(anycast_ecmp_shrink_to_single_nexthop_test,
                   "anycast_ecmp_shrink_to_single_nexthop_test");
    run_route_test(anycast_ecmp_collapse_to_none_clears_installed_test,
                   "anycast_ecmp_collapse_to_none_clears_installed_test");
    run_route_test(flush_ecmp_secondary_member_reprograms_kernel_test,
                   "flush_ecmp_secondary_member_reprograms_kernel_test");
    run_route_test(flush_primary_with_alternate_reprograms_kernel_test,
                   "flush_primary_with_alternate_reprograms_kernel_test");
    run_route_test(flush_neighbour_routes_ecmp_secondary_member_reprograms_kernel_test,
                   "flush_neighbour_routes_ecmp_secondary_member_reprograms_kernel_test");
    run_route_test(flush_interface_routes_ecmp_secondary_member_reprograms_kernel_test,
                   "flush_interface_routes_ecmp_secondary_member_reprograms_kernel_test");
    run_route_test(anycast_ecmp_equal_metric_change_does_not_reprogram_kernel_test,
                   "anycast_ecmp_equal_metric_change_does_not_reprogram_kernel_test");
    run_route_test(anycast_ecmp_equal_infinity_retraction_reprograms_kernel_test,
                   "anycast_ecmp_equal_infinity_retraction_reprograms_kernel_test");
    run_route_test(anycast_ecmp_equal_repeated_finite_metric_changes_skip_reprogram_test,
                   "anycast_ecmp_equal_repeated_finite_metric_changes_skip_reprogram_test");
    run_route_test(singlepath_metric_change_is_coalesced_test,
                   "singlepath_metric_change_is_coalesced_test");
    run_route_test(route_stream_traverses_multipath_members_test,
                   "route_stream_traverses_multipath_members_test");
    run_route_test(refresh_promotes_member_preserving_group_test,
                   "refresh_promotes_member_preserving_group_test");
    run_route_test(refresh_installed_ranks_preserves_other_groups_test,
                   "refresh_installed_ranks_preserves_other_groups_test");
    fprintf(stderr, "-----------------------------------------------\n");
    fprintf(stderr, "Executed tests\n");
}

/*
Copyright 2007-2010 by Grégoire Henry, Julien Cristau and Juliusz Chroboczek

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

#include "kernel.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

int export_table = -1, import_table_count = 0, import_tables[MAX_IMPORT_TABLES];
int mocked_kernel_route_calls = 0;
int mocked_kernel_multipath_calls = 0;
int mocked_kernel_last_multipath_count = 0;
int mocked_kernel_last_operation = -1;
int mocked_kernel_last_new_metric = -1;
struct kernel_addr *mocked_kernel_addresses = NULL;
int mocked_kernel_address_count = 0;
struct kernel_route *mocked_kernel_routes = NULL;
int mocked_kernel_route_count = 0;

int
if_eui64(char *ifname, int ifindex, unsigned char *eui)
{
    return 0;
}

int
kernel_setup(int setup)
{
    return 0;
}

int
kernel_setup_socket(int setup)
{
    return 0;
}

int
kernel_setup_interface(int setup, const char *ifname, int ifindex)
{
    return 0;
}

int
kernel_interface_operational(const char *ifname, int ifindex)
{
    return 0;
}

int
kernel_interface_ipv4(const char *ifname, int ifindex, unsigned char *addr_r)
{
    return 0;
}

int
kernel_interface_mtu(const char *ifname, int ifindex)
{
    return 0;
}

int
kernel_interface_wireless(const char *ifname, int ifindex)
{
    return 0;
}

int
kernel_safe_v4viav6(void)
{
    return 0;
}

int
kernel_route(int operation, int table,
             const unsigned char *dest, unsigned short plen,
             const unsigned char *src, unsigned short src_plen,
             const unsigned char *pref_src,
             const unsigned char *gate, int ifindex, unsigned int metric,
             const unsigned char *newgate, int newifindex,
             unsigned int newmetric, int newtable,
             const unsigned char *newpref_src)
{
    mocked_kernel_route_calls++;
    mocked_kernel_last_operation = operation;
    return 0;
}

int
kernel_route_multipath(int operation, int table,
                       const unsigned char *dest, unsigned short plen,
                       const unsigned char *src, unsigned short src_plen,
                       const unsigned char *pref_src,
                       unsigned int metric,
                       unsigned int newmetric,
                       const struct kernel_nexthop *nexthops,
                       int nexthop_count,
                       int newtable,
                       const unsigned char *newpref_src)
{
    mocked_kernel_multipath_calls++;
    mocked_kernel_last_multipath_count = nexthop_count;
    mocked_kernel_last_operation = operation;
    mocked_kernel_last_new_metric = (int)newmetric;
    return 0;
}

int
kernel_dump(int operation, struct kernel_filter *filter)
{
    int i;

    if((operation & CHANGE_ADDR) && filter->addr) {
        for(i = 0; i < mocked_kernel_address_count; i++)
            filter->addr(1, &mocked_kernel_addresses[i], filter->addr_closure);
    }
    if((operation & CHANGE_ROUTE) && filter->route) {
        for(i = 0; i < mocked_kernel_route_count; i++) {
            if(mocked_kernel_routes[i].proto != RTPROT_BABEL)
                filter->route(1, &mocked_kernel_routes[i], filter->route_closure);
        }
    }
    return 0;
}

void
kernel_set_audit_route_tables(const int *tables, int table_count)
{
    /* The test backend has no per-table dump optimisation. */
}

int
kernel_dump_babel(int operation, struct kernel_filter *filter)
{
    int i;

    if((operation & CHANGE_ROUTE) && filter->route) {
        for(i = 0; i < mocked_kernel_route_count; i++) {
            if(mocked_kernel_routes[i].proto == RTPROT_BABEL)
                filter->route(1, &mocked_kernel_routes[i], filter->route_closure);
        }
    }
    return 0;
}

int
kernel_callback(struct kernel_filter *filter)
{
    return 0;
}

#include "route_test.h"
#include "test_utilities.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    run_test(kernel_delete_babel_proto_flushes_matching_xroute_test,
             "kernel_delete_babel_proto_flushes_matching_xroute_test");

    return tests_failed;
}

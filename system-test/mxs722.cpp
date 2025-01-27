/**
 * @file mxs722.cpp MaxScale configuration check functionality test
 *
 * - Get baseline for test from a valid config
 * - Test wrong parameter name
 * - Test wrong router_options value
 * - Test wrong filter parameter
 * - Test missing config file
 */


#include <iostream>
#include <unistd.h>
#include <maxtest/testconnections.hh>

using namespace std;

int main(int argc, char* argv[])
{
    TestConnections* test = new TestConnections(argc, argv);
    test->stop_timeout();
    test->maxscales->stop();

    /** Copy original config so we can easily reset the testing environment */
    test->maxscales->ssh_node_f(0, true, "cp /etc/maxscale.cnf /tmp/maxscale.cnf");
    test->maxscales->ssh_node_f(0, true, "chmod a+rw /tmp/maxscale.cnf");

    /** Get a baseline result with a good configuration */
    int baseline = test->maxscales->ssh_node_f(0, true, "maxscale -c --user=maxscale -f /tmp/maxscale.cnf");

    /** Configure bad parameter for a listener */
    test->maxscales->ssh_node_f(0, true, "sed -i -e 's/service/ecivres/' /tmp/maxscale.cnf");
    test->add_result(baseline == test->maxscales->ssh_node_f(0, true, "maxscale -c --user=maxscale -f /tmp/maxscale.cnf"),
                     "Bad parameter name should be detected.\n");
    test->maxscales->ssh_node_f(0, true, "cp /etc/maxscale.cnf /tmp/maxscale.cnf");

    /** Set router_options to a bad value */
    test->maxscales->ssh_node_f(0,
                                true,
                                "sed -i -e 's/router_options.*/router_options=bad_option=true/' /tmp/maxscale.cnf");
    test->add_result(baseline == test->maxscales->ssh_node_f(0, true, "maxscale -c --user=maxscale -f /tmp/maxscale.cnf"),
                     "Bad router_options should be detected.\n");

    test->maxscales->ssh_node_f(0, true, "cp /etc/maxscale.cnf /tmp/maxscale.cnf");

    /** Configure bad filter parameter */
    test->maxscales->ssh_node_f(0, true, "sed -i -e 's/filebase/basefile/' /tmp/maxscale.cnf");
    test->add_result(baseline == test->maxscales->ssh_node_f(0, true, "maxscale -c --user=maxscale -f /tmp/maxscale.cnf"),
                     "Bad filter parameter should be detected.\n");

    /** Remove configuration file */
    test->maxscales->ssh_node_f(0, true, "rm -f /tmp/maxscale.cnf");
    test->add_result(baseline == test->maxscales->ssh_node_f(0, true, "maxscale -c --user=maxscale -f /tmp/maxscale.cnf"),
                     "Missing configuration file should be detected.\n");

    int rval = test->global_result;
    delete test;
    return rval;
}

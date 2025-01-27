/**
 * @file max621_unreadable_cnf.cpp mxs621 regression case ("MaxScale fails to start silently if config file is
 * not readable")
 *
 * - make maxscale.cnf unreadable
 * - try to restart Maxscale
 * - check log for error
 * - retore access rights to maxscale.cnf
 */


#include <iostream>
#include <unistd.h>
#include <maxtest/testconnections.hh>

using namespace std;

int main(int argc, char* argv[])
{
    TestConnections* Test = new TestConnections(argc, argv);
    Test->set_timeout(30);
    Test->maxscales->ssh_node_f(0, true, "chmod 400 /etc/maxscale.cnf");
    Test->set_timeout(30);
    Test->maxscales->restart_maxscale();
    Test->set_timeout(30);
    Test->log_includes(0, "Opening file '/etc/maxscale.cnf' for reading failed");
    Test->set_timeout(30);
    Test->maxscales->ssh_node_f(0, true, "chmod 777 /etc/maxscale.cnf");

    int rval = Test->global_result;
    delete Test;
    return rval;
}

/**
 * @file mxs118.cpp bug mxs118 regression case ("Two monitors loaded at the same time result into not working
 * installation")
 *
 * - Configure two monitors using same backend serves
 * - try to connect to maxscale
 * - check logs for warning
 */

#include <iostream>
#include <unistd.h>
#include <maxtest/testconnections.hh>

using namespace std;

int main(int argc, char* argv[])
{
    TestConnections* Test = new TestConnections(argc, argv);
    Test->set_timeout(10);
    Test->maxscales->connect_maxscale();

    Test->log_includes(0, "is already monitored by");

    int rval = Test->global_result;
    delete Test;
    return rval;
}

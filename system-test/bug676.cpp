/**
 * @file bug676.cpp  reproducing attempt for bug676
 * - connect to RWSplit
 * - stop node0
 * - sleep 20 seconds
 * - reconnect
 * - check if 'USE test' is ok
 * - check MaxScale is alive
 */

#include <iostream>
#include <maxtest/testconnections.hh>
#include <maxtest/galera_cluster.hh>

int main(int argc, char* argv[])
{
    TestConnections::require_galera(true);
    TestConnections test(argc, argv);

    test.set_timeout(30);

    test.maxscales->connect_maxscale();
    test.tprintf("Stopping node 0");
    test.galera->block_node(0);
    test.maxscales->close_maxscale_connections();

    test.stop_timeout();

    test.tprintf("Waiting until the monitor picks a new master");
    test.maxscales->wait_for_monitor();

    test.set_timeout(30);

    test.maxscales->connect_maxscale();
    test.try_query(test.maxscales->conn_rwsplit[0], "USE test");
    test.try_query(test.maxscales->conn_rwsplit[0], "show processlist;");
    test.maxscales->close_maxscale_connections();

    test.stop_timeout();

    test.galera->unblock_node(0);

    return test.global_result;
}

/**
 * Check that SQL_MODE='PAD_CHAR_TO_FULL_LENGTH' doesn't break authentication
 */

#include <maxtest/testconnections.hh>

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);

    test.tprintf("Changing SQL_MODE to PAD_CHAR_TO_FULL_LENGTH and restarting MaxScale");
    test.repl->connect();
    test.repl->execute_query_all_nodes("SET GLOBAL SQL_MODE='PAD_CHAR_TO_FULL_LENGTH'");
    test.maxscales->restart_maxscale();

    test.tprintf("Connecting to MaxScale and executing a query");
    test.maxscales->connect_maxscale();
    test.try_query(test.maxscales->conn_rwsplit[0], "SELECT 1");
    test.maxscales->close_maxscale_connections();

    test.repl->execute_query_all_nodes("SET GLOBAL SQL_MODE=DEFAULT");
    return test.global_result;
}

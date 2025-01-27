/**
 * @file Check that removing a server from a service doesn't break active connections
 */

#include <maxtest/testconnections.hh>

static volatile bool running = true;

void* thr(void* data)
{
    TestConnections* test = (TestConnections*)data;

    while (running && test->global_result == 0)
    {
        test->set_timeout(60);
        if (test->try_query(test->maxscales->conn_rwsplit[0], "SELECT 1"))
        {
            test->tprintf("Failed to select via readwritesplit");
        }
        if (test->try_query(test->maxscales->conn_master[0], "SELECT 1"))
        {
            test->tprintf("Failed to select via readconnroute master");
        }
        if (test->try_query(test->maxscales->conn_slave[0], "SELECT 1"))
        {
            test->tprintf("Failed to select via readconnroute slave");
        }
    }

    test->stop_timeout();

    return NULL;
}

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);
    test.maxscales->connect_maxscale();

    test.tprintf("Connect to MaxScale and continuously execute queries");
    pthread_t thread;
    pthread_create(&thread, NULL, thr, &test);
    sleep(5);

    test.tprintf("Remove all servers from all services");

    test.maxctrl("unlink service RW-Split-Router server1 server2 server3 server4");
    test.maxctrl("unlink service Read-Connection-Router-Slave server1 server2 server3 server4");
    test.maxctrl("unlink service Read-Connection-Router-Master server1 server2 server3 server4");

    sleep(5);

    test.tprintf("Stop queries and close the connections");
    running = false;
    pthread_join(thread, NULL);
    test.maxscales->close_maxscale_connections();

    test.tprintf("Add all servers to all services");

    test.maxctrl("link service RW-Split-Router server1 server2 server3 server4");
    test.maxctrl("link service Read-Connection-Router-Slave server1 server2 server3 server4");
    test.maxctrl("link service Read-Connection-Router-Master server1 server2 server3 server4");

    test.check_maxscale_alive(0);

    return test.global_result;
}

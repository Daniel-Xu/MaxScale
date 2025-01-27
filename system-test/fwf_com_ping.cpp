/**
 * MXS-1111: Dbfwfilter COM_PING test
 *
 * Check that COM_PING is allowed with `action=allow`
 */

#include <maxtest/testconnections.hh>
#include <maxtest/fw_copy_rules.hh>

const char* rules = "rule test1 deny regex '.*'\n"
                    "users %@% match any rules test1\n";

int main(int argc, char** argv)
{
    /** Create the rule file */
    FILE* file = fopen("rules.txt", "w");
    fwrite(rules, 1, strlen(rules), file);
    fclose(file);

    TestConnections::skip_maxscale_start(true);
    TestConnections test(argc, argv);

    copy_rules(&test, (char*) "rules.txt", (char*) ".");

    test.maxscales->restart_maxscale();
    test.maxscales->connect_maxscale();
    test.tprintf("Pinging MaxScale, expecting success");
    test.add_result(mysql_ping(test.maxscales->conn_rwsplit[0]),
                    "Ping should not fail: %s",
                    mysql_error(test.maxscales->conn_rwsplit[0]));
    test.maxscales->close_maxscale_connections();

    return test.global_result;
}

/**
 * @file mariadb_nodes.cpp - backend nodes routines
 *
 * @verbatim
 * Revision History
 *
 * Date     Who     Description
 * 17/11/14 Timofey Turenko Initial implementation
 *
 * @endverbatim
 */

#include <maxtest/mariadb_nodes.hh>
#include <climits>
#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <future>
#include <functional>
#include <algorithm>
#include <maxbase/format.hh>
#include <maxtest/envv.hh>
#include <maxtest/log.hh>
#include <maxtest/mariadb_connector.hh>
#include <maxtest/test_dir.hh>
#include <cassert>

using std::cout;
using std::endl;
using std::string;
using std::move;

namespace
{
// These need to match the values in create_user.sh.
const string admin_user = "test-admin";
const string admin_pw = "test-admin-pw";
}

/**
 * Tries to find MariaDB server version number in the output of 'mysqld --version'
 *
 * @param version String returned by 'mysqld --version'
 * @return String with version number
 */
string MariaDBCluster::extract_version_from_string(const string& version)
{
    int pos1 = 0;
    int pos2 = 0;
    int l = version.length();
    while ((!isdigit(version[pos1])) && (pos1 < l))
    {
        pos1++;
    }
    pos2 = pos1;
    while (((isdigit(version[pos2]) || version[pos2] == '.')) && (pos2 < l))
    {
        pos2++;
    }
    return version.substr(pos1, pos2 - pos1);
}

MariaDBCluster::MariaDBCluster(mxt::SharedData* shared, const std::string& cnf_server_prefix)
    : Nodes(shared)
    , m_cnf_server_prefix(cnf_server_prefix)
{
    m_test_dir = test_dir;
}

bool MariaDBCluster::setup(const mxt::NetworkConfig& nwconfig, int n_min_expected)
{
    bool rval = true;
    m_n_req_backends = n_min_expected;
    int found = read_nodes_info(nwconfig);
    if (found < n_min_expected)
    {
        logger().add_failure("Found %i node(s) in network_config when at least %i was expected.",
                             found, n_min_expected);
        rval = false;
    }
    return rval;
}

MariaDBCluster::~MariaDBCluster()
{
    for (int i = 0; i < N; i++)
    {
        if (m_blocked[i])
        {
            unblock_node(i);
        }
    }

    close_connections();
}

int MariaDBCluster::connect(int i, const std::string& db)
{
    if (nodes[i] == NULL || mysql_ping(nodes[i]) != 0)
    {
        if (nodes[i])
        {
            mysql_close(nodes[i]);
        }
        nodes[i] = open_conn_db_timeout(port[i], ip4(i), db.c_str(), user_name, password, 50, m_ssl);
    }

    if ((nodes[i] == NULL) || (mysql_errno(nodes[i]) != 0))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

int MariaDBCluster::connect(const std::string& db)
{
    int res = 0;

    for (int i = 0; i < N; i++)
    {
        res += connect(i, db);
    }

    return res;
}

bool MariaDBCluster::robust_connect(int n)
{
    bool rval = false;

    for (int i = 0; i < n; i++)
    {
        if (connect() == 0)
        {
            // Connected successfully, return immediately
            rval = true;
            break;
        }

        // We failed to connect, disconnect and wait for a second before trying again
        disconnect();
        sleep(1);
    }

    return rval;
}

void MariaDBCluster::close_connections()
{
    for (int i = 0; i < N; i++)
    {
        if (nodes[i] != NULL)
        {
            mysql_close(nodes[i]);
            nodes[i] = NULL;
        }
    }
}

int MariaDBCluster::read_nodes_info(const mxt::NetworkConfig& nwconfig)
{
    auto prefixc = nwconf_prefix().c_str();

    string key_user = mxb::string_printf("%s_user", prefixc);
    user_name = envvar_get_set(key_user.c_str(), "skysql");

    string key_pw = mxb::string_printf("%s_password", prefixc);
    password = envvar_get_set(key_pw.c_str(), "skysql");

    string key_ssl = mxb::string_printf("%s_ssl", prefixc);
    setenv(key_ssl.c_str(), m_ssl ? "true" : "false", 1);

    const string space = " ";
    const char start_db_def[] = "systemctl start mariadb || service mysql start";
    const char stop_db_def[] = "systemctl stop mariadb || service mysql stop";
    const char clean_db_def[] = "rm -rf /var/lib/mysql/*; killall -9 mysqld";

    clear_vms();
    m_backends.clear();

    int i = 0;
    while (i < N_MAX)
    {
        string node_name = mxb::string_printf("%s_%03d", prefixc, i);
        if (add_node(nwconfig, node_name))
        {
            string cnf_name = m_cnf_server_prefix + std::to_string(i + 1);
            auto srv = std::make_unique<mxt::MariaDBServer>(cnf_name, *node(i), *this, i);
            string key_port = node_name + "_port";
            port[i] = readenv_int(key_port.c_str(), 3306);

            string key_socket = node_name + "_socket";
            string val_socket = envvar_get_set(key_socket.c_str(), "%s", space.c_str());
            m_socket_cmd[i] = (val_socket != space) ? ("--socket=" + val_socket) : space;

            string key_socket_cmd = node_name + "_socket_cmd";
            setenv(key_socket_cmd.c_str(), m_socket_cmd[i].c_str(), 1);

            string key_start_db_cmd = node_name + "_start_db_command";
            srv->m_settings.start_db_cmd = envvar_get_set(key_start_db_cmd.c_str(), start_db_def);

            string key_stop_db_cmd = node_name + "_stop_db_command";
            srv->m_settings.stop_db_cmd = envvar_get_set(key_stop_db_cmd.c_str(), stop_db_def);

            string key_clear_db_cmd = node_name + "_cleanup_db_command";
            srv->m_settings.cleanup_db_cmd = envvar_get_set(key_clear_db_cmd.c_str(), clean_db_def);
            m_backends.push_back(move(srv));
            i++;
        }
        else
        {
            break;
        }
    }


    assert(i == Nodes::n_nodes());
    N = i;
    return i;
}

void MariaDBCluster::print_env()
{
    auto namec = name().c_str();
    for (int i = 0; i < N; i++)
    {
        printf("%s node %d \t%s\tPort=%d\n", namec, i, ip4(i), port[i]);
        printf("%s Access user %s\n", namec, access_user(i));
    }
    printf("%s User name %s\n", namec, user_name.c_str());
    printf("%s Password %s\n", namec, password.c_str());
}

int MariaDBCluster::stop_node(int node)
{
    return ssh_node(node, m_backends[node]->m_settings.stop_db_cmd, true);
}

int MariaDBCluster::start_node(int node, const char* param)
{
    string cmd = mxb::string_printf("%s %s", m_backends[node]->m_settings.start_db_cmd.c_str(), param);
    return ssh_node(node, cmd, true);
}

bool MariaDBCluster::stop_nodes()
{
    auto func = [this](int i) {
            return stop_node(i) == 0;
        };
    return run_on_every_backend(func);
}

bool MariaDBCluster::start_nodes()
{
    auto func = [this](int i) {
            return m_backends[i]->start_database();
        };
    return run_on_every_backend(func);
}

int MariaDBCluster::stop_slaves()
{
    int i;
    int global_result = 0;
    connect();
    for (i = 0; i < N; i++)
    {
        printf("Stopping slave %d\n", i);
        fflush(stdout);
        global_result += execute_query(nodes[i], (char*) "stop slave;");
    }
    close_connections();
    return global_result;
}

void MariaDBCluster::create_users(int node)
{
    // Create users for replication as well as the users that are used by the tests
    string str = mxb::string_printf("%s/create_user.sh", m_test_dir.c_str());
    copy_to_node(node, str.c_str(), access_homedir(node));

    ssh_node_f(node, true,
               "export require_ssl=\"%s\"; "
               "export node_user=\"%s\"; "
               "export node_password=\"%s\"; "
               "%s/create_user.sh \"%s\" %s",
               m_ssl ? "REQUIRE SSL" : "",
               user_name.c_str(),
               password.c_str(),
               access_homedir(0),
               m_socket_cmd[0].c_str(),
               type_string().c_str());
}

int MariaDBCluster::clean_iptables(int node)
{
    return ssh_node_f(node,
                      true,
                      "while [ \"$(iptables -n -L INPUT 1|grep '%d')\" != \"\" ]; do iptables -D INPUT 1; done;"
                      "while [ \"$(ip6tables -n -L INPUT 1|grep '%d')\" != \"\" ]; do ip6tables -D INPUT 1; done;"
                      "while [ \"$(iptables -n -L OUTPUT 1|grep '3306')\" != \"\" ]; do iptables -D OUTPUT 1; done;",
                      port[node],
                      port[node]);
}


void MariaDBCluster::block_node_from_node(int src, int dest)
{
    std::ostringstream ss;

    ss << "iptables -I OUTPUT 1 -p tcp -d " << ip4(dest) << " --dport 3306 -j DROP;";

    ssh_node_f(src, true, "%s", ss.str().c_str());
}

void MariaDBCluster::unblock_node_from_node(int src, int dest)
{
    std::ostringstream ss;

    ss << "iptables -D OUTPUT -p tcp -d " << ip4(dest) << " --dport 3306 -j DROP;";

    ssh_node_f(src, true, "%s", ss.str().c_str());
}

std::string MariaDBCluster::block_command(int node) const
{
    const char FORMAT[] =
        "iptables -I INPUT -p tcp --dport %d -j REJECT;"
        "ip6tables -I INPUT -p tcp --dport %d -j REJECT";

    char command[sizeof(FORMAT) + 20];

    sprintf(command, FORMAT, port[node], port[node]);

    return command;
}

std::string MariaDBCluster::unblock_command(int node) const
{
    const char FORMAT[] =
        "iptables -I INPUT -p tcp --dport %d -j ACCEPT;"
        "ip6tables -I INPUT -p tcp --dport %d -j ACCEPT";

    char command[sizeof(FORMAT) + 20];

    sprintf(command, FORMAT, port[node], port[node]);

    return command;
}

bool MariaDBCluster::block_node(int node)
{
    std::string command = block_command(node);
    int res = ssh_node_f(node, true, "%s", command.c_str());
    m_blocked[node] = true;
    return res == 0;
}

bool MariaDBCluster::unblock_node(int node)
{
    string command = unblock_command(node);
    int res = clean_iptables(node);
    res += ssh_node_f(node, true, "%s", command.c_str());
    m_blocked[node] = false;
    return res == 0;
}

int MariaDBCluster::block_all_nodes()
{
    auto func = [this](int i) {
            return block_node(i);
        };
    return run_on_every_backend(func);
}


bool MariaDBCluster::unblock_all_nodes()
{
    auto func = [this](int i) {
            return unblock_node(i);
        };
    return run_on_every_backend(func);
}

bool MariaDBCluster::fix_replication()
{
    auto namec = name().c_str();
    auto& log = logger();

    // First, check that all backends can be connected to. If not, try to start any failed ones.
    bool dbs_running = false;
    if (update_status())
    {
        dbs_running = true;
    }
    else
    {
        log.log_msgf("Some servers of %s could not be queried. Trying to restart and reconnect.",
                     namec);
        start_nodes();
        sleep(1);
        if (update_status())
        {
            dbs_running = true;
            log.log_msgf("Reconnection to %s worked.", namec);
        }
        else
        {
            log.log_msgf("Reconnection to %s failed.", namec);
        }
    }

    // close_active_connections(); TODO: readd
    bool need_fixing = true;
    if (dbs_running)
    {
        if (check_replication() && prepare_servers_for_test())
        {
            need_fixing = false;
        }
    }

    bool rval = false;
    if (need_fixing)
    {
        log.log_msgf("%s is broken, fixing ...", namec);

        if (unblock_all_nodes())
        {
            log.log_msgf("Firewalls on %s open.", namec);
            if (reset_servers())
            {
                log.log_msgf("%s reset. Starting replication.", namec);
                start_replication();

                int attempts = 0;
                bool cluster_ok = false;

                while (!cluster_ok && attempts < 10)
                {
                    if (attempts > 0)
                    {
                        log.log_msgf("Iteration %i, %s is still broken, waiting.", attempts, namec);
                        sleep(10);
                    }
                    if (check_replication())
                    {
                        cluster_ok = true;
                    }
                    attempts++;
                }

                if (cluster_ok)
                {
                    log.log_msgf("%s is replicating/synced.", namec);
                    rval = prepare_servers_for_test();
                }
                else
                {
                    log.add_failure("%s is still broken.", namec);
                }
            }
            else
            {
                logger().add_failure("Server preparation on %s failed.", name().c_str());
            }
        }
        else
        {
            logger().add_failure("Failed to unblock %s.", name().c_str());
        }
    }
    else
    {
        rval = true;
    }

    disconnect();
    return rval;
}

int MariaDBCluster::get_server_id(int index)
{
    int id = -1;
    char str[1024];

    if (find_field(this->nodes[index], "SELECT @@server_id", "@@server_id", (char*) str) == 0)
    {
        id = atoi(str);
    }
    else
    {
        printf("find_field failed for %s:%d\n", ip4(index), this->port[index]);
    }

    return id;
}

std::string MariaDBCluster::get_server_id_str(int index)
{
    std::stringstream ss;
    ss << get_server_id(index);
    return ss.str();
}

std::vector<std::string> MariaDBCluster::get_all_server_ids_str()
{
    std::vector<std::string> rval;

    for (int i = 0; i < N; i++)
    {
        rval.push_back(get_server_id_str(i));
    }

    return rval;
}

std::vector<int> MariaDBCluster::get_all_server_ids()
{
    std::vector<int> rval;

    for (int i = 0; i < N; i++)
    {
        rval.push_back(get_server_id(i));
    }

    return rval;
}

std::string MariaDBCluster::anonymous_users_query() const
{
    return "SELECT CONCAT('\\'', user, '\\'@\\'', host, '\\'') FROM mysql.user WHERE user = ''";
}

bool MariaDBCluster::prepare_servers_for_test()
{
    // Remove anonymous users. TODO: Extend this to detect leftover users and create any missing users.
    bool drop_ok = false;
    auto master = m_backends[0].get();      // Assume that first server is a master for all cluster types.
    if (master->ping_or_open_admin_connection())
    {
        auto conn = master->admin_connection();
        auto res = conn->query(anonymous_users_query());
        if (res)
        {
            drop_ok = true;
            int rows = res->get_row_count();
            if (rows > 0)
            {
                logger().log_msgf("Detected %i anonymous users on %s, dropping them.", rows, name().c_str());
                while (res->next_row())
                {
                    string user = res->get_string(0);
                    string query = mxb::string_printf("DROP USER %s;", user.c_str());
                    if (!conn->try_cmd(query))
                    {
                        drop_ok = false;
                    }
                }
            }
        }
    }

    bool rval = false;
    if (drop_ok)
    {
        bool normal_conn_ok = check_normal_conns();
        if (!normal_conn_ok)
        {
            // Try to regenerate users. The user generation script replaces users. As the cluster
            // is replicating, doing this on the master should be enough.
            logger().log_msgf("Recreating users on '%s' with SSL %s.",
                              m_backends[0]->m_vm.m_name.c_str(), m_ssl ? "on" : "off");
            create_users(0);
            sleep(1);   // Wait for cluster sync. Could come up with something better.
            normal_conn_ok = check_normal_conns();
            logger().log_msgf("Connections to %s %s after recreating users.",
                              name().c_str(), normal_conn_ok ? "worked" : "failed");
        }

        if (normal_conn_ok)
        {
            rval = true;
            for (int i = 0; i < N; i++)
            {
                auto srv = m_backends[i].get();
                srv->ping_or_open_admin_connection();
                auto conn = srv->admin_connection();
                if (conn->cmd("SET GLOBAL max_connections=10000"))
                {
                    conn->try_cmd("SET GLOBAL max_connect_errors=10000000");    // fails on Xpand
                }
                else
                {
                    rval = false;
                }
            }
        }
    }

    return rval;
}

int MariaDBCluster::execute_query_all_nodes(const char* sql)
{
    int local_result = 0;
    connect();
    for (int i = 0; i < N; i++)
    {
        local_result += execute_query(nodes[i], "%s", sql);
    }
    close_connections();
    return local_result;
}

void MariaDBCluster::close_active_connections()
{
    if (this->nodes[0] == NULL)
    {
        this->connect();
    }

    const char* sql
        =
            "select id from information_schema.processlist where id != @@pseudo_thread_id and user not in ('system user', 'repl')";

    for (int i = 0; i < N; i++)
    {
        if (!mysql_query(nodes[i], sql))
        {
            MYSQL_RES* res = mysql_store_result(nodes[i]);
            if (res)
            {
                MYSQL_ROW row;

                while ((row = mysql_fetch_row(res)))
                {
                    std::string q("KILL ");
                    q += row[0];
                    execute_query_silent(nodes[i], q.c_str());
                }
                mysql_free_result(res);
            }
        }
    }
}


void MariaDBCluster::stash_server_settings(int node)
{
    ssh_node(node, "sudo rm -rf /etc/my.cnf.d.backup/", true);
    ssh_node(node, "sudo mkdir /etc/my.cnf.d.backup/", true);
    ssh_node(node, "sudo cp -r /etc/my.cnf.d/* /etc/my.cnf.d.backup/", true);
}

void MariaDBCluster::restore_server_settings(int node)
{
    ssh_node(node, "sudo mv -f /etc/my.cnf.d.backup/* /etc/my.cnf.d/", true);
}

void MariaDBCluster::disable_server_setting(int node, const char* setting)
{
    ssh_node_f(node, true, "sudo sed -i 's/%s/#%s/' /etc/my.cnf.d/*", setting, setting);
}

void MariaDBCluster::add_server_setting(int node, const char* setting)
{
    ssh_node_f(node, true, "sudo sed -i '$a [server]' /etc/my.cnf.d/*server*.cnf");
    ssh_node_f(node, true, "sudo sed -i '$a %s' /etc/my.cnf.d/*server*.cnf", setting);
}

void MariaDBCluster::reset_server_settings(int node)
{
    string cnf_dir = m_test_dir + "/mdbci/cnf/";
    string cnf_file = get_srv_cnf_filename(node);
    string cnf_path = cnf_dir + cnf_file;

    // Note: This is a CentOS specific path
    ssh_node(node, "rm -rf /etc/my.cnf.d/*", true);

    copy_to_node(node, cnf_path.c_str(), "~/");
    ssh_node_f(node, false, "sudo install -o root -g root -m 0644 ~/%s /etc/my.cnf.d/", cnf_file.c_str());

    // Always configure the backend for SSL
    std::string ssl_dir = m_test_dir + "/ssl-cert";
    std::string ssl_cnf = m_test_dir + "/ssl.cnf";
    copy_to_node_legacy(ssl_dir.c_str(), "~/", node);
    copy_to_node_legacy(ssl_cnf.c_str(), "~/", node);

    ssh_node_f(node, true, "cp %s/ssl.cnf /etc/my.cnf.d/", access_homedir(node));
    ssh_node_f(node, true, "cp -r %s/ssl-cert /etc/", access_homedir(node));
    ssh_node_f(node, true, "chown mysql:mysql -R /etc/ssl-cert");
}

void MariaDBCluster::reset_all_servers_settings()
{
    for (int node = 0; node < N; node++)
    {
        reset_server_settings(node);
    }
}

bool MariaDBCluster::reset_server(int i)
{
    auto& srv = m_backends[i];
    srv->stop_database();
    srv->cleanup_database();
    reset_server_settings(i);

    auto& vm = srv->vm_node();
    auto namec = vm.m_name.c_str();

    // Note: These should be done by MDBCI
    vm.run_cmd_sudo("test -d /etc/apparmor.d/ && "
                    "ln -s /etc/apparmor.d/usr.sbin.mysqld /etc/apparmor.d/disable/usr.sbin.mysqld && "
                    "sudo service apparmor restart && "
                    "chmod a+r -R /etc/my.cnf.d/*");

    bool reset_ok = false;
    const char vrs_cmd[] = "/usr/sbin/mysqld --version";
    auto res_version = vm.run_cmd_output(vrs_cmd);

    if (res_version.rc == 0)
    {
        string version_digits = extract_version_from_string(res_version.output);
        if (version_digits.compare(0, 3, "10.") == 0)
        {
            const char reset_db_cmd[] = "mysql_install_db; sudo chown -R mysql:mysql /var/lib/mysql";
            logger().log_msgf("Running '%s' on '%s'", reset_db_cmd, namec);
            if (vm.run_cmd_sudo(reset_db_cmd) == 0)
            {
                reset_ok = true;
            }
            else
            {
                logger().add_failure("'%s' failed on '%s'.", reset_db_cmd, namec);
            }
        }
        else
        {
            logger().add_failure("'%s' on '%s' returned '%s'. Detected server version '%s' is not "
                                 "supported by the test system.",
                                 vrs_cmd, vm.m_name.c_str(), res_version.output.c_str(),
                                 version_digits.c_str());
        }
    }
    else
    {
        logger().add_failure("'%s' failed on '%s'.", vrs_cmd, vm.m_name.c_str());
    }

    bool started = srv->start_database();
    if (!started)
    {
        logger().add_failure("Database process start failed on '%s' after reset.", namec);
    }
    return reset_ok && started;
}

bool MariaDBCluster::reset_servers()
{
    auto func = [this](int i) {
            return reset_server(i);
        };
    return run_on_every_backend(func);
}

std::string MariaDBCluster::cnf_servers()
{
    string rval;
    rval.reserve(100 * N);
    bool use_ip6 = using_ipv6();
    for (int i = 0; i < N; i++)
    {
        auto& name = m_backends[i]->cnf_name();
        string one_server = mxb::string_printf("[%s]\n"
                                               "type=server\n"
                                               "address=%s\n"
                                               "port=%i\n\n",
                                               name.c_str(),
                                               use_ip6 ? ip6(i) : ip_private(i),
                                               port[i]);
        rval += one_server;
    }
    return rval;
}

std::string MariaDBCluster::cnf_servers_line()
{
    string rval;
    string sep;
    for (int i = 0; i < N; i++)
    {
        rval.append(sep).append(m_backends[i]->cnf_name());
        sep = ",";
    }
    return rval;
}

const char* MariaDBCluster::ip(int i) const
{
    return m_use_ipv6 ? Nodes::ip6(i) : Nodes::ip4(i);
}

void MariaDBCluster::set_use_ipv6(bool use_ipv6)
{
    m_use_ipv6 = use_ipv6;
}

const char* MariaDBCluster::ip_private(int i) const
{
    return Nodes::ip_private(i);
}

const char* MariaDBCluster::ip6(int i) const
{
    return Nodes::ip6(i);
}

const char* MariaDBCluster::access_homedir(int i) const
{
    return Nodes::access_homedir(i);
}

const char* MariaDBCluster::access_sudo(int i) const
{
    return Nodes::access_sudo(i);
}

const char* MariaDBCluster::ip4(int i) const
{
    return Nodes::ip4(i);
}

void MariaDBCluster::disable_ssl()
{
    for (int i = 0; i < N; i++)
    {
        stop_node(i);
        ssh_node(i, "rm -f /etc/my.cnf.d/ssl.cnf", true);
        start_node(i);
    }
}

bool MariaDBCluster::using_ipv6() const
{
    return m_use_ipv6;
}

const std::string& MariaDBCluster::cnf_server_prefix() const
{
    return m_cnf_server_prefix;
}

bool MariaDBCluster::update_status()
{
    bool rval = true;
    for (auto& srv : m_backends)
    {
        if (!srv->update_status())
        {
            rval = false;
        }
    }
    return rval;
}

bool MariaDBCluster::check_backend_versions(uint64_t min_vrs)
{
    bool rval = false;
    if (update_status())
    {
        bool version_ok = true;
        for (auto& srv : m_backends)
        {
            if (srv->m_status.version_num < min_vrs)
            {
                // Old backend is classified as test skip, not a failed test.
                logger().log_msgf("Server version on '%s' is %lu when at least %lu is required.",
                                  srv->m_vm.m_name.c_str(), srv->m_status.version_num, min_vrs);
                version_ok = false;
            }
        }
        rval = version_ok;
    }
    else
    {
        logger().add_failure("Failed to update servers of %s.", name().c_str());
    }
    return rval;
}

mxt::TestLogger& MariaDBCluster::logger()
{
    return m_shared.log;
}

mxt::MariaDBServer* MariaDBCluster::backend(int i)
{
    return m_backends[i].get();
}

bool MariaDBCluster::check_create_test_db()
{
    bool rval = false;
    if (!m_backends.empty())
    {
        auto srv = m_backends[0].get();
        if (srv->ping_or_open_admin_connection())
        {
            auto conn = srv->admin_connection();
            if (conn->cmd("DROP DATABASE IF EXISTS test;") && conn->cmd("CREATE DATABASE test;"))
            {
                rval = true;
            }
        }
    }
    return rval;
}

bool MariaDBCluster::basic_test_prepare()
{
    auto prepare_one = [this](int i) {
            auto srv = m_backends[i].get();
            bool rval = false;
            auto& vm = srv->m_vm;
            if (vm.init_ssh_master())
            {
                rval = true;
                if (vm.is_remote())
                {
                    const char truncate_cmd[] = "truncate -s 0 /var/lib/mysql/*.err;"
                                                "truncate -s 0 /var/log/syslog;"
                                                "truncate -s 0 /var/log/messages;"
                                                "rm -f /etc/my.cnf.d/binlog_enc*;";
                    auto ret = vm.run_cmd_sudo(truncate_cmd);
                    if (ret != 0)
                    {
                        // Should this be a fatal error? Maybe some of the files don't exist.
                        logger().log_msgf("Log truncation failed. '%s' returned %i.", truncate_cmd, ret);
                    }
                }
            }
            return rval;
        };
    return run_on_every_backend(prepare_one);
}

int MariaDBCluster::ping_or_open_admin_connections()
{
    std::atomic_int rval {0};

    auto add_connection = [this, &rval](int i) {
            bool success = m_backends[i]->ping_or_open_admin_connection();
            if (success)
            {
                rval++;
            }
            return true;
        };
    run_on_every_backend(add_connection);
    return rval;
}

bool MariaDBCluster::run_on_every_backend(const std::function<bool(int)>& func)
{
    mxt::BoolFuncArray funcs;
    funcs.reserve(N);

    for (int i = 0; i < N; i++)
    {
        auto wrapper_func = [&func, i]() {
                return func(i);
            };
        funcs.push_back(std::move(wrapper_func));
    }
    return m_shared.concurrent_run(funcs);
}

bool MariaDBCluster::check_normal_conns()
{
    using SslMode = mxt::MariaDBServer::SslMode;

    // Check that normal connections to backends work. If ssl-mode is on, the connector refuses non-ssl
    // connections.
    bool rval = true;
    for (int i = 0; i < N; i++)
    {
        auto srv = backend(i);
        if (m_ssl)
        {
            auto conn = srv->try_open_connection(SslMode::ON);
            if (!conn->is_open())
            {
                logger().log_msgf("Connecting to '%s' as '%s' with SSL failed when SSL should be enabled.",
                                  srv->m_vm.m_name.c_str(), user_name.c_str());
                rval = false;
            }

            // Normal connections without ssl should not work.
            conn = srv->try_open_connection(SslMode::OFF);
            if (conn->is_open())
            {
                logger().log_msgf("Connecting to '%s' as '%s' without SSL succeeded when "
                                  "SSL should be required.",
                                  srv->m_vm.m_name.c_str(), user_name.c_str());
                rval = false;
            }
        }
        else
        {
            auto conn = srv->try_open_connection(SslMode::OFF);
            if (!conn->is_open())
            {
                logger().log_msgf("Connecting to '%s' as '%s' without SSL failed when SSL should not "
                                  "be required.",
                                  srv->m_vm.m_name.c_str(), user_name.c_str());
                rval = false;
            }
            // SSL-connections would likely work as well, as server is always configured for it. No need to
            // test it, though.
        }
    }
    return rval;
}

bool MariaDBCluster::ssl() const
{
    return m_ssl;
}

void MariaDBCluster::set_use_ssl(bool use_ssl)
{
    m_ssl = use_ssl;
}

void MariaDBCluster::remove_extra_backends()
{
    if (m_backends.size() > (size_t)m_n_req_backends)
    {
        for (size_t i = m_n_req_backends; i < m_backends.size(); i++)
        {
            auto srv = m_backends[i].get();
            if (srv->ping_or_open_admin_connection())
            {
                logger().log_msgf("Shutting down MariaDB Server running on '%s', "
                                  "as it's not required by test.",
                                  srv->m_vm.m_name.c_str());
                srv->stop_database();
            }
        }

        m_backends.erase(m_backends.begin() + m_n_req_backends, m_backends.end());
        N = m_backends.size();
    }
}

namespace maxtest
{
maxtest::MariaDBServer::MariaDBServer(const string& cnf_name, VMNode& vm, MariaDBCluster& cluster,
                                      int ind)
    : m_cnf_name(cnf_name)
    , m_vm(vm)
    , m_cluster(cluster)
    , m_ind(ind)
{
}

bool MariaDBServer::start_database()
{
    return m_vm.run_cmd_sudo(m_settings.start_db_cmd) == 0;
}

bool MariaDBServer::stop_database()
{
    return m_vm.run_cmd_sudo(m_settings.stop_db_cmd) == 0;
}

bool MariaDBServer::cleanup_database()
{
    return m_vm.run_cmd_sudo(m_settings.cleanup_db_cmd) == 0;
}

const MariaDBServer::Status& MariaDBServer::status() const
{
    return m_status;
}

bool MariaDBServer::update_status()
{
    bool rval = false;
    if (ping_or_open_admin_connection())
    {
        m_status.version_num = m_admin_conn->version_info().version;
        auto res = m_admin_conn->query("SELECT @@server_id, @@read_only;");
        if (res && res->next_row())
        {
            m_status.server_id = res->get_int(0);
            m_status.read_only = res->get_bool(1);
            if (!res->error())
            {
                rval = true;
            }
        }
    }
    return rval;
}

MariaDBServer::SMariaDB MariaDBServer::try_open_connection(SslMode ssl)
{
    auto conn = std::make_unique<mxt::MariaDB>(m_vm.log());
    auto& sett = conn->connection_settings();
    sett.user = m_cluster.user_name;
    sett.password = m_cluster.password;
    if (ssl == SslMode::ON)
    {
        sett.ssl.key = mxb::string_printf("%s/ssl-cert/client-key.pem", test_dir);
        sett.ssl.cert = mxb::string_printf("%s/ssl-cert/client-cert.pem", test_dir);
        sett.ssl.ca = mxb::string_printf("%s/ssl-cert/ca.pem", test_dir);
    }
    sett.timeout = 10;
    auto& ip = m_cluster.using_ipv6() ? m_vm.ip6s() : m_vm.ip4s();
    conn->try_open(ip, port());
    return conn;
}

MariaDBServer::SMariaDB MariaDBServer::try_open_connection()
{
    return try_open_connection(m_cluster.ssl() ? SslMode::ON : SslMode::OFF);
}

bool MariaDBServer::ping_or_open_admin_connection()
{
    bool rval = false;
    if (m_admin_conn && m_admin_conn->is_open() && m_admin_conn->ping())
    {
        // Connection already exists and works.
        rval = true;
    }
    else
    {
        auto conn = std::make_unique<mxt::MariaDB>(m_vm.log());
        auto& sett = conn->connection_settings();
        sett.user = admin_user;
        sett.password = admin_pw;
        sett.clear_sql_mode = true;
        sett.timeout = 10;
        conn->try_open(m_vm.ip4s(), port());

        m_admin_conn = move(conn);      // Saved even if not open, so that m_admin_conn is not left null.
        if (m_admin_conn->is_open())
        {
            rval = true;
        }
    }
    return rval;
}

std::string MariaDBServer::version_as_string()
{
    auto v = m_status.version_num;
    uint32_t major = v / 10000;
    uint32_t minor = (v - major * 10000) / 100;
    uint32_t patch = v - major * 10000 - minor * 100;
    return mxb::string_printf("%i.%i.%i", major, minor, patch);
}

const string& MariaDBServer::cnf_name() const
{
    return m_cnf_name;
}

VMNode& MariaDBServer::vm_node()
{
    return m_vm;
}

int MariaDBServer::port()
{
    return m_cluster.port[m_ind];
}

mxt::MariaDB* MariaDBServer::admin_connection()
{
    // Can assume that the connection has been created.
    return m_admin_conn.get();
}
}

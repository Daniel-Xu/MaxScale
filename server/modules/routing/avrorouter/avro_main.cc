/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2025-04-28
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "avrorouter.hh"

#include <ctype.h>
#include <ini.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <glob.h>
#include <ini.h>
#include <sys/stat.h>
#include <maxbase/atomic.h>
#include <maxscale/cn_strings.hh>
#include <maxscale/maxscale.h>
#include <maxbase/worker.hh>
#include <maxbase/alloc.h>
#include <maxscale/dcb.hh>
#include <maxscale/modulecmd.hh>
#include <maxscale/paths.hh>
#include <maxscale/protocol/mariadb/mysql.hh>
#include <maxscale/random.h>
#include <maxscale/router.hh>
#include <maxscale/server.hh>
#include <maxscale/service.hh>
#include <maxscale/utils.h>
#include <maxscale/routingworker.hh>

#include "avro_converter.hh"

using namespace maxbase;

static bool conversion_task_ctl(Avro* inst, bool start);

/**
 * Create an instance of the router for a particular service
 * within MaxScale.
 *
 * The process of creating the instance causes the router to register
 * with the master server and begin replication of the binlogs from
 * the master server to MaxScale.
 *
 * @param service   The service this router is being create for
 * @param options   An array of options for this query router
 *
 * @return The instance data for this new instance
 */
MXS_ROUTER* createInstance(SERVICE* service, mxs::ConfigParameters* params)
{
    Avro* router = Avro::create(service);

    if (router && !params->contains(CN_SERVERS) && !params->contains(CN_CLUSTER))
    {
        conversion_task_ctl(router, true);
    }

    return router;
}

void destroyInstance(MXS_ROUTER* router)
{
    delete static_cast<Avro*>(router);
}

/**
 * Associate a new session with this instance of the router.
 *
 * In the case of the avrorouter a new session equates to a new slave
 * connecting to MaxScale and requesting binlog records. We need to go
 * through the slave registration process for this new slave.
 *
 * @param instance  The router instance data
 * @param session   The session itself
 * @return Session specific data for this session
 */
static MXS_ROUTER_SESSION* newSession(MXS_ROUTER* instance, MXS_SESSION* session,
                                      mxs::Upstream* up, const Endpoints& endpoints)
{
    Avro* inst = reinterpret_cast<Avro*>(instance);
    return AvroSession::create(inst, session);
}

/**
 * The session is no longer required. Shutdown all operation and free memory
 * associated with this session. In this case a single session is associated
 * to a slave of MaxScale. Therefore this is called when that slave is no
 * longer active and should remove of reference to that slave, free memory
 * and prevent any further forwarding of binlog records to that slave.
 *
 * Parameters:
 * @param router_instance   The instance of the router
 * @param router_cli_ses    The particular session to free
 *
 */
static void freeSession(MXS_ROUTER* router_instance, MXS_ROUTER_SESSION* router_client_ses)
{
    AvroSession* client = (AvroSession*) router_client_ses;
    delete client;
}

/**
 * Close a session with the router, this is the mechanism
 * by which a router may cleanup data structure etc.
 *
 * @param instance          The router instance data
 * @param router_session    The session being closed
 */
static void closeSession(MXS_ROUTER* instance, MXS_ROUTER_SESSION* router_session)
{
}

/**
 * We have data from the client, this is likely to be packets related to
 * the registration of the slave to receive binlog records. Unlike most
 * MaxScale routers there is no forwarding to the backend database, merely
 * the return of either predefined server responses that have been cached
 * or binlog records.
 *
 * @param instance      The router instance
 * @param router_session    The router session returned from the newSession call
 * @param queue         The queue of data buffers to route
 * @return 1 on success, 0 on error
 */
static int routeQuery(MXS_ROUTER* instance, MXS_ROUTER_SESSION* router_session, GWBUF* queue)
{
    AvroSession* client = (AvroSession*) router_session;

    return client->routeQuery(queue);
}

/**
 * Display router diagnostics
 *
 * @param instance  Instance of the router
 */
static json_t* diagnostics(const MXS_ROUTER* router)
{
    Avro* router_inst = (Avro*)router;

    json_t* rval = json_object();

    char pathbuf[PATH_MAX + 1];
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", router_inst->avrodir.c_str(), AVRO_PROGRESS_FILE);

    json_object_set_new(rval, "infofile", json_string(pathbuf));
    json_object_set_new(rval, "avrodir", json_string(router_inst->avrodir.c_str()));
    json_object_set_new(rval, "binlogdir", json_string(router_inst->binlogdir.c_str()));
    json_object_set_new(rval, "binlog_name", json_string(router_inst->binlog_name.c_str()));
    json_object_set_new(rval, "binlog_pos", json_integer(router_inst->current_pos));

    if (router_inst->handler)
    {
        gtid_pos_t gtid = router_inst->handler->get_gtid();
        snprintf(pathbuf, sizeof(pathbuf), "%lu-%lu-%lu", gtid.domain, gtid.server_id, gtid.seq);
        json_object_set_new(rval, "gtid", json_string(pathbuf));
        json_object_set_new(rval, "gtid_timestamp", json_integer(gtid.timestamp));
        json_object_set_new(rval, "gtid_event_number", json_integer(gtid.event_num));
    }

    return rval;
}

/**
 * Client Reply routine - in this case this is a message from the
 * master server, It should be sent to the state machine that manages
 * master packets as it may be binlog records or part of the registration
 * handshake that takes part during connection establishment.
 *
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       master_dcb      The DCB for the connection to the master
 * @param       queue           The GWBUF with reply data
 */
static void clientReply(MXS_ROUTER* instance,
                        MXS_ROUTER_SESSION* router_session,
                        GWBUF* queue,
                        const mxs::ReplyRoute& backend_dcb,
                        const mxs::Reply& reply)
{
    /** We should never end up here */
    mxb_assert(false);
}

/**
 * Error Reply routine
 *
 * The routine will reply to client errors and/or closing the session
 * or try to open a new backend connection.
 *
 * @param       instance        The router instance
 * @param       router_session  The router session
 * @param       message         The error message to reply
 * @param       backend_dcb     The backend DCB
 *
 */
static bool errorReply(MXS_ROUTER* instance,
                       MXS_ROUTER_SESSION* router_session,
                       mxs::ErrorType type,
                       GWBUF* message,
                       mxs::Endpoint* backend_dcb,
                       const mxs::Reply& reply)
{
    /** We should never end up here */
    mxb_assert(false);
    return false;
}

static uint64_t getCapabilities(MXS_ROUTER* instance)
{
    return RCAP_TYPE_NONE;
}

/**
 * Conversion task: MySQL binlogs to AVRO files
 */
bool converter_func(Worker::Call::action_t action, Avro* router)
{
    if (action == Worker::Call::CANCEL)
    {
        return false;
    }

    bool progress = false;
    avro_binlog_end_t binlog_end = AVRO_BINLOG_ERROR;

    uint64_t start_pos = router->current_pos;
    std::string binlog_name = router->binlog_name;

    if (avro_open_binlog(router->binlogdir.c_str(), router->binlog_name.c_str(), &router->binlog_fd))
    {
        binlog_end = avro_read_all_events(router);

        if (router->current_pos != start_pos || binlog_name != router->binlog_name)
        {
            progress = true;
        }

        close(router->binlog_fd);
    }

    static int logged = true;

    /** We reached end of file, flush unwritten records to disk */
    if (progress)
    {
        router->handler->flush();
        avro_save_conversion_state(router);
        logged = false;
    }

    if (binlog_end == AVRO_LAST_FILE && !logged)
    {
        logged = true;
        MXS_INFO("Stopped processing file %s at position %lu. Waiting until"
                 " more data is written before continuing.",
                 router->binlog_name.c_str(),
                 router->current_pos);
    }

    return true;
}

class ConversionCtlTask : public Worker::DisposableTask
{
public:
    ConversionCtlTask(Avro* instance, bool start)
        : m_instance(instance)
        , m_start(start)
    {
    }

    void execute(Worker& worker)
    {
        if (m_instance->task_handle)
        {
            worker.cancel_delayed_call(m_instance->task_handle);
            m_instance->task_handle = 0;
        }

        if (m_start)
        {
            m_instance->task_handle = worker.delayed_call(1000, converter_func, m_instance);
        }
    }

private:
    Avro* m_instance;
    bool  m_start;
};

static bool conversion_task_ctl(Avro* inst, bool start)
{
    bool rval = false;

    if (!maxscale_is_shutting_down())
    {
        Worker* worker = static_cast<Worker*>(mxs_rworker_get(MXS_RWORKER_MAIN));
        std::unique_ptr<ConversionCtlTask> task(new(std::nothrow) ConversionCtlTask(inst, start));

        if (task.get())
        {
            worker->execute(std::move(task), Worker::EXECUTE_AUTO);
            rval = true;
        }
    }

    return rval;
}

bool avro_handle_convert(const MODULECMD_ARG* args, json_t** output)
{
    bool rval = false;

    if (strcmp(args->argv[1].value.string, "start") == 0
        && conversion_task_ctl((Avro*)args->argv[0].value.service->router_instance, true))
    {
        MXS_NOTICE("Started conversion for service '%s'.", args->argv[0].value.service->name());
        rval = true;
    }
    else if (strcmp(args->argv[1].value.string, "stop") == 0
             && conversion_task_ctl((Avro*)args->argv[0].value.service->router_instance, false))
    {
        MXS_NOTICE("Stopped conversion for service '%s'.", args->argv[0].value.service->name());
        rval = true;
    }

    return rval;
}


static bool do_unlink(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    char filename[PATH_MAX + 1];
    vsnprintf(filename, sizeof(filename), format, args);

    va_end(args);

    int rc = unlink(filename);
    return rc == 0 || rc == ENOENT;
}

static bool do_unlink_with_pattern(const char* format, ...)
{
    bool rval = true;
    va_list args;
    va_start(args, format);

    char filename[PATH_MAX + 1];
    vsnprintf(filename, sizeof(filename), format, args);

    va_end(args);

    glob_t g;
    int rc = glob(filename, 0, NULL, &g);

    if (rc == 0)
    {
        for (size_t i = 0; i < g.gl_pathc; i++)
        {
            if (!do_unlink("%s", g.gl_pathv[i]))
            {
                rval = false;
            }
        }
    }
    else if (rc != GLOB_NOMATCH)
    {
        modulecmd_set_error("Failed to search '%s': %d, %s",
                            filename,
                            errno,
                            mxs_strerror(errno));
        rval = false;
    }

    globfree(&g);

    return rval;
}

static bool avro_handle_purge(const MODULECMD_ARG* args, json_t** output)
{
    Avro* inst = (Avro*)args->argv[0].value.service->router_instance;

    // First stop the conversion service
    conversion_task_ctl(inst, false);

    // Then delete the files
    return do_unlink("%s/%s", inst->avrodir.c_str(), AVRO_PROGRESS_FILE)    // State file
           && do_unlink_with_pattern("/%s/*.avro", inst->avrodir.c_str())   // .avro files
           && do_unlink_with_pattern("/%s/*.avsc", inst->avrodir.c_str());  // .avsc files
}

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static modulecmd_arg_type_t args_convert[] =
    {
        {MODULECMD_ARG_SERVICE | MODULECMD_ARG_NAME_MATCHES_DOMAIN,
         "The avrorouter service"},
        {MODULECMD_ARG_STRING,
         "Action, whether to 'start' or 'stop' the conversion process"}
    };
    modulecmd_register_command(MXS_MODULE_NAME,
                               "convert",
                               MODULECMD_TYPE_ACTIVE,
                               avro_handle_convert,
                               2,
                               args_convert,
                               "Start or stop the binlog to avro conversion process");

    static modulecmd_arg_type_t args_purge[] =
    {
        {
            MODULECMD_ARG_SERVICE | MODULECMD_ARG_NAME_MATCHES_DOMAIN,
            "The avrorouter service to purge (NOTE: THIS REMOVES ALL CONVERTED FILES)"
        }
    };
    modulecmd_register_command(MXS_MODULE_NAME,
                               "purge",
                               MODULECMD_TYPE_ACTIVE,
                               avro_handle_purge,
                               1,
                               args_purge,
                               "Purge created Avro files and reset conversion state. "
                               "NOTE: MaxScale must be restarted after this call.");

    static MXS_ROUTER_OBJECT MyObject =
    {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        routeQuery,
        diagnostics,
        clientReply,
        errorReply,
        getCapabilities,
        destroyInstance
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_ROUTER,
        MXS_MODULE_GA,
        MXS_ROUTER_VERSION,
        "Avrorouter",
        "V1.0.0",
        0,
        &MyObject,
        NULL,
        NULL,
        NULL,
        NULL,
        {
            {
                "binlogdir",
                MXS_MODULE_PARAM_PATH,
                NULL,
                MXS_MODULE_OPT_PATH_R_OK
                | MXS_MODULE_OPT_PATH_W_OK
                | MXS_MODULE_OPT_PATH_X_OK
                | MXS_MODULE_OPT_PATH_CREAT
            },
            {
                "avrodir",
                MXS_MODULE_PARAM_PATH,
                MXS_DEFAULT_DATADIR,
                MXS_MODULE_OPT_PATH_R_OK
                | MXS_MODULE_OPT_PATH_W_OK
                | MXS_MODULE_OPT_PATH_X_OK
                | MXS_MODULE_OPT_PATH_CREAT
            },
            {"filestem",              MXS_MODULE_PARAM_STRING, BINLOG_NAME_ROOT},
            {"group_rows",            MXS_MODULE_PARAM_COUNT, "1000"},
            {"group_trx",             MXS_MODULE_PARAM_COUNT, "1"},
            {"start_index",           MXS_MODULE_PARAM_COUNT, "1"},
            {"block_size",            MXS_MODULE_PARAM_SIZE, "0"},
            {"codec",                 MXS_MODULE_PARAM_ENUM, "null", MXS_MODULE_OPT_ENUM_UNIQUE, codec_values},
            {"match",                 MXS_MODULE_PARAM_REGEX  },
            {"exclude",               MXS_MODULE_PARAM_REGEX  },
            {"server_id",             MXS_MODULE_PARAM_COUNT, "1234"},
            {"gtid_start_pos",        MXS_MODULE_PARAM_STRING },
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}

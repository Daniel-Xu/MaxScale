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

/**
 * @file luafilter.c - Lua Filter
 *
 * A filter that calls a set of functions in a Lua script.
 *
 * The entry points for the Lua script expect the following signatures:
 *  * nil createInstance() - global script only
 *  * nil newSession(string, string)
 *  * nil closeSession()
 *  * (nil | bool | string) routeQuery(string)
 *  * nil clientReply()
 *  * string diagnostic() - global script only
 *
 * These functions, if found in the script, will be called whenever a call to the
 * matching entry point is made.
 *
 * The details for each entry point are documented in the functions.
 * @see createInstance, newSession, closeSession, routeQuery, clientReply, diagnostic
 *
 * The filter has two scripts, a global and a session script. If the global script
 * is defined and valid, the matching entry point function in Lua will be called.
 * The same holds true for session script apart from no calls to createInstance
 * or diagnostic being made for the session script.
 */

#define MXS_MODULE_NAME "luafilter"

#include <maxscale/ccdefs.hh>

extern "C"
{

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <string.h>
}

#include <mutex>
#include <maxbase/alloc.h>
#include <maxscale/filter.hh>
#include <maxscale/modutil.hh>
#include <maxscale/query_classifier.hh>
#include <maxscale/session.hh>

/*
 * The filter entry points
 */
static MXS_FILTER*         createInstance(const char* name, mxs::ConfigParameters*);
static MXS_FILTER_SESSION* newSession(MXS_FILTER* instance,
                                      MXS_SESSION* session,
                                      SERVICE* service,
                                      mxs::Downstream* downstream,
                                      mxs::Upstream* upstream);
static void    closeSession(MXS_FILTER* instance, MXS_FILTER_SESSION* session);
static void    freeSession(MXS_FILTER* instance, MXS_FILTER_SESSION* session);
static int32_t routeQuery(MXS_FILTER* instance, MXS_FILTER_SESSION* fsession, GWBUF* queue);
static int32_t clientReply(MXS_FILTER* instance,
                           MXS_FILTER_SESSION* fsession,
                           GWBUF* queue,
                           const mxs::ReplyRoute& down,
                           const mxs::Reply& reply);
static void     diagnostic(MXS_FILTER* instance, MXS_FILTER_SESSION* fsession, DCB* dcb);
static json_t*  diagnostics(const MXS_FILTER* instance, const MXS_FILTER_SESSION* fsession);
static uint64_t getCapabilities(MXS_FILTER* instance);

extern "C"
{

/**
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_FILTER_OBJECT MyObject =
    {
        createInstance,
        newSession,
        closeSession,
        freeSession,
        routeQuery,
        clientReply,
        diagnostics,
        getCapabilities,
        NULL,       // No destroyInstance
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_FILTER,
        MXS_MODULE_EXPERIMENTAL,
        MXS_FILTER_VERSION,
        "Lua Filter",
        "V1.0.0",
        RCAP_TYPE_CONTIGUOUS_INPUT,
        &MyObject,
        NULL,                       /* Process init. */
        NULL,                       /* Process finish. */
        NULL,                       /* Thread init. */
        NULL,                       /* Thread finish. */
        {
            {"global_script",      MXS_MODULE_PARAM_PATH,  NULL, MXS_MODULE_OPT_PATH_R_OK},
            {"session_script",     MXS_MODULE_PARAM_PATH,  NULL, MXS_MODULE_OPT_PATH_R_OK},
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}
}

static int id_pool = 0;
static GWBUF* current_global_query = NULL;

/**
 * Push an unique integer to the Lua state's stack
 * @param state Lua state
 * @return Always 1
 */
static int id_gen(lua_State* state)
{
    lua_pushinteger(state, atomic_add(&id_pool, 1));
    return 1;
}

static int lua_qc_get_type_mask(lua_State* state)
{
    int ibuf = lua_upvalueindex(1);
    GWBUF* buf = *((GWBUF**)lua_touserdata(state, ibuf));

    if (buf)
    {
        uint32_t type = qc_get_type_mask(buf);
        char* mask = qc_typemask_to_string(type);
        lua_pushstring(state, mask);
        MXS_FREE(mask);
    }
    else
    {
        lua_pushliteral(state, "");
    }

    return 1;
}

static int lua_qc_get_operation(lua_State* state)
{
    int ibuf = lua_upvalueindex(1);
    GWBUF* buf = *((GWBUF**)lua_touserdata(state, ibuf));

    if (buf)
    {
        qc_query_op_t op = qc_get_operation(buf);
        const char* opstring = qc_op_to_string(op);
        lua_pushstring(state, opstring);
    }
    else
    {
        lua_pushliteral(state, "");
    }

    return 1;
}

static int lua_get_canonical(lua_State* state)
{
    int ibuf = lua_upvalueindex(1);
    GWBUF* buf = *((GWBUF**)lua_touserdata(state, ibuf));

    if (buf)
    {
        char* canon = modutil_get_canonical(buf);
        lua_pushstring(state, canon);
        MXS_FREE(canon);
    }
    else
    {
        lua_pushliteral(state, "");
    }

    return 1;
}

/**
 * The Lua filter instance.
 */
typedef struct
{
    lua_State* global_lua_state;
    char*      global_script;
    char*      session_script;
    std::mutex lock;
} LUA_INSTANCE;

/**
 * The session structure for Lua filter.
 */
typedef struct
{
    MXS_SESSION*     session;
    lua_State*       lua_state;
    GWBUF*           current_query;
    SERVICE*         service;
    mxs::Downstream* down;
    mxs::Upstream*   up;
} LUA_SESSION;

void expose_functions(lua_State* state, GWBUF** active_buffer)
{
    /** Expose an ID generation function */
    lua_pushcfunction(state, id_gen);
    lua_setglobal(state, "id_gen");

    /** Expose a part of the query classifier API */
    lua_pushlightuserdata(state, active_buffer);
    lua_pushcclosure(state, lua_qc_get_type_mask, 1);
    lua_setglobal(state, "lua_qc_get_type_mask");

    lua_pushlightuserdata(state, active_buffer);
    lua_pushcclosure(state, lua_qc_get_operation, 1);
    lua_setglobal(state, "lua_qc_get_operation");

    lua_pushlightuserdata(state, active_buffer);
    lua_pushcclosure(state, lua_get_canonical, 1);
    lua_setglobal(state, "lua_get_canonical");
}

/**
 * Create a new instance of the Lua filter.
 *
 * The global script will be loaded in this function and executed once on a global
 * level before calling the createInstance function in the Lua script.
 * @param options The options for this filter
 * @param params  Filter parameters
 * @return The instance data for this new instance
 */
static MXS_FILTER* createInstance(const char* name, mxs::ConfigParameters* params)
{
    LUA_INSTANCE* my_instance = new(std::nothrow) LUA_INSTANCE;

    if (my_instance == NULL)
    {
        return NULL;
    }

    my_instance->global_script = params->get_c_str_copy("global_script");
    my_instance->session_script = params->get_c_str_copy("session_script");
    my_instance->global_lua_state = nullptr;

    if (my_instance->global_script)
    {
        if ((my_instance->global_lua_state = luaL_newstate()))
        {
            luaL_openlibs(my_instance->global_lua_state);

            if (luaL_dofile(my_instance->global_lua_state, my_instance->global_script))
            {
                MXS_ERROR("Failed to execute global script at '%s':%s.",
                          my_instance->global_script,
                          lua_tostring(my_instance->global_lua_state, -1));
                MXS_FREE(my_instance->global_script);
                MXS_FREE(my_instance->session_script);
                MXS_FREE(my_instance);
                my_instance = NULL;
            }
            else if (my_instance->global_lua_state)
            {
                lua_getglobal(my_instance->global_lua_state, "createInstance");

                if (lua_pcall(my_instance->global_lua_state, 0, 0, 0))
                {
                    MXS_WARNING("Failed to get global variable 'createInstance':  %s."
                                " The createInstance entry point will not be called for the global script.",
                                lua_tostring(my_instance->global_lua_state, -1));
                    lua_pop(my_instance->global_lua_state, -1);     // Pop the error off the stack
                }

                expose_functions(my_instance->global_lua_state, &current_global_query);
            }
        }
        else
        {
            MXS_ERROR("Unable to initialize new Lua state.");
            MXS_FREE(my_instance);
            my_instance = NULL;
        }
    }

    return (MXS_FILTER*) my_instance;
}

/**
 * Create a new session
 *
 * This function is called for each new client session and it is used to initialize
 * data used for the duration of the session.
 *
 * This function first loads the session script and executes in on a global level.
 * After this, the newSession function in the Lua scripts is called.
 *
 * There is a single C function exported as a global variable for the session
 * script named id_gen. The id_gen function returns an integer that is unique for
 * this service only. This function is only accessible to the session level scripts.
 * @param instance The filter instance data
 * @param session The session itself
 * @return Session specific data for this session
 */
static MXS_FILTER_SESSION* newSession(MXS_FILTER* instance,
                                      MXS_SESSION* session,
                                      SERVICE* service,
                                      mxs::Downstream* downstream,
                                      mxs::Upstream* upstream)
{
    LUA_SESSION* my_session;
    LUA_INSTANCE* my_instance = (LUA_INSTANCE*) instance;

    if ((my_session = (LUA_SESSION*) MXS_CALLOC(1, sizeof(LUA_SESSION))) == NULL)
    {
        return NULL;
    }

    my_session->service = service;
    my_session->down = downstream;
    my_session->up = upstream;
    my_session->session = session;

    if (my_instance->session_script)
    {
        my_session->lua_state = luaL_newstate();
        luaL_openlibs(my_session->lua_state);

        if (luaL_dofile(my_session->lua_state, my_instance->session_script))
        {
            MXS_ERROR("Failed to execute session script at '%s': %s.",
                      my_instance->session_script,
                      lua_tostring(my_session->lua_state, -1));
            lua_close(my_session->lua_state);
            MXS_FREE(my_session);
            my_session = NULL;
        }
        else
        {
            expose_functions(my_session->lua_state, &my_session->current_query);

            /** Call the newSession entry point */
            lua_getglobal(my_session->lua_state, "newSession");
            lua_pushstring(my_session->lua_state, session->user().c_str());
            lua_pushstring(my_session->lua_state, session->client_remote().c_str());

            if (lua_pcall(my_session->lua_state, 2, 0, 0))
            {
                MXS_WARNING("Failed to get global variable 'newSession': '%s'."
                            " The newSession entry point will not be called.",
                            lua_tostring(my_session->lua_state, -1));
                lua_pop(my_session->lua_state, -1);     // Pop the error off the stack
            }
        }
    }

    if (my_session && my_instance->global_lua_state)
    {
        std::lock_guard<std::mutex> guard(my_instance->lock);

        lua_getglobal(my_instance->global_lua_state, "newSession");
        lua_pushstring(my_instance->global_lua_state, session->user().c_str());
        lua_pushstring(my_instance->global_lua_state, session->client_remote().c_str());

        if (lua_pcall(my_instance->global_lua_state, 2, 0, 0))
        {
            MXS_WARNING("Failed to get global variable 'newSession': '%s'."
                        " The newSession entry point will not be called for the global script.",
                        lua_tostring(my_instance->global_lua_state, -1));
            lua_pop(my_instance->global_lua_state, -1);     // Pop the error off the stack
        }
    }

    return (MXS_FILTER_SESSION*)my_session;
}

/**
 * Close a session with the filter, this is the mechanism
 * by which a filter may cleanup data structure etc.
 *
 * The closeSession function in the Lua scripts will be called.
 * @param instance The filter instance data
 * @param session The session being closed
 */
static void closeSession(MXS_FILTER* instance, MXS_FILTER_SESSION* session)
{
    LUA_SESSION* my_session = (LUA_SESSION*) session;
    LUA_INSTANCE* my_instance = (LUA_INSTANCE*) instance;


    if (my_session->lua_state)
    {
        lua_getglobal(my_session->lua_state, "closeSession");

        if (lua_pcall(my_session->lua_state, 0, 0, 0))
        {
            MXS_WARNING("Failed to get global variable 'closeSession': '%s'."
                        " The closeSession entry point will not be called.",
                        lua_tostring(my_session->lua_state, -1));
            lua_pop(my_session->lua_state, -1);
        }
    }

    if (my_instance->global_lua_state)
    {
        std::lock_guard<std::mutex> guard(my_instance->lock);

        lua_getglobal(my_instance->global_lua_state, "closeSession");

        if (lua_pcall(my_instance->global_lua_state, 0, 0, 0))
        {
            MXS_WARNING("Failed to get global variable 'closeSession': '%s'."
                        " The closeSession entry point will not be called for the global script.",
                        lua_tostring(my_instance->global_lua_state, -1));
            lua_pop(my_instance->global_lua_state, -1);
        }
    }
}

/**
 * Free the memory associated with the session
 *
 * @param instance The filter instance
 * @param session The filter session
 */
static void freeSession(MXS_FILTER* instance, MXS_FILTER_SESSION* session)
{
    LUA_SESSION* my_session = (LUA_SESSION*) session;

    if (my_session->lua_state)
    {
        lua_close(my_session->lua_state);
    }

    MXS_FREE(my_session);
}

/**
 * The client reply entry point.
 *
 * This function calls the clientReply function of the Lua scripts.
 * @param instance Filter instance
 * @param session Filter session
 * @param queue Server response
 * @return 1 on success
 */
static int32_t clientReply(MXS_FILTER* instance,
                           MXS_FILTER_SESSION* session,
                           GWBUF* queue,
                           const mxs::ReplyRoute& down,
                           const mxs::Reply& reply)
{
    LUA_SESSION* my_session = (LUA_SESSION*) session;
    LUA_INSTANCE* my_instance = (LUA_INSTANCE*) instance;

    if (my_session->lua_state)
    {
        lua_getglobal(my_session->lua_state, "clientReply");

        if (lua_pcall(my_session->lua_state, 0, 0, 0))
        {
            MXS_ERROR("Session scope call to 'clientReply' failed: '%s'.",
                      lua_tostring(my_session->lua_state, -1));
            lua_pop(my_session->lua_state, -1);
        }
    }

    if (my_instance->global_lua_state)
    {
        std::lock_guard<std::mutex> guard(my_instance->lock);

        lua_getglobal(my_instance->global_lua_state, "clientReply");

        if (lua_pcall(my_instance->global_lua_state, 0, 0, 0))
        {
            MXS_ERROR("Global scope call to 'clientReply' failed: '%s'.",
                      lua_tostring(my_session->lua_state, -1));
            lua_pop(my_instance->global_lua_state, -1);
        }
    }

    return my_session->up->clientReply(my_session->up->instance,
                                       my_session->up->session,
                                       queue, down, reply);
}

/**
 * The routeQuery entry point. This is passed the query buffer
 * to which the filter should be applied. Once processed the
 * query is passed to the downstream component
 * (filter or router) in the filter chain.
 *
 * The Luafilter calls the routeQuery functions of both the session and the global script.
 * The query is passed as a string parameter to the routeQuery Lua function and
 * the return values of the session specific function, if any were returned,
 * are interpreted. If the first value is bool, it is interpreted as a decision
 * whether to route the query or to send an error packet to the client.
 * If it is a string, the current query is replaced with the return value and
 * the query will be routed. If nil is returned, the query is routed normally.
 *
 * @param instance The filter instance data
 * @param session The filter session
 * @param queue  The query data
 */
static int32_t routeQuery(MXS_FILTER* instance, MXS_FILTER_SESSION* session, GWBUF* queue)
{
    LUA_SESSION* my_session = (LUA_SESSION*) session;
    LUA_INSTANCE* my_instance = (LUA_INSTANCE*) instance;
    char* fullquery = NULL;
    bool route = true;
    GWBUF* forward = queue;
    int rc = 0;

    if (modutil_is_SQL(queue) || modutil_is_SQL_prepare(queue))
    {
        fullquery = modutil_get_SQL(queue);

        if (fullquery && my_session->lua_state)
        {
            /** Store the current query being processed */
            my_session->current_query = queue;

            lua_getglobal(my_session->lua_state, "routeQuery");

            lua_pushlstring(my_session->lua_state, fullquery, strlen(fullquery));

            if (lua_pcall(my_session->lua_state, 1, 1, 0))
            {
                MXS_ERROR("Session scope call to 'routeQuery' failed: '%s'.",
                          lua_tostring(my_session->lua_state, -1));
                lua_pop(my_session->lua_state, -1);
            }
            else if (lua_gettop(my_session->lua_state))
            {
                if (lua_isstring(my_session->lua_state, -1))
                {
                    gwbuf_free(forward);
                    forward = modutil_create_query(lua_tostring(my_session->lua_state, -1));
                }
                else if (lua_isboolean(my_session->lua_state, -1))
                {
                    route = lua_toboolean(my_session->lua_state, -1);
                }
            }
            my_session->current_query = NULL;
        }

        if (my_instance->global_lua_state)
        {
            std::lock_guard<std::mutex> guard(my_instance->lock);
            current_global_query = queue;

            lua_getglobal(my_instance->global_lua_state, "routeQuery");

            lua_pushlstring(my_instance->global_lua_state, fullquery, strlen(fullquery));

            if (lua_pcall(my_instance->global_lua_state, 1, 1, 0))
            {
                MXS_ERROR("Global scope call to 'routeQuery' failed: '%s'.",
                          lua_tostring(my_instance->global_lua_state, -1));
                lua_pop(my_instance->global_lua_state, -1);
            }
            else if (lua_gettop(my_instance->global_lua_state))
            {
                if (lua_isstring(my_instance->global_lua_state, -1))
                {
                    gwbuf_free(forward);
                    forward = modutil_create_query(lua_tostring(my_instance->global_lua_state, -1));
                }
                else if (lua_isboolean(my_instance->global_lua_state, -1))
                {
                    route = lua_toboolean(my_instance->global_lua_state, -1);
                }
            }

            current_global_query = NULL;
        }

        MXS_FREE(fullquery);
    }

    if (!route)
    {
        gwbuf_free(queue);
        GWBUF* err = modutil_create_mysql_err_msg(1, 0, 1045, "28000", "Access denied.");
        session_set_response(my_session->session, my_session->service,
                             my_session->up, err);
        rc = 1;
    }
    else
    {
        rc = my_session->down->routeQuery(my_session->down->instance,
                                          my_session->down->session,
                                          forward);
    }

    return rc;
}

/**
 * Diagnostics routine.
 *
 * This will call the matching diagnostics entry point in the Lua script. If the
 * Lua function returns a string, it will be printed to the client DCB.
 *
 * @param instance The filter instance
 * @param fsession Filter session, may be NULL
 */
static json_t* diagnostics(const MXS_FILTER* instance, const MXS_FILTER_SESSION* fsession)
{
    LUA_INSTANCE* my_instance = (LUA_INSTANCE*)instance;
    json_t* rval = json_object();

    if (my_instance)
    {
        if (my_instance->global_lua_state)
        {
            std::lock_guard<std::mutex> guard(my_instance->lock);

            lua_getglobal(my_instance->global_lua_state, "diagnostic");

            if (lua_pcall(my_instance->global_lua_state, 0, 1, 0) == 0)
            {
                lua_gettop(my_instance->global_lua_state);
                if (lua_isstring(my_instance->global_lua_state, -1))
                {
                    json_object_set_new(rval,
                                        "script_output",
                                        json_string(lua_tostring(my_instance->global_lua_state, -1)));
                }
            }
            else
            {
                lua_pop(my_instance->global_lua_state, -1);
            }
        }
        if (my_instance->global_script)
        {
            json_object_set_new(rval, "global_script", json_string(my_instance->global_script));
        }
        if (my_instance->session_script)
        {
            json_object_set_new(rval, "session_script", json_string(my_instance->session_script));
        }
    }

    return rval;
}

/**
 * Capability routine.
 *
 * @return The capabilities of the filter.
 */
static uint64_t getCapabilities(MXS_FILTER* instance)
{
    return RCAP_TYPE_NONE;
}

/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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
#pragma once

#include "schemarouter.hh"

#include <string>
#include <list>

#include <maxscale/router.hh>
#include <maxscale/session_command.hh>
#include <maxscale/protocol/mariadb/client_connection.hh>

#include "shard_map.hh"

namespace schemarouter
{
/**
 * Bitmask values for the router session's initialization. These values are used
 * to prevent responses from internal commands being forwarded to the client.
 */
enum init_mask
{
    INIT_READY   = 0x00,
    INIT_MAPPING = 0x01,
    INIT_USE_DB  = 0x02,
    INIT_UNINT   = 0x04,
    INIT_FAILED  = 0x08
};

enum showdb_response
{
    SHOWDB_FULL_RESPONSE,
    SHOWDB_PARTIAL_RESPONSE,
    SHOWDB_DUPLICATE_DATABASES,
    SHOWDB_FATAL_ERROR
};

#define SCHEMA_ERR_DUPLICATEDB    5000
#define SCHEMA_ERRSTR_DUPLICATEDB "DUPDB"
#define SCHEMA_ERR_DBNOTFOUND     1049
#define SCHEMA_ERRSTR_DBNOTFOUND  "42000"

/**
 * Route target types
 */
enum route_target
{
    TARGET_UNDEFINED,
    TARGET_NAMED_SERVER,
    TARGET_ALL,
    TARGET_ANY
};

/** Helper macros for route target type */
#define TARGET_IS_UNDEFINED(t)    (t == TARGET_UNDEFINED)
#define TARGET_IS_NAMED_SERVER(t) (t == TARGET_NAMED_SERVER)
#define TARGET_IS_ALL(t)          (t == TARGET_ALL)
#define TARGET_IS_ANY(t)          (t == TARGET_ANY)

class SchemaRouter;

/**
 * The client session structure used within this router.
 */
class SchemaRouterSession : public mxs::RouterSession
{
public:

    SchemaRouterSession(MXS_SESSION* session, SchemaRouter* router, SRBackendList backends);

    /**
     * Called when a client session has been closed.
     */
    void close();

    /**
     * Called when a packet being is routed to the backend. The router should
     * forward the packet to the appropriate server(s).
     *
     * @param pPacket A client packet.
     */
    int32_t routeQuery(GWBUF* pPacket);

    /**
     * Called when a packet is routed to the client. The router should
     * forward the packet to the client using `RouterSession::clientReply`.
     *
     * @param pPacket  A client packet.
     * @param pBackend The backend the packet is coming from.
     */
    void clientReply(GWBUF* pPacket, const mxs::ReplyRoute& pBackend, const mxs::Reply& reply);

    bool handleError(mxs::ErrorType type, GWBUF* pMessage, mxs::Endpoint* pProblem, const mxs::Reply& pReply);

private:
    /**
     * Internal functions
     */

    /** Helper functions */
    mxs::Target* get_shard_target(GWBUF* buffer, uint32_t qtype);
    SRBackend*   get_shard_backend(const char* name);
    bool         have_servers();
    bool         handle_default_db();
    void         handle_default_db_response();
    bool         ignore_duplicate_table(const std::string& data);
    mxs::Target* get_query_target(GWBUF* buffer);
    mxs::Target* get_ps_target(GWBUF* buffer, uint32_t qtype, qc_query_op_t op);

    /** Routing functions */
    bool         route_session_write(GWBUF* querybuf, uint8_t command);
    void         process_sescmd_response(SRBackend* bref, GWBUF** ppPacket, const mxs::Reply& reply);
    mxs::Target* resolve_query_target(GWBUF* pPacket,
                                      uint32_t type,
                                      uint8_t command,
                                      enum route_target& route_target);

    /** Shard mapping functions */
    void                 send_databases();
    bool                 send_shards();
    void                 query_databases();
    int                  inspect_mapping_states(SRBackend* bref, GWBUF** wbuf);
    enum showdb_response parse_mapping_response(SRBackend* bref, GWBUF** buffer);
    void                 route_queued_query();
    void                 synchronize_shards();
    void                 handle_mapping_reply(SRBackend* bref, GWBUF** pPacket);
    bool                 handle_statement(GWBUF* querybuf, SRBackend* bref, uint8_t command, uint32_t type);
    std::string          get_cache_key() const;

    /** Member variables */
    bool                     m_closed;          /**< True if session closed */
    MariaDBClientConnection* m_client {nullptr};/**< Client connection */

    MYSQL_session*         m_mysql_session; /**< Session client data (username, password, SHA1). */
    SRBackendList          m_backends;      /**< Backend references */
    SConfig                m_config;        /**< Session specific configuration */
    SchemaRouter*          m_router;        /**< The router instance */
    Shard                  m_shard;         /**< Database to server mapping */
    std::string            m_connect_db;    /**< Database the user was trying to connect to */
    std::string            m_current_db;    /**< Current active database */
    int                    m_state;         /**< Initialization state bitmask */
    std::list<mxs::Buffer> m_queue;         /**< Query that was received before the session was ready */
    Stats                  m_stats;         /**< Statistics for this router */
    uint64_t               m_sent_sescmd;   /**< The latest session command being executed */
    uint64_t               m_replied_sescmd;/**< The last session command reply that was sent to the client */
    mxs::Target*           m_load_target;   /**< Target for LOAD DATA LOCAL INFILE */
    SRBackend*             m_sescmd_replier {nullptr};
    int                    m_num_init_db = 0;
};
}

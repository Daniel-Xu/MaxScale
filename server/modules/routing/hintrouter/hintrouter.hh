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

#include "hintrouterdefs.hh"

#include <maxscale/router.hh>
#include "hintroutersession.hh"

class HintRouter : public maxscale::Router<HintRouter, HintRouterSession>
{
public:
    static HintRouter* create(SERVICE* pService, mxs::ConfigParameters* params);
    HintRouterSession* newSession(MXS_SESSION* pSession, const Endpoints& endpoints);
    json_t*            diagnostics() const;
    uint64_t getCapabilities() const
    {
        return RCAP_TYPE_NONE;
    }
    HINT_TYPE get_default_action() const
    {
        return m_default_action;
    }
    const string& get_default_server() const
    {
        return m_default_server;
    }
    /* Simple, approximate statistics */
    volatile unsigned int m_routed_to_master;
    volatile unsigned int m_routed_to_slave;
    volatile unsigned int m_routed_to_named;
    volatile unsigned int m_routed_to_all;
private:
    HintRouter(SERVICE* pService,
               HINT_TYPE default_action,
               string& default_server,
               int max_slaves);

    HINT_TYPE    m_default_action;
    string       m_default_server;
    int          m_max_slaves;
    volatile int m_total_slave_conns;
private:
    HintRouter(const HintRouter&);
    HintRouter& operator=(const HintRouter&);

    static bool connect_to_backend(MXS_SESSION* session,
                                   mxs::Endpoint* sref,
                                   HintRouterSession::BackendMap* all_backends);
};

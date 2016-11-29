#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cdefs.h>
#include <tr1/functional>
#include <string>
#include <maxscale/buffer.h>
#include <maxscale/session.h>
#include "cachefilter.h"
#include "cache_storage_api.h"

class SessionCache;

class Cache
{
public:
    virtual ~Cache();

    const CACHE_CONFIG& config() const { return m_config; }

    /**
     * Returns whether the results of a particular query should be stored.
     *
     * @param zDefaultDb  The current default database.
     * @param pQuery      Buffer containing a SELECT.
     *
     * @return True of the result should be cached.
     */
    bool should_store(const char* zDefaultDb, const GWBUF* pQuery);

    /**
     * Returns whether cached results should be used.
     *
     * @param pSession  The session in question.
     *
     * @return True of cached results should be used.
     */
    bool should_use(const SESSION* pSession);

    /**
     * Specifies whether a particular SessioCache should refresh the data.
     *
     * @param key            The hashed key for a query.
     * @param pSessionCache  The session cache asking.
     *
     * @return True, if the session cache should refresh the data.
     */
    virtual bool must_refresh(const CACHE_KEY& key, const SessionCache* pSessionCache) = 0;

    /**
     * To inform the cache that a particular item has been updated upon request.
     *
     * @param key            The hashed key for a query.
     * @param pSessionCache  The session cache informing.
     */
    virtual void refreshed(const CACHE_KEY& key,  const SessionCache* pSessionCache) = 0;

    virtual cache_result_t get_key(const char* zDefaultDb, const GWBUF* pQuery, CACHE_KEY* pKey) = 0;

    virtual cache_result_t get_value(const CACHE_KEY& key, uint32_t flags, GWBUF** ppValue) = 0;

    virtual cache_result_t put_value(const CACHE_KEY& key, const GWBUF* pValue) = 0;

    virtual cache_result_t del_value(const CACHE_KEY& key) = 0;

protected:
    Cache(const std::string&  name,
          const CACHE_CONFIG* pConfig,
          CACHE_RULES*        pRules,
          StorageFactory*     pFactory);

    static bool Create(const CACHE_CONFIG& config,
                       CACHE_RULES**       ppRules);

    static bool Create(const CACHE_CONFIG& config,
                       CACHE_RULES**       ppRules,
                       StorageFactory**    ppFactory);

private:
    Cache(const Cache&);
    Cache& operator = (const Cache&);

protected:
    const std::string   m_name;     // The name of the instance; the section name in the config.
    const CACHE_CONFIG& m_config;   // The configuration of the cache instance.
    CACHE_RULES*        m_pRules;   // The rules of the cache instance.
    StorageFactory*     m_pFactory; // The storage factory.
};

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
#include <maxscale/spinlock.h>
#include "lrustorage.h"

class LRUStorageMT : public LRUStorage
{
public:
    ~LRUStorageMT();

    LRUStorageMT* create(Storage* pstorage, size_t max_count, size_t max_size);

    cache_result_t get_value(const CACHE_KEY& key,
                             uint32_t flags,
                             GWBUF** ppvalue);

    cache_result_t put_value(const CACHE_KEY& key,
                             const GWBUF* pvalue);

    cache_result_t del_value(const CACHE_KEY& key);

private:
    LRUStorageMT(Storage* pstorage, size_t max_count, size_t max_size);

    LRUStorageMT(const LRUStorageMT&);
    LRUStorageMT& operator = (const LRUStorageMT&);

private:
    SPINLOCK lock_;
};

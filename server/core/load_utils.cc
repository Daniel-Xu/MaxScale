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
 * @file load_utils.c Utility functions for loading of modules
 */

#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <ftw.h>
#include <algorithm>
#include <string>

#include <maxscale/modinfo.hh>
#include <maxscale/version.h>
#include <maxscale/paths.hh>
#include <maxbase/alloc.h>
#include <maxscale/json_api.hh>
#include <maxscale/modulecmd.hh>
#include <maxscale/protocol.hh>
#include <maxscale/router.hh>
#include <maxscale/filter.hh>
#include <maxscale/authenticator.hh>
#include <maxscale/monitor.hh>
#include <maxscale/query_classifier.hh>
#include <maxscale/routingworker.hh>

#include "internal/modules.hh"
#include "internal/config.hh"
#include "internal/monitor.hh"
#include "internal/server.hh"
#include "internal/service.hh"
#include "internal/listener.hh"

namespace
{
const char CN_ARG_MAX[] = "arg_max";
const char CN_ARG_MIN[] = "arg_min";
const char CN_METHOD[] = "method";
const char CN_MODULES[] = "modules";
const char CN_MODULE_COMMAND[] = "module_command";


typedef struct loaded_module
{
    char*                  module;  /**< The name of the module */
    char*                  type;    /**< The module type */
    char*                  version; /**< Module version */
    void*                  handle;  /**< The handle returned by dlopen */
    void*                  modobj;  /**< The module "object" this is the set of entry points */
    MXS_MODULE*            info;    /**< The module information */
    struct  loaded_module* next;    /**< Next module in the linked list */
} LOADED_MODULE;

struct NAME_MAPPING
{
    const char* type;   // The type of the module.
    const char* from;   // Old module name.
    const char* to;     // What should be loaded instead.
    bool        warned; // Whether a warning has been logged.
};
}

static NAME_MAPPING name_mappings[] =
{
    {MODULE_MONITOR,       "clustrixmon", "xpandmon",      false},
    {MODULE_MONITOR,       "mysqlmon",    "mariadbmon",    false},
    {MODULE_PROTOCOL,      "mysqlclient", "mariadbclient", false},
    {MODULE_PROTOCOL,      "mariadb",     "mariadbclient", true },
    {MODULE_AUTHENTICATOR, "mysqlauth",   "mariadbauth",   false},
};

static const size_t N_NAME_MAPPINGS = sizeof(name_mappings) / sizeof(name_mappings[0]);

static LOADED_MODULE* registered = NULL;

static LOADED_MODULE* find_module(const char* module);
static LOADED_MODULE* register_module(const char* module,
                                      const char* type,
                                      void* dlhandle,
                                      MXS_MODULE* mod_info);
static void unregister_module(const char* module);

static const char* module_type_to_str(MXS_MODULE_API type)
{
    switch (type)
    {
    case MXS_MODULE_API_PROTOCOL:
        return MODULE_PROTOCOL;

    case MXS_MODULE_API_AUTHENTICATOR:
        return MODULE_AUTHENTICATOR;

    case MXS_MODULE_API_ROUTER:
        return MODULE_ROUTER;

    case MXS_MODULE_API_MONITOR:
        return MODULE_MONITOR;

    case MXS_MODULE_API_FILTER:
        return MODULE_FILTER;

    case MXS_MODULE_API_QUERY_CLASSIFIER:
        return MODULE_QUERY_CLASSIFIER;
    }

    mxb_assert(!true);
    return "unknown";
}

static bool api_version_mismatch(const MXS_MODULE* mod_info, const char* module)
{
    bool rval = false;
    MXS_MODULE_VERSION api = {};

    switch (mod_info->modapi)
    {
    case MXS_MODULE_API_PROTOCOL:
        api = MXS_PROTOCOL_VERSION;
        break;

    case MXS_MODULE_API_AUTHENTICATOR:
        api = MXS_AUTHENTICATOR_VERSION;
        break;

    case MXS_MODULE_API_ROUTER:
        api = MXS_ROUTER_VERSION;
        break;

    case MXS_MODULE_API_MONITOR:
        api = MXS_MONITOR_VERSION;
        break;

    case MXS_MODULE_API_FILTER:
        api = MXS_FILTER_VERSION;
        break;

    case MXS_MODULE_API_QUERY_CLASSIFIER:
        api = MXS_QUERY_CLASSIFIER_VERSION;
        break;

    default:
        MXS_ERROR("Unknown module type: 0x%02hhx", mod_info->modapi);
        mxb_assert(!true);
        break;
    }

    if (api.major != mod_info->api_version.major
        || api.minor != mod_info->api_version.minor
        || api.patch != mod_info->api_version.patch)
    {
        MXS_ERROR("API version mismatch for '%s': Need version %d.%d.%d, have %d.%d.%d",
                  module,
                  api.major,
                  api.minor,
                  api.patch,
                  mod_info->api_version.major,
                  mod_info->api_version.minor,
                  mod_info->api_version.patch);
        rval = true;
    }

    return rval;
}

static bool check_module(const MXS_MODULE* mod_info, const char* type, const char* module)
{
    bool success = true;

    if (type)
    {
        if (strcmp(type, MODULE_PROTOCOL) == 0
            && mod_info->modapi != MXS_MODULE_API_PROTOCOL)
        {
            MXS_ERROR("Module '%s' does not implement the protocol API.", module);
            success = false;
        }
        if (strcmp(type, MODULE_AUTHENTICATOR) == 0
            && mod_info->modapi != MXS_MODULE_API_AUTHENTICATOR)
        {
            MXS_ERROR("Module '%s' does not implement the authenticator API.", module);
            success = false;
        }
        if (strcmp(type, MODULE_ROUTER) == 0
            && mod_info->modapi != MXS_MODULE_API_ROUTER)
        {
            MXS_ERROR("Module '%s' does not implement the router API.", module);
            success = false;
        }
        if (strcmp(type, MODULE_MONITOR) == 0
            && mod_info->modapi != MXS_MODULE_API_MONITOR)
        {
            MXS_ERROR("Module '%s' does not implement the monitor API.", module);
            success = false;
        }
        if (strcmp(type, MODULE_FILTER) == 0
            && mod_info->modapi != MXS_MODULE_API_FILTER)
        {
            MXS_ERROR("Module '%s' does not implement the filter API.", module);
            success = false;
        }
        if (strcmp(type, MODULE_QUERY_CLASSIFIER) == 0
            && mod_info->modapi != MXS_MODULE_API_QUERY_CLASSIFIER)
        {
            MXS_ERROR("Module '%s' does not implement the query classifier API.", module);
            success = false;
        }
    }

    if (api_version_mismatch(mod_info, module))
    {
        success = false;
    }

    if (mod_info->version == NULL)
    {
        MXS_ERROR("Module '%s' does not define a version string", module);
        success = false;
    }

    if (mod_info->module_object == NULL)
    {
        MXS_ERROR("Module '%s' does not define a module object", module);
        success = false;
    }

    return success;
}

static bool is_maxscale_module(const char* fpath)
{
    bool rval = false;

    if (void* dlhandle = dlopen(fpath, RTLD_LAZY | RTLD_LOCAL))
    {
        if (void* sym = dlsym(dlhandle, MXS_MODULE_SYMBOL_NAME))
        {
            Dl_info info;

            if (dladdr(sym, &info))
            {
                if (strcmp(info.dli_fname, fpath) == 0)
                {
                    // The module entry point symbol is located in the file we're loading,
                    // this is a MaxScale module.
                    rval = true;
                }
            }
        }

        dlclose(dlhandle);
    }

    if (!rval)
    {
        MXS_INFO("Not a MaxScale module: %s", fpath);
    }

    return rval;
}

static int load_module_cb(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf)
{
    int rval = 0;

    if (typeflag == FTW_F)
    {
        const char* filename = fpath + ftwbuf->base;

        if (strncmp(filename, "lib", 3) == 0)
        {
            const char* name = filename + 3;

            if (const char* dot = strchr(filename, '.'))
            {
                std::string module(name, dot);

                if (is_maxscale_module(fpath) && !load_module(module.c_str(), nullptr))
                {
                    MXS_ERROR("Failed to load '%s'. Make sure it is not a stale library "
                              "left over from an old installation of MaxScale.", fpath);
                    rval = 1;
                }
            }
        }
    }

    return rval;
}

bool load_all_modules()
{
    int rv = nftw(mxs::libdir(), load_module_cb, 10, FTW_PHYS);
    return rv == 0;
}

void* load_module(const char* module, const char* type)
{
    mxb_assert(module);
    LOADED_MODULE* mod;

    module = mxs_module_get_effective_name(module);

    if ((mod = find_module(module)) == NULL)
    {
        size_t len = strlen(module);
        char lc_module[len + 1];
        lc_module[len] = 0;
        std::transform(module, module + len, lc_module, tolower);

        /** The module is not already loaded, search for the shared object */
        char fname[MAXPATHLEN + 1];
        snprintf(fname, MAXPATHLEN + 1, "%s/lib%s.so", mxs::libdir(), lc_module);

        if (access(fname, F_OK) == -1)
        {
            MXS_ERROR("Unable to find library for "
                      "module: %s. Module dir: %s",
                      module,
                      mxs::libdir());
            return NULL;
        }

        void* dlhandle = dlopen(fname, RTLD_NOW | RTLD_LOCAL);

        if (dlhandle == NULL)
        {
            MXS_ERROR("Unable to load library for module: "
                      "%s\n\n\t\t      %s."
                      "\n\n",
                      module,
                      dlerror());
            return NULL;
        }

        void* sym = dlsym(dlhandle, MXS_MODULE_SYMBOL_NAME);

        if (sym == NULL)
        {
            MXS_ERROR("Expected entry point interface missing "
                      "from module: %s\n\t\t\t      %s.",
                      module,
                      dlerror());
            dlclose(dlhandle);
            return NULL;
        }

        void* (* entry_point)() = (void* (*)())sym;
        MXS_MODULE* mod_info = (MXS_MODULE*)entry_point();

        if (!check_module(mod_info, type, module))
        {
            dlclose(dlhandle);
            return NULL;
        }

        mod = register_module(module, module_type_to_str(mod_info->modapi), dlhandle, mod_info);
        mxb_assert(mod);

        MXS_NOTICE("Loaded module %s: %s from %s", module, mod_info->version, fname);

        if (mxs::RoutingWorker::is_running())
        {
            if (mod_info->process_init)
            {
                mod_info->process_init();
            }

            if (mod_info->thread_init)
            {
                mxs::RoutingWorker::broadcast(
                    [mod_info]() {
                        mod_info->thread_init();
                    }, mxs::RoutingWorker::EXECUTE_AUTO);

                if (mxs::MainWorker::created())
                {
                    mxs::MainWorker::get()->call(
                        [mod_info]() {
                            mod_info->thread_init();
                        }, mxb::Worker::EXECUTE_AUTO);
                }
            }
        }
    }

    return mod->modobj;
}

void unload_module(const char* module)
{
    module = mxs_module_get_effective_name(module);

    LOADED_MODULE* mod = find_module(module);

    if (mod)
    {
        void* handle = mod->handle;
        unregister_module(module);
        dlclose(handle);
    }
}

/**
 * Find a module that has been previously loaded and return the handle for that
 * library
 *
 * @param module        The name of the module
 * @return              The module handle or NULL if it was not found
 */
static LOADED_MODULE* find_module(const char* module)
{
    LOADED_MODULE* mod = registered;

    if (module)
    {
        while (mod)
        {
            if (strcasecmp(mod->module, module) == 0)
            {
                return mod;
            }
            else
            {
                mod = mod->next;
            }
        }
    }
    return NULL;
}

/**
 * Register a newly loaded module. The registration allows for single copies
 * to be loaded and cached entry point information to be return.
 *
 * @param module        The name of the module loaded
 * @param type          The type of the module loaded
 * @param dlhandle      The handle returned by dlopen
 * @param version       The version string returned by the module
 * @param modobj        The module object
 * @param mod_info      The module information
 * @return The new registered module or NULL on memory allocation failure
 */
static LOADED_MODULE* register_module(const char* module,
                                      const char* type,
                                      void* dlhandle,
                                      MXS_MODULE* mod_info)
{
    module = MXS_STRDUP(module);
    type = MXS_STRDUP(type);
    char* version = MXS_STRDUP(mod_info->version);

    LOADED_MODULE* mod = (LOADED_MODULE*)MXS_MALLOC(sizeof(LOADED_MODULE));

    if (!module || !type || !version || !mod)
    {
        MXS_FREE((void*)module);
        MXS_FREE((void*)type);
        MXS_FREE(version);
        MXS_FREE(mod);
        return NULL;
    }

    mod->module = (char*)module;
    mod->type = (char*)type;
    mod->handle = dlhandle;
    mod->version = version;
    mod->modobj = mod_info->module_object;
    mod->next = registered;
    mod->info = mod_info;
    registered = mod;
    return mod;
}

/**
 * Unregister a module
 *
 * @param module        The name of the module to remove
 */
static void unregister_module(const char* module)
{
    LOADED_MODULE* mod = find_module(module);
    LOADED_MODULE* ptr;

    if (!mod)
    {
        return;         // Module not found
    }
    if (registered == mod)
    {
        registered = mod->next;
    }
    else
    {
        ptr = registered;
        while (ptr && ptr->next != mod)
        {
            ptr = ptr->next;
        }

        /*<
         * Remove the module to be be freed from the list.
         */
        if (ptr && (ptr->next == mod))
        {
            ptr->next = ptr->next->next;
        }
    }

    /*<
     * The module is now not in the linked list and all
     * memory related to it can be freed
     */
    dlclose(mod->handle);
    MXS_FREE(mod->module);
    MXS_FREE(mod->type);
    MXS_FREE(mod->version);
    MXS_FREE(mod);
}

void unload_all_modules()
{
    while (registered)
    {
        unregister_module(registered->module);
    }
}

struct cb_param
{
    json_t*     commands;
    const char* domain;
    const char* host;
};

bool modulecmd_cb(const MODULECMD* cmd, void* data)
{
    cb_param* d = static_cast<cb_param*>(data);

    json_t* obj = json_object();
    json_object_set_new(obj, CN_ID, json_string(cmd->identifier));
    json_object_set_new(obj, CN_TYPE, json_string(CN_MODULE_COMMAND));

    json_t* attr = json_object();
    const char* method = MODULECMD_MODIFIES_DATA(cmd) ? "POST" : "GET";
    json_object_set_new(attr, CN_METHOD, json_string(method));
    json_object_set_new(attr, CN_ARG_MIN, json_integer(cmd->arg_count_min));
    json_object_set_new(attr, CN_ARG_MAX, json_integer(cmd->arg_count_max));
    json_object_set_new(attr, CN_DESCRIPTION, json_string(cmd->description));

    json_t* param = json_array();

    for (int i = 0; i < cmd->arg_count_max; i++)
    {
        json_t* p = json_object();
        json_object_set_new(p, CN_DESCRIPTION, json_string(cmd->arg_types[i].description));
        json_object_set_new(p, CN_TYPE, json_string(modulecmd_argtype_to_str(&cmd->arg_types[i])));
        json_object_set_new(p, CN_REQUIRED, json_boolean(MODULECMD_ARG_IS_REQUIRED(&cmd->arg_types[i])));
        json_array_append_new(param, p);
    }

    std::string s = d->domain;
    s += "/";
    s += cmd->identifier;
    mxb_assert(strcasecmp(d->domain, cmd->domain) == 0);

    json_object_set_new(obj, CN_LINKS, mxs_json_self_link(d->host, CN_MODULES, s.c_str()));
    json_object_set_new(attr, CN_PARAMETERS, param);
    json_object_set_new(obj, CN_ATTRIBUTES, attr);

    json_array_append_new(d->commands, obj);

    return true;
}

static json_t* default_value_to_json(mxs_module_param_type type, const char* value)
{
    switch (type)
    {
    case MXS_MODULE_PARAM_COUNT:
    case MXS_MODULE_PARAM_INT:
        return json_integer(strtol(value, nullptr, 10));

    case MXS_MODULE_PARAM_SIZE:
        {
            uint64_t val = 0;
            get_suffixed_size(value, &val);
            return json_integer(val);
        }

    case MXS_MODULE_PARAM_BOOL:
        return json_boolean(config_truth_value(value));

    case MXS_MODULE_PARAM_STRING:
    case MXS_MODULE_PARAM_QUOTEDSTRING:
    case MXS_MODULE_PARAM_PASSWORD:
    case MXS_MODULE_PARAM_ENUM:
    case MXS_MODULE_PARAM_PATH:
    case MXS_MODULE_PARAM_SERVICE:
    case MXS_MODULE_PARAM_SERVER:
    case MXS_MODULE_PARAM_TARGET:
    case MXS_MODULE_PARAM_SERVERLIST:
    case MXS_MODULE_PARAM_TARGETLIST:
    case MXS_MODULE_PARAM_REGEX:
    case MXS_MODULE_PARAM_DURATION:
        return json_string(value);

    default:
        mxb_assert(!true);
        return json_null();
    }
}
static json_t* module_param_to_json(const MXS_MODULE_PARAM& param)
{
    json_t* p = json_object();
    const char* type;

    if (param.type == MXS_MODULE_PARAM_ENUM && (param.options & MXS_MODULE_OPT_ENUM_UNIQUE) == 0)
    {
        type = "enum_mask";
    }
    else
    {
        type = mxs_module_param_type_to_string(param.type);
    }

    json_object_set_new(p, CN_NAME, json_string(param.name));
    json_object_set_new(p, CN_TYPE, json_string(type));

    if (param.default_value)
    {
        json_object_set_new(p, "default_value", default_value_to_json(param.type, param.default_value));
    }

    json_object_set_new(p, "mandatory", json_boolean(param.options & MXS_MODULE_OPT_REQUIRED));

    if (param.type == MXS_MODULE_PARAM_ENUM && param.accepted_values)
    {
        json_t* arr = json_array();

        for (int x = 0; param.accepted_values[x].name; x++)
        {
            json_array_append_new(arr, json_string(param.accepted_values[x].name));
        }

        json_object_set_new(p, "enum_values", arr);
    }
    else if (param.type == MXS_MODULE_PARAM_DURATION)
    {
        const char* value_unit = param.options & MXS_MODULE_OPT_DURATION_S ? "s" : "ms";
        json_object_set_new(p, "unit", json_string(value_unit));
    }

    return p;
}

static json_t* module_json_data(const LOADED_MODULE* mod, const char* host)
{
    json_t* obj = json_object();

    json_object_set_new(obj, CN_ID, json_string(mod->module));
    json_object_set_new(obj, CN_TYPE, json_string(CN_MODULES));

    json_t* attr = json_object();
    json_object_set_new(attr, "module_type", json_string(mod->type));
    json_object_set_new(attr, "version", json_string(mod->info->version));
    json_object_set_new(attr, CN_DESCRIPTION, json_string(mod->info->description));
    json_object_set_new(attr, "api", json_string(mxs_module_api_to_string(mod->info->modapi)));
    json_object_set_new(attr, "maturity", json_string(mxs_module_status_to_string(mod->info->status)));

    json_t* commands = json_array();
    cb_param p = {commands, mod->module, host};
    modulecmd_foreach(mod->module, NULL, modulecmd_cb, &p);

    json_t* params = json_array();

    for (int i = 0; mod->info->parameters[i].name; i++)
    {
        json_array_append_new(params, module_param_to_json(mod->info->parameters[i]));
    }

    const MXS_MODULE_PARAM* extra = nullptr;
    std::set<std::string> ignored;

    switch (mod->info->modapi)
    {
    case MXS_MODULE_API_FILTER:
    case MXS_MODULE_API_AUTHENTICATOR:
    case MXS_MODULE_API_QUERY_CLASSIFIER:
        break;

    case MXS_MODULE_API_PROTOCOL:
        extra = common_listener_params();
        ignored = {CN_SERVICE, CN_TYPE, CN_MODULE};
        break;

    case MXS_MODULE_API_ROUTER:
        extra = common_service_params();
        ignored = {CN_SERVERS, CN_TARGETS, CN_ROUTER, CN_TYPE, CN_CLUSTER, CN_FILTERS};
        break;

    case MXS_MODULE_API_MONITOR:
        extra = common_monitor_params();
        ignored = {CN_SERVERS, CN_TYPE, CN_MODULE};
        break;
    }

    if (extra)
    {
        for (int i = 0; extra[i].name; i++)
        {
            if (ignored.count(extra[i].name) == 0)
            {
                json_array_append_new(params, module_param_to_json(extra[i]));
            }
        }
    }

    json_object_set_new(attr, "commands", commands);
    json_object_set_new(attr, CN_PARAMETERS, params);
    json_object_set_new(obj, CN_ATTRIBUTES, attr);
    json_object_set_new(obj, CN_LINKS, mxs_json_self_link(host, CN_MODULES, mod->module));

    return obj;
}

json_t* module_to_json(const MXS_MODULE* module, const char* host)
{
    json_t* data = NULL;

    for (LOADED_MODULE* ptr = registered; ptr; ptr = ptr->next)
    {
        if (ptr->info == module)
        {
            data = module_json_data(ptr, host);
            break;
        }
    }

    // This should always be non-NULL
    mxb_assert(data);

    return mxs_json_resource(host, MXS_JSON_API_MODULES, data);
}

json_t* spec_module_json_data(const char* host, const mxs::config::Specification& spec)
{
    json_t* commands = json_array();
    // TODO: The following data will now be somewhat different compared to
    // TODO: what the modules that do not use the new configuration mechanism
    // TODO: return.
    json_t* params = spec.to_json();

    json_t* attr = json_object();
    json_object_set_new(attr, "module_type", json_string(spec.module().c_str()));
    json_object_set_new(attr, "version", json_string(MAXSCALE_VERSION));
    json_object_set_new(attr, CN_DESCRIPTION, json_string(spec.module().c_str()));
    json_object_set_new(attr, "maturity", json_string("GA"));
    json_object_set_new(attr, "commands", commands);
    json_object_set_new(attr, CN_PARAMETERS, params);

    json_t* obj = json_object();
    json_object_set_new(obj, CN_ID, json_string(spec.module().c_str()));
    json_object_set_new(obj, CN_TYPE, json_string(CN_MODULES));
    json_object_set_new(obj, CN_ATTRIBUTES, attr);
    json_object_set_new(obj, CN_LINKS, mxs_json_self_link(host, CN_MODULES, spec.module().c_str()));

    return obj;
}

json_t* spec_module_to_json(const char* host, const mxs::config::Specification& spec)
{
    json_t* data = spec_module_json_data(host, spec);

    return mxs_json_resource(host, MXS_JSON_API_MODULES, data);
}

json_t* module_list_to_json(const char* host)
{
    json_t* arr = json_array();

    json_array_append_new(arr, spec_module_json_data(host, mxs::Config::get().specification()));
    json_array_append_new(arr, spec_module_json_data(host, Server::specification()));

    for (LOADED_MODULE* ptr = registered; ptr; ptr = ptr->next)
    {
        json_array_append_new(arr, module_json_data(ptr, host));
    }

    return mxs_json_resource(host, MXS_JSON_API_MODULES, arr);
}

static const char* module_status_to_string(LOADED_MODULE* ptr)
{
    switch (ptr->info->status)
    {
    case MXS_MODULE_IN_DEVELOPMENT:
        return "In Development";

    case MXS_MODULE_ALPHA_RELEASE:
        return "Alpha";

    case MXS_MODULE_BETA_RELEASE:
        return "Beta";

    case MXS_MODULE_GA:
        return "GA";

    case MXS_MODULE_EXPERIMENTAL:
        return "Experimental";
    }

    return "Unknown";
}

const MXS_MODULE* get_module(const char* name, const char* type)
{
    name = mxs_module_get_effective_name(name);

    LOADED_MODULE* mod = find_module(name);

    if (mod == NULL && type && load_module(name, type))
    {
        mod = find_module(name);
    }

    return mod ? mod->info : NULL;
}

MXS_MODULE_ITERATOR mxs_module_iterator_get(const char* type)
{
    LOADED_MODULE* module = registered;

    while (module && type && (strcmp(module->type, type) != 0))
    {
        module = module->next;
    }

    MXS_MODULE_ITERATOR iterator;
    iterator.type = type;
    iterator.position = module;

    return iterator;
}

bool mxs_module_iterator_has_next(const MXS_MODULE_ITERATOR* iterator)
{
    return iterator->position != NULL;
}

MXS_MODULE* mxs_module_iterator_get_next(MXS_MODULE_ITERATOR* iterator)
{
    MXS_MODULE* module = NULL;
    LOADED_MODULE* loaded_module = (LOADED_MODULE*)iterator->position;

    if (loaded_module)
    {
        module = loaded_module->info;

        do
        {
            loaded_module = loaded_module->next;
        }
        while (loaded_module && iterator->type && (strcmp(loaded_module->type, iterator->type) != 0));

        iterator->position = loaded_module;
    }

    return module;
}

const char* mxs_module_get_effective_name(const char* name)
{
    const char* effective_name = NULL;
    size_t i = 0;

    while (!effective_name && (i < N_NAME_MAPPINGS))
    {
        NAME_MAPPING& nm = name_mappings[i];

        if (strcasecmp(name, nm.from) == 0)
        {
            if (!nm.warned)
            {
                MXS_WARNING("%s module '%s' has been deprecated, use '%s' instead.",
                            nm.type,
                            nm.from,
                            nm.to);
                nm.warned = true;
            }
            effective_name = nm.to;
        }

        ++i;
    }

    if (!effective_name)
    {
        effective_name = name;
    }

    return effective_name;
}

bool modules_thread_init()
{
    bool initialized = false;

    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_init)
        {
            int rc = (module->thread_init)();

            if (rc != 0)
            {
                break;
            }
        }
    }

    if (module)
    {
        // If module is non-NULL it means that the initialization failed for
        // that module. We now need to call finish on all modules that were
        // successfully initialized.
        MXS_MODULE* failed_module = module;
        i = mxs_module_iterator_get(NULL);

        while ((module = mxs_module_iterator_get_next(&i)) != failed_module)
        {
            if (module->thread_finish)
            {
                (module->thread_finish)();
            }
        }
    }
    else
    {
        initialized = true;
    }

    return initialized;
}

void modules_thread_finish()
{
    MXS_MODULE_ITERATOR i = mxs_module_iterator_get(NULL);
    MXS_MODULE* module = NULL;

    while ((module = mxs_module_iterator_get_next(&i)) != NULL)
    {
        if (module->thread_finish)
        {
            (module->thread_finish)();
        }
    }
}

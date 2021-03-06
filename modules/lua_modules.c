/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ****************************************************************************/

/**
 * @file
 * @brief IronBee --- Lua Modules
 *
 * IronBee Modules as Lua scripts.
 *
 * @author Sam Baskinger <sbaskinger@qualys.com>
 */

#include "lua_modules_private.h"
#include "lua_private.h"
#include "lua_runtime_private.h"

#include <ironbee/context.h>
#include <ironbee/engine_state.h>

#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

//! Callback data for Lua module configuration callbacks.
struct modlua_cfg_cbdata_t {
    lua_State   *L;      /**< The Lua configuration stack. */
    ib_module_t *module; /**< The Lua-defined module. */
};
typedef struct modlua_cfg_cbdata_t modlua_cfg_cbdata_t;


/**
 * A container to hold both ibmod_lua and a user-defined Lua module.
 *
 * This is used as callback data to state handlers that need to know
 * which user-defined module they were registered as, as well as,
 * which modules the ibmod_lua module was registered as.
 */
struct modlua_modules_t {
    /**
     * The Lua module pointer, not the pointer to a Lua-implemented module.
     *
     * This is used to retrieve shared runtimes and other global configuration.
     */
    ib_module_t *modlua;

    /**
     * Pointer to the Lua module created by the user. This represents Lua code.
     *
     * This is used when calling the Lua code to fetch configurations, etc.
     */
    ib_module_t *module;
};
typedef struct modlua_modules_t modlua_modules_t;

/**
 * Callback data for the modlua_luamod_init function.
 *
 * This is used to initialize Lua Modules.
 */
typedef struct modlua_luamod_init_t {
    const char  *file;       /**< Lua File to load. */
    /**
     * The Lua module. Not the user's module written in Lua.
     */
    ib_module_t  *modlua;
    modlua_cfg_t *modlua_cfg; /**< Configuration for modlua. */
} modlua_luamod_init_t;

/**
 * Push the specified handler for a lua module on top of the Lua stack L.
 *
 * @param[in] ib IronBee engine.
 * @param[in] modlua_modules Lua and lua-defined modules.
 * @param[in] state The state.
 * @param[out] L the execution environment to modify.
 *
 * @returns
 *   - IB_OK on success. The stack is 1 element higher.
 *   - IB_EINVAL on a Lua runtime error.
 */
static ib_status_t modlua_push_lua_handler(
    ib_engine_t      *ib,
    modlua_modules_t *modlua_modules,
    ib_state_t        state,
    lua_State        *L
    )
{
    assert(ib != NULL);
    assert(modlua_modules != NULL);
    assert(modlua_modules->module != NULL);
    assert(L != NULL);
    assert(lua_checkstack(L, 6));

    /* Use the user-defined lua module. Do not use ibmod_lua.so. */
    ib_module_t *module = modlua_modules->module;
    ib_status_t rc;

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        return IB_EINVAL;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_getfield(L, -1, "get_callbacks"); /* Push get_callback func. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function get_callbacks is undefined.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(ib, "Module function get_callbacks is not a function.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_pushlightuserdata(L, ib);
    lua_pushinteger(L, module->idx);
    lua_pushinteger(L, state);

    rc = ib_lua_pcall(ib, L, 3, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure in Lua module %s. See previous messages.",
            module->name);
        return rc;
    }

    /* Is the result a table, which should list the functions. */
    if (lua_istable(L, -1)) {
        /* Pop off modlua table by moving the function at -1 to -2 and popping. */
        lua_replace(L, -2);
        return IB_OK;
    }
    else if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return IB_ENOENT;
    }
    else {
        lua_pop(L, 2);
        return IB_EINVAL;
    }
}

/**
 * Push the specified handler for a lua module on top of the Lua stack L.
 *
 * @param[in] ib IronBee engine.
 * @param[in] modlua_modules Lua and lua-defined modules.
 * @param[out] L the execution environment to modify.
 *
 * @returns
 *   - IB_OK on success. The stack is 1 element higher.
 *   - IB_EINVAL on a Lua runtime error.
 */
static ib_status_t modlua_push_lua_handler_logevents(
    ib_engine_t      *ib,
    modlua_modules_t *modlua_modules,
    lua_State        *L
)
{
    assert(ib != NULL);
    assert(modlua_modules != NULL);
    assert(modlua_modules->module != NULL);
    assert(L != NULL);
    assert(lua_checkstack(L, 5));

    /* Use the user-defined lua module. Do not use ibmod_lua.so. */
    ib_module_t *module = modlua_modules->module;
    ib_status_t rc;

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        return IB_EINVAL;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_getfield(L, -1, "get_callbacks_logevents"); /* Push get_callback func. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function get_callbacks is undefined.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(ib, "Module function get_callbacks is not a function.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_pushlightuserdata(L, ib);
    lua_pushinteger(L, module->idx);

    rc = ib_lua_pcall(ib, L, 2, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure in Lua module %s. See previous messages.",
            module->name);
        return rc;
    }

    /* Is the result a table, which should list the functions. */
    if (lua_istable(L, -1)) {
        /* Pop off modlua table by moving the function at -1 to -2 and popping. */
        lua_replace(L, -2);
        return IB_OK;
    }
    else if (lua_isnil(L, -1)) {
        lua_pop(L, 2);
        return IB_ENOENT;
    }
    else {
        lua_pop(L, 2);
        return IB_EINVAL;
    }
}

/**
 * Push the lua callback dispatcher function to the stack.
 *
 * It takes a callback function handler and a table of arguments as
 * arguments. When run, it will pre-process any arguments
 * using the FFI and hand the user a final table.
 *
 * @param[in] ib IronBee engine.
 * @param[in] state The state to check for.
 * @param[out] L The Lua state to push the dispatcher onto.
 *
 * @returns
 *   - IB_OK if a handler exists.
 *   - IB_ENOENT if a handler does not exist.
 *   - IB_EINVAL on a Lua runtime error. See log file for details.
 */
static ib_status_t modlua_push_dispatcher(
    ib_engine_t *ib,
    ib_state_t state,
    lua_State *L)
{
    assert(ib != NULL);
    assert(L != NULL);
    assert(lua_checkstack(L, 2));

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        return IB_EINVAL;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_getfield(L, -1, "dispatch_module"); /* Push dispatch_module func. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function dispatch_module is undefined.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(ib, "Module function dispatch_module is not a function.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    /* Replace the modlua table by replacing it with dispatch_handler. */
    lua_replace(L, -2);

    return IB_OK;
}

/**
 * Check if a Lua module has a callback handler for a log events
 *
 * @param[in] ib IronBee engine.
 * @param[in] ibmod_modules Lua and lua-defined modules.
 * @param[in] L The Lua state that is checked. While it is an "in"
 *            parameter, it is manipulated and returned to its
 *            original state before this function returns.
 *
 * @returns
 *   - IB_OK if a handler exists.
 *   - IB_ENOENT if a handler does not exist.
 *   - IB_EINVAL on a Lua runtime error. See log file for details.
 */
static ib_status_t modlua_has_callback_logevents(
    ib_engine_t *ib,
    modlua_modules_t *ibmod_modules,
    lua_State *L
)
{
    assert(ib != NULL);
    assert(ibmod_modules != NULL);
    assert(L != NULL);
    assert(lua_checkstack(L, 1));

    ib_status_t rc;

    rc = modlua_push_lua_handler_logevents(ib, ibmod_modules, L);

    if (rc == IB_OK) {
        /* Pop the lua handler off the stack. We're just checking for it. */
        lua_pop(L, 1);
    }

    return rc;
}

/**
 * Check if a Lua module has a callback handler for a particular state.
 *
 * @param[in] ib IronBee engine.
 * @param[in] ibmod_modules Lua and lua-defined modules.
 * @param[in] state The state to check for.
 * @param[in] L The Lua state that is checked. While it is an "in"
 *            parameter, it is manipulated and returned to its
 *            original state before this function returns.
 *
 * @returns
 *   - IB_OK if a handler exists.
 *   - IB_ENOENT if a handler does not exist.
 *   - IB_EINVAL on a Lua runtime error. See log file for details.
 */
static ib_status_t module_has_callback(
    ib_engine_t *ib,
    modlua_modules_t *ibmod_modules,
    ib_state_t state,
    lua_State *L)
{
    assert(ib != NULL);
    assert(ibmod_modules != NULL);
    assert(L != NULL);
    assert(lua_checkstack(L, 1));

    ib_status_t rc;

    rc = modlua_push_lua_handler(ib, ibmod_modules, state, L);

    if (rc == IB_OK) {
        /* Pop the lua handler off the stack. We're just checking for it. */
        lua_pop(L, 1);
    }

    return rc;
}

/**
 * Push the first 8 arguments for the callback dispatch lua function.
 *
 * Other arguments are conditional to the particular state.
 *
 * Functions (callback hooks) that use this function should then
 * modify the table at the top of the stack to include custom
 * arguments and then call @ref ib_lua_pcall().
 *
 * The table at the top of the stack will have defined in it:
 *   - @c ib_engine
 *   - @c ib_tx (if @a tx is not null)
 *   - @c ib_conn
 *   - @c state as an integer
 *
 * @param[in] ib The IronBee engine. This may not be null.
 * @param[in] state The state.
 * @param[in] tx The transaction. This may be null.
 * @param[in] conn The connection. This may be null.
 * @param[in] modlua_runtime Lua runtime.
 * @param[in] modlua_modules Both the lua module and the user's
 *            lua-defined module.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EOTHER on a Lua runtime error.
 */
static
ib_status_t modlua_callback_setup(
    ib_engine_t      *ib,
    ib_state_t        state,
    ib_tx_t          *tx,
    ib_conn_t        *conn,
    modlua_runtime_t *modlua_runtime,
    modlua_modules_t *modlua_modules
)
{
    assert(ib                     != NULL);
    assert(modlua_runtime         != NULL);
    assert(modlua_runtime->L      != NULL);
    assert(modlua_modules         != NULL);
    assert(modlua_modules->module != NULL);

    /* Pick the best context to use. */
    ib_context_t *ctx = ib_context_get_context(ib, conn, tx);
    lua_State    *L   = modlua_runtime->L;
    ib_status_t   rc;

    assert(lua_checkstack(L, 9));

    /* Push Lua dispatch method to stack. */
    rc = modlua_push_dispatcher(ib, state, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Cannot push modlua.dispatch_handler to stack.");
        return rc;
    }

    /* Push Lua handler onto the table. */
    rc = modlua_push_lua_handler(ib, modlua_modules, state, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Cannot push modlua state handler to stack.");
        return rc;
    }

    lua_pushlightuserdata(L, ib);
    lua_pushlightuserdata(L, modlua_modules->module);
    lua_pushinteger(L, state);
    lua_pushlightuserdata(L, ctx);

    /* Push connection. */
    if (conn != NULL) {
        lua_pushlightuserdata(L, conn);
    }
    else {
        lua_pushnil(L);
    }

    /* Push transaction. */
    if (tx != NULL) {
        lua_pushlightuserdata(L, tx);
    }
    else {
        lua_pushnil(L);
    }

    /* Push configuration context used in conn. */
    lua_pushlightuserdata(L, ctx);

    return IB_OK;
}


/**
 * Check if a module is registered in a Lua stack.
 *
 * This is used to ensure that newly created Lua stacks have been initialized.
 * This is only necessary for callbacks that execute during the
 * configuration phase. After configuration all Lua stacks are destroyed
 * and recreated, thus all modules are necessarily registered after
 * the configuration phase.
 *
 * @param[in] ib Engine. Used for logging.
 * @param[in] L The Lua stack to examine.
 * @param[in] module The module to check for in @a L.
 *
 * @return true of @a module is found registered in the stack @a L.
 *         false, otherwise, including on errors.
 */
static bool modlua_contains_module(
    ib_engine_t      *ib,
    lua_State        *L,
    ib_module_t      *module
)
{
    assert(ib != NULL);
    assert(L != NULL);
    assert(module != NULL);
    assert(lua_checkstack(L, 4));

    int lua_rc;
    bool result;

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        goto cleanup_err;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        goto cleanup_err;
    }

    lua_getfield(L, -1, "has_module"); /* Push has_module. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function has_module is undefined.");
        goto cleanup_err;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(ib, "Module function has_module is not a function.");
        goto cleanup_err;
    }

    /* Push arguments.*/
    lua_pushlightuserdata(L, ib);
    lua_pushlightuserdata(L, module);

    /* Execute the function.*/
    lua_rc = lua_pcall(L, 2, 1, 0);
    if (lua_rc != 0) {
        if (lua_isstring(L, -1)) {
            ib_log_error(
                ib,
                "Failed find registered lua module: %s: %s",
                module->name,
                lua_tostring(L, -1));
        }
        else {
            ib_log_error(
                ib,
                "Failed find registered lua module: %s.",
                module->name);
        }
        goto cleanup_err;
    }

    if (!lua_isboolean(L, -1)) {
        ib_log_error(ib, "modlua.has_module return an non-boolean value.");
        goto cleanup_err;
    }

    result = lua_toboolean(L, -1) != 0;
    lua_pop(L, lua_gettop(L));
    return result;

cleanup_err:
    lua_pop(L, lua_gettop(L));
    return false;
}

/**
 * Callback for logevents.
 *
 * @param[in] ib The engine.
 * @param[in] tx The transaction this event is associated with.
 * @param[in] logevent The @ref ib_logevent_t being created.
 * @param[in] cbdata Callback data of type @ref modlua_modules_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on failure.
 *
 * @sa ib_engine_notify_logevent_register()
 * @sa ib_engine_notify_logevent()
 */
static ib_status_t modlua_logevent(
    ib_engine_t *ib,
    ib_tx_t     *tx,
    ib_logevent_t *logevent,
    void          *cbdata
)
{
    assert(ib != NULL);
    assert(tx != NULL);
    assert(tx->ctx != NULL);
    assert(logevent != NULL);
    assert(cbdata != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    lua_State        *L              = NULL;
    modlua_cfg_t     *cfg            = NULL;
    modlua_runtime_t *runtime        = NULL;
    modlua_modules_t *modlua_modules = (modlua_modules_t *)cbdata;

    assert(modlua_modules->modlua != NULL);
    assert(modlua_modules->module != NULL);

    rc = ib_context_module_config(tx->ctx, modlua_modules->modlua, &cfg);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to retrieve module configuration.");
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get a Lua runtime resource.");
        return rc;
    }

    L = runtime->L;

    if(!lua_checkstack(L, 4)) {
        ib_log_error(
            ib,
            "Lua stack does not have room to execute logevent handlers."
        );
        return IB_EOTHER;
    }

    /* Conditionally reload the main module context, if necessary. */
    if (! modlua_contains_module(ib, L, modlua_modules->module)) {
        rc = modlua_reload_ctx_main(ib, modlua_modules->modlua, L);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to configure Lua stack.");
            goto exit;
        }
    }

    rc = modlua_reload_ctx_except_main(ib, modlua_modules->modlua, tx->ctx, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to configure Lua stack.");
        goto exit;
    }

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        return IB_EINVAL;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    /* Push dispatch_module func. */
    lua_getfield(L, -1, "dispatch_module_logevent");
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function dispatch_module is undefined.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(
            ib,
            "Module function dispatch_module_logevent is not a function."
        );
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    /* Replace the modlua table by replacing it with dispatch_handler. */
    lua_replace(L, -2);

    /* Push the log handler. */
    rc = modlua_push_lua_handler_logevents(ib, modlua_modules, L);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Push the arguments to the handler. */
    lua_pushlightuserdata(L, ib);
    lua_pushlightuserdata(L, tx);
    lua_pushlightuserdata(L, tx->ctx);
    lua_pushlightuserdata(L, modlua_modules->module);
    lua_pushlightuserdata(L, logevent);
    rc = ib_lua_pcall(ib, L, 6, 1, 0);
    if (rc != IB_OK) {
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failure while returning Lua runtime.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a null state into a Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] state The state.
 * @param[in] cbdata A pointer to a modlua_modules_t with the lua module
 *            and the user's lua-defined module struct in it.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static
ib_status_t modlua_null(
    ib_engine_t *ib,
    ib_state_t   state,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(cbdata != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    lua_State        *L              = NULL;
    ib_context_t     *ctx            = ib_context_main(ib);
    modlua_cfg_t     *cfg            = NULL;
    modlua_runtime_t *runtime        = NULL;
    modlua_modules_t *modlua_modules = (modlua_modules_t *)cbdata;

    assert(modlua_modules->modlua != NULL);
    assert(modlua_modules->module != NULL);
    assert(modlua_modules->module->name != NULL);

    rc = ib_context_module_config(ctx, modlua_modules->modlua, &cfg);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to retrieve module configuration.");
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to get a Lua runtime resource.");
        return rc;
    }

    L = runtime->L;

    /* Conditionally reload the main module context, if necessary. */
    if (! modlua_contains_module(ib, L, modlua_modules->module)) {
        rc = modlua_reload_ctx_main(ib, modlua_modules->modlua, L);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to configure Lua stack.");
            goto exit;
        }
    }

    rc = modlua_reload_ctx_except_main(ib, modlua_modules->modlua, ctx, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to configure Lua stack.");
        goto exit;
    }

    rc = modlua_callback_setup(ib, state, NULL, NULL, runtime, modlua_modules);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failure while setting up arguments for callback.");
        goto exit;
    }

    rc = ib_lua_pcall(ib, L, 8, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback handler for module %s.",
            modlua_modules->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failure while returning Lua runtime.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a connection state into a Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] conn Connection.
 * @param[in] state The state.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_conn(
    ib_engine_t *ib,
    ib_conn_t *conn,
    ib_state_t state,
    void *cbdata)
{
    assert(ib != NULL);
    assert(conn != NULL);
    assert(cbdata != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(conn->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, NULL, conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */

    rc = ib_lua_pcall(ib, runtime->L, 8, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback connection handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a transaction state into a Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] tx Transaction.
 * @param[in] state The state.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_tx(
    ib_engine_t *ib,
    ib_tx_t *tx,
    ib_state_t state,
    void *cbdata)
{
    assert(ib       != NULL);
    assert(tx       != NULL);
    assert(tx->ctx  != NULL);
    assert(tx->conn != NULL);
    assert(cbdata   != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(tx->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, tx, tx->conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */

    rc = ib_lua_pcall(ib, runtime->L, 8, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback tx handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a transaction data state into a Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] data Transaction data.
 * @param[in] data_length Transaction data length.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static
ib_status_t modlua_txdata(
    ib_engine_t *ib,
    ib_tx_t     *tx,
    ib_state_t   state,
    const char  *data,
    size_t       data_length,
    void        *cbdata
)
{
    assert(ib       != NULL);
    assert(tx       != NULL);
    assert(tx->ctx  != NULL);
    assert(tx->conn != NULL);
    assert(cbdata   != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(tx->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, tx, tx->conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */
    assert(lua_checkstack(runtime->L, 2));
    lua_pushlightuserdata(runtime->L, (char *)data);
    lua_pushinteger(runtime->L, data_length);

    rc = ib_lua_pcall(ib, runtime->L, 10, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to releases Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a header callback hook.
 *
 * @param[in] ib IronBee engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] header Parsed header data.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_header(
    ib_engine_t *ib,
    ib_tx_t *tx,
    ib_state_t state,
    ib_parsed_header_t *header,
    void *cbdata)
{
    assert(ib       != NULL);
    assert(tx       != NULL);
    assert(tx->ctx  != NULL);
    assert(tx->conn != NULL);
    assert(cbdata   != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(tx->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, tx, tx->conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */
    assert(lua_checkstack(runtime->L, 1));
    lua_pushlightuserdata(runtime->L, header);

    rc = ib_lua_pcall(ib, runtime->L, 9, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback header handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a request line callback hook.
 *
 * @param[in] ib IronBee engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] line Parsed request line.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_reqline(
    ib_engine_t *ib,
    ib_tx_t *tx,
    ib_state_t state,
    ib_parsed_req_line_t *line,
    void *cbdata)
{
    assert(ib       != NULL);
    assert(tx       != NULL);
    assert(tx->ctx  != NULL);
    assert(tx->conn != NULL);
    assert(cbdata   != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(tx->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, tx, tx->conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */
    assert(lua_checkstack(runtime->L, 1));
    lua_pushlightuserdata(runtime->L, line);

    rc = ib_lua_pcall(ib, runtime->L, 9, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback reqline handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a response line callback hook.
 *
 * @param[in] ib IronBee engine.
 * @param[in] tx Transaction.
 * @param[in] state State.
 * @param[in] line The parsed response line.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_respline(
    ib_engine_t *ib,
    ib_tx_t *tx,
    ib_state_t state,
    ib_parsed_resp_line_t *line,
    void *cbdata)
{
    assert(ib);
    assert(tx);
    assert(tx->conn);
    assert(cbdata != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    modlua_runtime_t *runtime;
    modlua_cfg_t     *cfg;

    modlua_modules_t *mod_cbdata = (modlua_modules_t *)cbdata;

    rc = ib_context_module_config(tx->ctx, mod_cbdata->modlua, &cfg);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_callback_setup(ib, state, tx, tx->conn, runtime, mod_cbdata);
    if (rc != IB_OK) {
        goto exit;
    }

    /* Custom table setup */
    assert(lua_checkstack(runtime->L, 1));
    lua_pushlightuserdata(runtime->L, line);

    rc = ib_lua_pcall(ib, runtime->L, 9, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback response handler for module %s.",
            mod_cbdata->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Dispatch a context state into a Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] ctx Context.
 * @param[in] state State.
 * @param[in] cbdata A modlua_modules_t containing the lua module and the
 *            user's lua-defined  module.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_ctx(
    ib_engine_t *ib,
    ib_context_t *ctx,
    ib_state_t state,
    void *cbdata)
{
    assert(ib != NULL);
    assert(ctx != NULL);
    assert(cbdata != NULL);

    ib_status_t       rc;
    ib_status_t       rc2;
    lua_State        *L;
    modlua_cfg_t     *cfg = NULL;
    modlua_runtime_t *runtime;

    modlua_modules_t *modlua_modules = (modlua_modules_t *)cbdata;
    assert(modlua_modules != NULL);
    assert(modlua_modules->modlua != NULL);
    assert(modlua_modules->module != NULL);

    rc = ib_context_module_config(ctx, modlua_modules->modlua, &cfg);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to retrieve module configuration.");
        return rc;
    }

    rc = modlua_acquirestate(ib, cfg, &runtime);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to acquire Lua runtime.");
        return rc;
    }
    L = runtime->L;

    /* Conditionally reload the main module context, if necessary. */
    if (! modlua_contains_module(ib, L, modlua_modules->module)) {
        rc = modlua_reload_ctx_main(ib, modlua_modules->modlua, L);
        if (rc != IB_OK) {
            ib_log_error(ib, "Failed to configure Lua stack.");
            goto exit;
        }
    }

    rc = modlua_reload_ctx_except_main(ib, modlua_modules->modlua, ctx, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to configure Lua stack.");
        goto exit;
    }

    rc = modlua_callback_setup(ib, state, NULL, NULL, runtime, modlua_modules);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failure while setting up arguments for callback.");
        goto exit;
    }

    rc = ib_lua_pcall(ib, L, 8, 1, 0);
    if (rc != IB_OK) {
        ib_log_error(
            ib,
            "Failure while executing callback context handler for module %s.",
            modlua_modules->module->name
        );
        goto exit;
    }

exit:
    rc2 = modlua_releasestate(ib, cfg, runtime);
    if (rc2 != IB_OK) {
        ib_log_error(ib, "Failed to release Lua stack back to resource pool.");
        if (rc == IB_OK) {
            return rc2;
        }
    }

    return rc;
}

/**
 * Called by modlua_module_load to wire the callbacks in @a ib.
 *
 * @param[in] ib IronBee engine.
 * @param[in] modlua The Lua module. Not the user's Lua-implemented module.
 * @param[in] file The file we are loading.
 * @param[in] module The Lua-implemented registered module structure.
 * @param[in,out] L The lua context that @a file will be loaded into as
 *                @a module.
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on allocation errors.
 *   - IB_EOTHER on unexpected errors.
 */
static ib_status_t modlua_module_load_wire_callbacks(
    ib_engine_t *ib,
    ib_module_t *modlua,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
{

    assert(ib != NULL);
    assert(modlua != NULL);
    assert(file != NULL);
    assert(module != NULL);
    assert(L != NULL);

    ib_status_t rc;
    ib_mm_t mm;
    modlua_modules_t *ibmod_modules_cbdata = NULL;

    mm = ib_engine_mm_main_get(ib);
    if (ib_mm_is_null(mm)) {
        ib_log_error(
            ib,
            "Failed to fetch main engine memory pool for Lua module: %s",
            file);
        return IB_EOTHER;
    }

    ibmod_modules_cbdata =
        ib_mm_calloc(mm, 1, sizeof(*ibmod_modules_cbdata));
    if (ibmod_modules_cbdata == NULL) {
        ib_log_error(ib, "Failed to allocate callback data.");
        return IB_EALLOC;
    }
    ibmod_modules_cbdata->modlua = modlua;
    ibmod_modules_cbdata->module = module;

    rc = modlua_has_callback_logevents(ib, ibmod_modules_cbdata, L);
    if (rc == IB_OK) {
        ib_engine_notify_logevent_register(
            ib,
            modlua_logevent,
            ibmod_modules_cbdata
        );
        if (rc != IB_OK) {
            ib_log_error(
                ib,
                "Failed to register logevent callback for module %s.",
                file
            );
            return rc;
        }
    }

    for (ib_state_t state = 0; state < IB_STATE_NUM; ++state) {

        rc = module_has_callback(ib, ibmod_modules_cbdata, state, L);
        if (rc == IB_OK) {
            switch(ib_state_hook_type(state)) {
                case IB_STATE_HOOK_NULL:
                    rc = ib_hook_null_register(
                        ib,
                        state,
                        modlua_null,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_INVALID:
                    ib_log_error(ib, "Invalid hook: %d", state);
                    break;
                case IB_STATE_HOOK_CTX:
                    rc = ib_hook_context_register(
                        ib,
                        state,
                        modlua_ctx,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_CONN:
                    rc = ib_hook_conn_register(
                        ib,
                        state,
                        modlua_conn,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_TX:
                    rc = ib_hook_tx_register(
                        ib,
                        state,
                        modlua_tx,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_TXDATA:
                    rc = ib_hook_txdata_register(
                        ib,
                        state,
                        modlua_txdata,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_REQLINE:
                    rc = ib_hook_parsed_req_line_register(
                        ib,
                        state,
                        modlua_reqline,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_RESPLINE:
                    rc = ib_hook_parsed_resp_line_register(
                        ib,
                        state,
                        modlua_respline,
                        ibmod_modules_cbdata);
                    break;
                case IB_STATE_HOOK_HEADER:
                    rc = ib_hook_parsed_header_data_register(
                        ib,
                        state,
                        modlua_header,
                        ibmod_modules_cbdata);
                    break;
            }
        }
        if ((rc != IB_OK) && (rc != IB_ENOENT)) {
            ib_log_error(ib,
                         "Failed to register hook: %s",
                         ib_status_to_string(rc));
            return rc;
        }
    }

    return IB_OK;
}

/**
 * Evaluate the Lua stack and report errors about directive processing.
 *
 * @param[in] L Lua state.
 * @param[in] ib IronBee engine.
 * @param[in] module The Lua module structure.
 * @param[in] name The function. This is used for logging only.
 * @param[in] args_in The number of arguments to the Lua function being called.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC If Lua interpretation fails with an LUA_ERRMEM error.
 *   - IB_EINVAL on all other failures.
 */
static ib_status_t modlua_config_cb_eval(
    lua_State   *L,
    ib_engine_t *ib,
    ib_module_t *module,
    const char  *name,
    int          args_in
)
{
    ib_status_t rc;
    int lua_rc = lua_pcall(L, args_in, 1, 0);
    switch(lua_rc) {
        case 0:
            /* NOP */
            break;
        case LUA_ERRRUN:
            ib_log_error(
                ib,
                "Error processing call for module %s: %s",
                module->name,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            return IB_EINVAL;
        case LUA_ERRMEM:
            ib_log_error(
                ib,
                "Failed to allocate memory processing call for %s",
                module->name);
            return IB_EALLOC;
        case LUA_ERRERR:
            ib_log_error(
                ib,
                "Failed to fetch error message during call for %s",
                module->name);
            return IB_EINVAL;
#if LUA_VERSION_NUM > 501
        /* If LUA_ERRGCMM is defined, include a custom error for it as well.
          This was introduced in Lua 5.2. */
        case LUA_ERRGCMM:
            ib_log_error(
                ib,
                "Garbage collection error during call for %s.",
                module->name);
            return IB_EINVAL;
#endif
        default:
            ib_log_error(
                ib,
                "Unexpected error(%d) during call %s for %s: %s",
                lua_rc,
                name,
                module->name,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            return IB_EINVAL;
    }

    if (!lua_isnumber(L, -1)) {
        ib_log_error(ib, "Directive handler did not return integer.");
        rc = IB_EINVAL;
    }
    else {
        rc = lua_tonumber(L, -1);
    }

    lua_pop(L, 1);

    return rc;
}

/**
 * Callback to dispatch Block-End configuration states to Lua.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Directive name for the block that is being closed.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 *   - IB_OK on success.
 *   - IB_EALLOC on memory errors in the Lua VM.
 *   - IB_EINVAL if lua evaluation fails.
 */
static ib_status_t modlua_config_cb_blkend(
    ib_cfgparser_t *cp,
    const char     *name,
    void           *cbdata
)
{
    assert(cp != NULL);
    assert(cp->ib != NULL);
    assert(name != NULL);

    ib_status_t   rc;
    ib_context_t *ctx;
    ib_engine_t  *ib                       = cp->ib;
    ib_module_t  *module                   = NULL;
    lua_State    *L                        = NULL;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 6));
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_blkend");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    lua_pushstring(L, name);

    rc = modlua_config_cb_eval(L, ib, module, name, 4);
    return rc;
}

/**
 * Lua configuration callback.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] onoff On or off setting.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_onoff(
    ib_cfgparser_t *cp,
    const char     *name,
    int             onoff,
    void           *cbdata
)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);
    assert(cbdata != NULL);

    ib_status_t          rc;
    lua_State           *L;
    ib_context_t        *ctx;
    ib_module_t         *module            = NULL;
    ib_engine_t         *ib                = cp->ib;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 7));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    assert(name != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_onoff");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    lua_pushstring(L, name);
    lua_pushinteger(L, onoff);

    rc = modlua_config_cb_eval(L, ib, module, name, 5);
    return rc;
}
/**
 * Lua param configuration callback.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] p1 The only parameter.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_param1(
    ib_cfgparser_t *cp,
    const char     *name,
    const char     *p1,
    void           *cbdata
)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);
    assert(p1     != NULL);

    ib_status_t   rc;
    lua_State    *L;
    ib_module_t  *module = NULL;
    ib_engine_t  *ib = cp->ib;
    ib_context_t *ctx;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 7));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_param1");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    assert(name != NULL);
    assert(p1 != NULL);
    lua_pushstring(L, name);
    lua_pushstring(L, p1);

    rc = modlua_config_cb_eval(L, ib, module, name, 5);
    return rc;
}

/**
 * Lua param 2 configuration callback.

 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] p1 The first parameter.
 * @param[in] p2 The second parameter.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_param2(
    ib_cfgparser_t *cp,
    const char     *name,
    const char     *p1,
    const char     *p2,
    void           *cbdata
)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);
    assert(p1     != NULL);
    assert(p2     != NULL);

    ib_status_t          rc;
    lua_State           *L;
    ib_context_t        *ctx;
    ib_module_t         *module            = NULL;
    ib_engine_t         *ib                = cp->ib;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 8));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_param2");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    assert(name != NULL);
    assert(p1 != NULL);
    assert(p2 != NULL);
    lua_pushstring(L, name);
    lua_pushstring(L, p1);
    lua_pushstring(L, p2);

    rc = modlua_config_cb_eval(L, ib, module, name, 6);
    return rc;
}
/**
 * Lua list configuration callback.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] list List of values.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_list(
    ib_cfgparser_t  *cp,
    const char      *name,
    const ib_list_t *list,
    void            *cbdata
)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);
    assert(list   != NULL);

    ib_status_t          rc;
    lua_State           *L;
    ib_context_t        *ctx;
    ib_module_t         *module            = NULL;
    ib_engine_t         *ib                = cp->ib;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 7));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_list");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    assert(name != NULL);
    assert(list != NULL);
    lua_pushstring(L, name);
    lua_pushlightuserdata(L, (void *)list);

    rc = modlua_config_cb_eval(L, ib, module, name, 5);
    return rc;
}

/**
 * Lua flag configuration callback.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] mask The flags we are setting.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_opflags(
    ib_cfgparser_t *cp,
    const char     *name,
    ib_flags_t      mask,
    void           *cbdata)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);

    ib_status_t          rc;
    lua_State           *L;
    ib_context_t        *ctx;
    ib_module_t         *module            = NULL;
    ib_engine_t         *ib                = cp->ib;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 7));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_opflags");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    assert(name != NULL);
    lua_pushstring(L, name);
    lua_pushinteger(L, mask);

    rc = modlua_config_cb_eval(L, ib, module, name, 5);
    return rc;
}

/**
 * Lua block configuration callback.
 *
 * @param[in] cp Configuration parser.
 * @param[in] name Configuration directive name.
 * @param[in] p1 The block name that we are exiting.
 * @param[in] cbdata Callback data. A @ref modlua_cfg_cbdata_t.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_config_cb_sblk1(
    ib_cfgparser_t *cp,
    const char     *name,
    const char     *p1,
    void           *cbdata
)
{
    assert(cp     != NULL);
    assert(cp->ib != NULL);
    assert(name   != NULL);
    assert(p1     != NULL);

    ib_status_t          rc;
    lua_State           *L;
    ib_context_t        *ctx;
    ib_module_t         *module            = NULL;
    ib_engine_t         *ib                = cp->ib;
    modlua_cfg_cbdata_t *modlua_cfg_cbdata = (modlua_cfg_cbdata_t *)cbdata;

    module = modlua_cfg_cbdata->module;
    L      = modlua_cfg_cbdata->L;

    rc = ib_cfgparser_context_current(cp, &ctx);
    if (rc != IB_OK) {
        ib_cfg_log_error(cp, "Failed to retrieve current context.");
        return rc;
    }

    /* Push standard module directive arguments. */
    assert(lua_checkstack(L, 7));
    assert(module != NULL);
    assert(module->ib != NULL);
    assert(ctx != NULL);
    lua_getglobal(L, "modlua");
    lua_getfield(L, -1, "modlua_config_cb_sblk1");
    lua_replace(L, -2); /* Effectively remove then modlua table. */
    lua_pushlightuserdata(L, cp);
    lua_pushinteger(L, module->idx);
    lua_pushlightuserdata(L, ctx);

    /* Push config parameters. */
    assert(name != NULL);
    assert(p1 != NULL);
    lua_pushstring(L, name);
    lua_pushstring(L, p1);

    rc = modlua_config_cb_eval(L, ib, module, name, 5);
    return rc;
}

/**
 * Proxy function to ib_config_register_directive callable by Lua.
 *
 * @param[in] L Lua state. This stack should have on it 3 arguments
 *            to this function:
 *            - @c self - The Module API object.
 *            - @c name - The name of the directive to register.
 *            - @c type - The type of directive this is.
 *            - @c strvalmap - Optional string-value table map.
 *
 * @returns The number of values returned on the @a L. This
 *          conforms to the Lua C dispatch contract.
 */
static int modlua_config_register_directive(lua_State *L)
{
    assert(L != NULL);

    ib_status_t          rc    = IB_OK;
    const char          *rcmsg = "Success.";
    modlua_cfg_cbdata_t *modlua_cfg_cbdata;  /* Callback data. */
    ib_void_fn_t         cfg_cb;             /* Configuration callback. */

    /* Variables pulled from the Lua state. */
    int           args = lua_gettop(L); /* Num args passed on Lua stack. */
    ib_engine_t  *ib;                   /* Fetched from self on *L */
    const char   *name;                 /* Name of the directive. 2nd arg. */
    ib_dirtype_t  type;                 /* Type of directive. 3rd arg. */
    ib_strval_t  *strvalmap;            /* Opt. 4th arg. Must be converted. */
    ib_module_t  *module;               /* Lua-defined module. */

    /* We choose assert here because if this value is incorrect,
     * then the lua module code (lua.c and ironbee/module.lua) are
     * inconsistent with each other. */
    assert(
        (args==3 || args==4) &&
        "lua.c and ironbee/module.lua are inconsistent.");
    assert(lua_checkstack(L, args));

    if (lua_istable(L, 0-args)) {

        /* Get IB Engine. */
        lua_getfield(L, 0-args, "ib_engine");
        ib = (ib_engine_t *)lua_topointer(L, -1);
        assert(ib != NULL);
        /* Pop the engine pointer we just fetched. */
        lua_pop(L, 1);

        /* Get Module. */
        lua_getfield(L, 0-args, "ib_module");
        module = (ib_module_t *)lua_topointer(L, -1);
        assert(module != NULL);
        /* Pop the module pointer we just fetched. */
        lua_pop(L, 1);
    }
    else {
        rc = IB_EINVAL;
        rcmsg = "1st argument is not self table.";
        goto exit;
    }

    if (lua_isstring(L, 1-args)) {
        name = lua_tostring(L, 1-args);
    }
    else {
        rc = IB_EINVAL;
        rcmsg = "2nd argument is not a string.";
        goto exit;
    }

    if (lua_isnumber(L, 2-args)) {
        type = lua_tonumber(L, 2-args);
    }
    else {
        rc = IB_EINVAL;
        rcmsg = "3rd argument is not a number.";
        goto exit;
    }

    if (args==4) {
        if (lua_istable(L, 3-args)) {
            int varmapsz = 0;

            /* Count the size of the table. table.maxn is not sufficient. */
            while (lua_next(L, 3-args)) { /* Push string, int onto stack. */
                ++varmapsz;
                lua_pop(L, 1); /* Pop off value. Leave key. */
            }

            if (varmapsz > 0) {
                ib_mm_t mm = ib_engine_mm_config_get(ib);

                /* Allocate space for strvalmap. */
                strvalmap = ib_mm_alloc(
                    mm,
                    sizeof(*strvalmap) * (varmapsz + 1));
                if (strvalmap == NULL) {
                    rc = IB_EALLOC;
                    rcmsg = "Cannot allocate strval map.";
                    goto exit;
                }

                /* Build map. */
                for (int i = 0; lua_next(L, 3-args); ++i) {
                    strvalmap[i].str =
                        ib_mm_strdup(mm, lua_tostring(L, -2));
                    strvalmap[i].val =
                        lua_tointeger(L, -1);
                    lua_pop(L, 1); /* Pop off value. Leave key. */
                }

                /* Null terminate the list. */
                strvalmap[varmapsz].str = NULL;
                strvalmap[varmapsz].val = 0;
            }
            else {
                strvalmap = NULL;
            }
        }
        else {
            rc = IB_EINVAL;
            rcmsg = "4th argument is not a table.";
            goto exit;
        }
    }
    else {
        strvalmap = NULL;
    }

    /* Assign the cfg_cb pointer to hand the callback. */
    switch (type) {
        case IB_DIRTYPE_ONOFF:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_onoff;
            break;
        case IB_DIRTYPE_PARAM1:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_param1;
            break;
        case IB_DIRTYPE_PARAM2:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_param2;
            break;
        case IB_DIRTYPE_LIST:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_list;
            break;
        case IB_DIRTYPE_OPFLAGS:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_opflags;
            break;
        case IB_DIRTYPE_SBLK1:
            cfg_cb = (ib_void_fn_t) &modlua_config_cb_sblk1;
            break;
        default:
            rc = IB_EINVAL;
            rcmsg = "Invalid configuration type.";
            goto exit;
    }

    modlua_cfg_cbdata =
        ib_mm_alloc(
            ib_engine_mm_config_get(ib),
            sizeof(*modlua_cfg_cbdata));
    if (modlua_cfg_cbdata == NULL) {
        rc = IB_EALLOC;
        rcmsg = "Failed to allocate callback data.";
        goto exit;
    }

    modlua_cfg_cbdata->module = module;
    modlua_cfg_cbdata->L = L;

    rc = ib_config_register_directive(
        ib,
        name,
        type,
        cfg_cb,
        &modlua_config_cb_blkend,
        modlua_cfg_cbdata, /* cfg_cb callback data. */
        modlua_cfg_cbdata, /* modlua_config_cb_blkend callback data. */
        strvalmap);
    if (rc != IB_OK) {
        rcmsg = "Failed to register directive.";
        goto exit;
    }

exit:
    lua_pop(L, lua_gettop(L));
    lua_pushinteger(L, rc);
    lua_pushstring(L, rcmsg);

    return lua_gettop(L);
}

/**
 * Setup the call stack for the Lua function modlua.load_module().
 *
 * This function will push onto the @a L stack:
 *
 * @code
 * +-------------------------------------------+
 * | load_module                               |
 * | ib                                        |
 * | ib_module                                 |
 * | module name (file name)                   |
 * | module index                              |
 * | modlua_config_register_directive (or nil) |
 * | module script                             |
 * +-------------------------------------------+
 * @endcode
 *
 * @param[in] ib IronBee engine.
 * @param[in] register_directives If true, modlua_config_register_directive
 *            will be pushed onto the stack, causing
 *            Lua directives to be added to the engine.
 *            If this function is called at config time, this should be true.
 *            If this function is called at module re-load time, this
 *            should be false, meaning @a ib already has all the
 *            directives defined.
 * @param[in] file The Lua file to load.
 * @param[in] module The module structure being loaded.
 * @param[in] L The Lua stack and environment being loaded into.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error. Errors are logged in this function.
 */
static ib_status_t modlua_load_module_push_stack(
    ib_engine_t *ib,
    bool         register_directives,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
NONNULL_ATTRIBUTE(1, 3, 4, 5);

static ib_status_t modlua_load_module_push_stack(
    ib_engine_t *ib,
    bool         register_directives,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
{
    assert(ib     != NULL);
    assert(file   != NULL);
    assert(module != NULL);
    assert(L      != NULL);
    assert(lua_checkstack(L, 8));

    int lua_rc;

    lua_getglobal(L, "modlua"); /* Get the package. */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module modlua is undefined.");
        return IB_EINVAL;
    }
    if (! lua_istable(L, -1)) {
        ib_log_error(ib, "Module modlua is not a table/module.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_getfield(L, -1, "load_module"); /* Push load_module */
    if (lua_isnil(L, -1)) {
        ib_log_error(ib, "Module function load_module is undefined.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }
    if (! lua_isfunction(L, -1)) {
        ib_log_error(ib, "Module function load_module is not a function.");
        lua_pop(L, 1); /* Pop modlua global off stack. */
        return IB_EINVAL;
    }

    lua_pushlightuserdata(L, ib); /* Push ib engine. */
    lua_pushlightuserdata(L, module); /* Push module. */
    lua_pushstring(L, module->name);
    lua_pushinteger(L, module->idx);

    if (register_directives) {
        lua_pushcfunction(L, &modlua_config_register_directive);
    }
    else {
        lua_pushnil(L);
    }

    lua_rc = luaL_loadfile(L, file);
    switch(lua_rc) {
        case 0:
            /* NOP */
            break;
        case LUA_ERRSYNTAX:
            ib_log_error(
                ib,
                "Syntax error evaluating %s: %s",
                file,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
        case LUA_ERRMEM:
            ib_log_error(
                ib,
                "Failed to allocate memory during load of %s",
                file);
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
        case LUA_ERRFILE:
            ib_log_error(
                ib,
                "Failed to load %s",
                file);
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
        default:
            ib_log_error(
                ib,
                "Unexpected error(%d) during evaluation of %s: %s",
                lua_rc,
                file,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
    }

    return IB_OK;
}

/**
 * After calling modlua_load_module_push_stack() call this to evaluate it.
 *
 * The function modlua_load_module_push_stack() sets up the call stack
 * to load a Lua module. This call evaluates the prepared stack
 * and reports any errors.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error. Errors are logged in this function.
 */
static ib_status_t modlua_load_module_eval(
    ib_engine_t *ib,
    const char  *file,
    lua_State    *L
)
{
    assert(ib != NULL);
    assert(file != NULL);
    assert(L != NULL);

    int lua_rc = lua_pcall(L, 6, 1, 0);
    switch(lua_rc) {
        case 0:
            /* NOP */
            break;
        case LUA_ERRRUN:
            ib_log_error(
                ib,
                "Error loading module %s: %s",
                file,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
        case LUA_ERRMEM:
            ib_log_error(
                ib,
                "Failed to allocate memory during module load of %s",
                file);
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
        case LUA_ERRERR:
            ib_log_error(
                ib,
                "Error fetching error message during module load of %s",
                file);
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
#if LUA_VERSION_NUM > 501
        /* If LUA_ERRGCMM is defined, include a custom error for it as well.
          This was introduced in Lua 5.2. */
        case LUA_ERRGCMM:
            ib_log_error(
                ib,
                "Garbage collection error during module load of %s.",
                file);
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
#endif
        default:
            ib_log_error(
                ib,
                "Unexpected error(%d) during evaluation of %s: %s",
                lua_rc,
                file,
                lua_tostring(L, -1));
            lua_pop(L, 1); /* Get error string off of the stack. */
            lua_pop(L, 1); /* Pop modlua global off stack. */
            return IB_EINVAL;
    }

    lua_pop(L, 1); /* Pop modlua global off stack. */

    return IB_OK;
}

/**
 * Called by modlua_module_load to load the lua script into the Lua runtime.
 *
 * This function will register configuration directives causing
 * Lua module configuration directives to be registered with @a ib.
 *
 * @param[in] ib IronBee engine.
 * @param[in] file The file we are loading.
 * @param[in] module The registered module structure.
 * @param[in,out] L The lua context that @a file will be loaded into as
 *                @a module.
 * @returns
 *   - IB_OK on success.
 *   - Other on error. Errors are logged by this function.
 */
static ib_status_t modlua_module_config_lua(
    ib_engine_t *ib,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
NONNULL_ATTRIBUTE(1, 2, 3, 4);

static ib_status_t modlua_module_config_lua(
    ib_engine_t *ib,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
{
    assert(ib     != NULL);
    assert(file   != NULL);
    assert(module != NULL);
    assert(L      != NULL);

    ib_status_t rc;

    /* Load the stack with the register directives function. */
    rc = modlua_load_module_push_stack(ib, true, file, module, L);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_load_module_eval(ib, file, L);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}


ib_status_t modlua_module_load_lua(
    ib_engine_t *ib,
    const char  *file,
    ib_module_t *module,
    lua_State   *L
)
{
    assert(ib     != NULL);
    assert(file   != NULL);
    assert(module != NULL);
    assert(L      != NULL);

    ib_status_t rc;

    /* Load the stack without the register directives function. */
    rc = modlua_load_module_push_stack(ib, false, file, module, L);
    if (rc != IB_OK) {
        return rc;
    }

    rc = modlua_load_module_eval(ib, file, L);
    if (rc != IB_OK) {
        return rc;
    }

    return IB_OK;
}


/**
 * Initialize a dynamically created Lua module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] module The created module structure.
 * @param[in] cbdata Other variables required for proper initialization
 *            not provided for in the module init api.
 *
 * @returns
 * - IB_OK On success.
 * - Other on error.
 */
static ib_status_t modlua_luamod_init(
    ib_engine_t *ib,
    ib_module_t *module,
    void        *cbdata
)
{
    assert(ib != NULL);
    assert(module != NULL);
    assert(cbdata != NULL);

    modlua_luamod_init_t *cfg = (modlua_luamod_init_t *)cbdata;

    /* Validate the passed along configuration. */
    assert(cfg->modlua != NULL);
    assert(cfg->modlua_cfg != NULL);
    assert(cfg->modlua_cfg->L != NULL);
    assert(cfg->file != NULL);

    ib_module_t          *modlua     = cfg->modlua;
    modlua_cfg_t         *modlua_cfg = cfg->modlua_cfg;
    lua_State            *L          = cfg->modlua_cfg->L;
    const char           *file       = cfg->file;
    ib_status_t           rc;

    /* Load the modules into the main Lua stack. Also register directives. */
    rc = modlua_module_config_lua(ib, file, module, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to load lua modules: %s", module->name);
        return rc;
    }

    /* If the previous succeeds, record that we should reload it on each tx. */
    rc = modlua_record_reload(
        ib,
        modlua_cfg,
        MODLUA_RELOAD_MODULE,
        module,
        NULL,
        file);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to record module file name to reload.");
        return rc;
    }

    /* Write up the callbacks. */
    rc = modlua_module_load_wire_callbacks(ib, modlua, file, module, L);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed register lua callbacks for module : %s", module->name);
        return rc;
    }

    return IB_OK;
}

/**
 * Load a Lua-defined module.
 *
 * @param[in] ib IronBee engine.
 * @param[in] modlua The ibmod_lua module structure.
 * @param[in] module_name Name of the *lua* module (not the ibmod_lua module).
 * @param[in] file The file that the user's lua-defined module resides in.
 * @param[in] cfg The module configuration for @a modlua.
 *
 * @returns
 * - IB_OK On success.
 * - IB_ENOENT If the file cannot be stat'ed.
 * - Other on error.
 */
ib_status_t modlua_module_load(
    ib_engine_t  *ib,
    ib_module_t  *modlua,
    const char   *module_name,
    const char   *file,
    modlua_cfg_t *cfg
)
{
    assert(ib != NULL);
    assert(modlua != NULL);
    assert(file != NULL);
    assert(cfg != NULL);
    assert(cfg->L != NULL);

    ib_module_t *module;
    ib_status_t  rc;
    ib_mm_t      mm = ib_engine_mm_main_get(ib);
    modlua_luamod_init_t *modlua_luamod_init_cbdata =
        ib_mm_alloc(mm, sizeof(*modlua_luamod_init_cbdata));
    int          sys_rc;
    struct stat  file_stat;

    /* Stat the file to avoid touching files that don't even exist. */
    sys_rc = stat(file, &file_stat);
    if (sys_rc == -1) {
        return IB_ENOENT;
    }

    if (modlua_luamod_init_cbdata == NULL) {
        return IB_EALLOC;
    }

    module_name = ib_mm_strdup(mm, module_name);
    if (module_name == NULL) {
        return IB_EALLOC;
    }

    /* Create the Lua module as if it was a normal module. */
    rc = ib_module_create(&module, ib);
    if (rc != IB_OK) {
        ib_log_error(ib, "Cannot allocate module structure.");
        return rc;
    }

    modlua_luamod_init_cbdata->file       = file;
    modlua_luamod_init_cbdata->modlua     = modlua;
    modlua_luamod_init_cbdata->modlua_cfg = cfg;

    /* Initialize the loaded module. */
    IB_MODULE_INIT_DYNAMIC(
        module,                         /* Module */
        file,                           /* Module code filename */
        NULL,                           /* Module data */
        ib,                             /* Engine */
        module_name,                    /* Module name */
        NULL,                           /* Global config data */
        0,                              /* Global config data length */
        NULL,                           /* Config copier */
        NULL,                           /* Config copier data */
        NULL,                           /* Configuration field map */
        NULL,                           /* Config directive map */
        modlua_luamod_init,             /* Initialize function */
        modlua_luamod_init_cbdata,      /* Callback data */
        NULL,                           /* Finish function */
        NULL                            /* Callback data */
    );

    /* Initialize and register the new lua module with the engine. */
    rc = ib_module_register(module, ib);
    if (rc != IB_OK) {
        ib_log_error(ib, "Failed to initialize / register a lua module.");
        return rc;
    }

    return rc;
}

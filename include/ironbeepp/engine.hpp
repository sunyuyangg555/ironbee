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
 * @brief IronBee++ --- Engine
 *
 * This code is under construction.  Do not use yet.
 *
 * @author Christopher Alfeld <calfeld@qualys.com>
 */

#ifndef __IBPP__ENGINE__
#define __IBPP__ENGINE__

#include <ironbeepp/abi_compatibility.hpp>
#include <ironbeepp/common_semantics.hpp>
#include <ironbeepp/list.hpp>

#include <ironbee/engine.h>
#include <ironbee/engine_state.h>

#include <iostream>

namespace IronBee {

class ConfigurationDirectivesRegistrar;
class ConfigurationParser;
class HooksRegistrar;
class Context;
class ConstContext;
class Notifier;
class Server;
class ConstServer;
class MemoryManager;
class VarConfig;
class ConstVarConfig;

/**
 * Const Engine; equivalent to a const pointer to ib_engine_t.
 *
 * Provides operators ==, !=, <, >, <=, >= and evaluation as a boolean for
 * singularity via CommonSemantics.
 *
 * See Engine for discussion of the engine.
 *
 * @sa Engine
 * @sa ironbeepp
 * @sa ib_engine_t
 * @nosubgrouping
 **/
class ConstEngine :
    public CommonSemantics<ConstEngine>
{
public:
    /**
     * Events in the engine state machine.
     *
     * This enum defines constants representing the states of the engine
     * state machine.  The main use to module writers is that they are passed
     * in to hook callbacks.
     **/
    enum state_e {
        connection_started         = conn_started_state,
        connection_finished        = conn_finished_state,
        connection_opened          = conn_opened_state,
        connection_closed          = conn_closed_state,
        transaction_started        = tx_started_state,
        transaction_process        = tx_process_state,
        transaction_finished       = tx_finished_state,
        handle_context_connection  = handle_context_conn_state,
        handle_connect             = handle_connect_state,
        handle_context_transaction = handle_context_tx_state,
        handle_request_header      = handle_request_header_state,
        handle_request             = handle_request_state,
        handle_response_header     = handle_response_header_state,
        handle_response            = handle_response_state,
        handle_disconnect          = handle_disconnect_state,
        handle_postprocess         = handle_postprocess_state,
        handle_logging             = handle_logging_state,
        request_started            = request_started_state,
        request_header_process     = request_header_process_state,
        request_header_finished    = request_header_finished_state,
        request_header_data        = request_header_data_state,
        request_body_data          = request_body_data_state,
        request_finished           = request_finished_state,
        response_started           = response_started_state,
        response_header_finished   = response_header_finished_state,
        response_header_data       = response_header_data_state,
        response_body_data         = response_body_data_state,
        response_finished          = response_finished_state,
        context_open               = context_open_state,
        context_close              = context_close_state,
        context_destroy            = context_destroy_state,
        engine_shutdown_initiated  = engine_shutdown_initiated_state
    };

    /**
     * Provides human readable version of @a state.
     *
     * @param[in] state State.
     * @returns Human readable string name of @a state.
     **/
    static const char* state_name(state_e state);

    //! C Type.
    typedef const ib_engine_t* ib_type;

    /**
     * Construct singular ConstEngine.
     *
     * All behavior of a singular ConstEngine is undefined except for
     * assignment, copying, comparison, and evaluate-as-bool.
     **/
    ConstEngine();

    /**
     * @name C Interoperability
     * Methods to access underlying C types.
     **/
    ///@{

    //! const ib_engine_t accessor.
    // Intentionally inlined.
    const ib_engine_t* ib() const
    {
        return m_ib;
    }

    //! Return the sensor ID for this engine.
    const char* sensor_id() const
    {
        return ib_engine_sensor_id(ib());
    }

    //! Construct Engine from ib_engine_t.
    explicit
    ConstEngine(const ib_engine_t* ib_engine);

    ///@}

    //! Main context.
    Context main_context() const;

    //! Var Config.
    ConstVarConfig var_config() const;

    //! Server.
    ConstServer server() const;

private:
    const ib_engine_t* m_ib;
};

/**
 * Engine; equivalent to a pointer to ib_engine_t.
 *
 * An Engine can be treated as a ConstEngine.  See @ref ironbeepp for
 * details on IronBee++ object semantics.
 *
 * The IronBee Engine is the central component of IronBee that processes
 * inputs and calls hooks.  It is a complex state machine.  See
 * IronBeeEngineState.
 *
 * This class provides some of the C API functionality.  In particular, it
 * allows module writers to register hooks with the engine and provides
 * logging functionality.
 *
 * @sa ironbeepp
 * @sa IronBeeEngineState
 * @sa ib_engine_t
 * @sa ConstEngine
 * @nosubgrouping
 **/
class Engine :
    public ConstEngine
{
public:
    //! C Type.
    typedef ib_engine_t* ib_type;

    /**
     * Create a new Engine.
     *
     * Result must be destroyed when finished via destroy().  Must also be
     * initialized with initialize() before use.
     *
     * @param[in] server The associated server.
     * @returns Engine
     **/
    static Engine create(Server server);

    /**
     * Remove the constness of a ConstEngine.
     *
     * @warning This is as dangerous as a @c const_cast, use carefully.
     *
     * @param[in] engine ConstEngine to remove const from.
     * @returns Engine pointing to same underlying byte string as @a bs.
     **/
    static Engine remove_const(ConstEngine engine);

    /**
     * Construct singular Engine.
     *
     * All behavior of a singular Engine is undefined except for
     * assignment, copying, comparison, and evaluate-as-bool.
     **/
    Engine();

    /**
     * @name Hooks
     * Methods to register hooks.
     *
     * See IronBeeEngineState for details on the states and transitions.
     **/
    ///@{

    ///@}

    /**
     * @name C Interoperability
     * Methods to access underlying C types.
     **/
    ///@{

    //! const ib_engine_t accessor.
    // Intentionally inlined.
    ib_engine_t* ib() const
    {
        return m_ib;
    }

    //! Construct Engine from ib_engine_t.
    explicit
    Engine(ib_engine_t* ib_engine);

    ///@}

    /**
     * Destroy Engine.
     *
     * Destroy Engine, reclaiming all memory.  Do not use afterwards.
     **/
    void destroy();

    /**
     * Register configuration directives.
     *
     * This method returns a ConfigurationDirectivesRegistrar, a helper class
     * to assist registering configuration directives.  See
     * ConfigurationDirectivesRegistrar for details on how to use it.
     *
     * @sa ConfigurationDirectivesRegistrar
     * @return ConfigurationDirectivesRegistrar
     **/
    ConfigurationDirectivesRegistrar
         register_configuration_directives() const;

    /**
     * Register engine hooks.
     *
     * This methods returns a HooksRegistrar, a helper class to assist
     * registering engine hooks.  See HooksRegistrar for details on how to use
     * it.
     *
     * @sa HooksRegistrar
     * @return HooksRegistrar
     **/
    HooksRegistrar register_hooks() const;

     /**
      * Notify engine of state changes.
      *
      * This methods returns a Notifier which can be used to notify the engine
      * of state changes.  See Notifier for details on how to use it.
      *
      * @sa Notifier
      * @return Notifier
      **/
    Notifier notify() const;

    /**
     * @name Memory Pools
     * Functions to fetch Engine memory pools.
     **/
    ///@{

    /**
     * Main memory manager.
     *
     * This memory manager should be used for memory that needs to live as
     * long as the engine.
     *
     * @returns Memory manager.
     **/
    MemoryManager main_memory_mm() const;

    /**
     * Configuration memory manager.
     *
     * This memory manager should be used for memory involved in
     * configuration.  At present, this memory lives as long as the engine.
     *
     * @returns Memory manager.
     **/
    MemoryManager configuration_memory_mm() const;

    /**
     * Temporary memory manager.
     *
     * This memory manager should be used for temporary storage during
     * configuration.  It is destroyed at the end of configuration and should
     * not be used afterwards.
     *
     * @returns Memory manager.
     **/
    MemoryManager temporary_memory_mm() const;

    //! Var Config.
    VarConfig var_config() const;

    //! Tell engine configuration has started.
    void configuration_started(
        ConfigurationParser configuration_parser
    ) const;

    //! Tell engine configuration is finished.
    void configuration_finished() const;

    //! Rule ownership function.
    typedef boost::function<
        void(
            ConstEngine,
            const ib_rule_t*,
            ConstContext
        )
    > rule_ownership_t;

    /**
     * Register a rule ownership function.
     *
     * Function that can claim rules, preventing them from going to the
     * default rule system.
     *
     * @param[in] name      Name of owner to use in logging.
     * @param[in] ownership Function to ask about ownership.
     **/
    void register_rule_ownership(
        const char*      name,
        rule_ownership_t ownership
    ) const;

    //! Rule injection function.
    typedef boost::function<
        void(
            ConstEngine,
            const ib_rule_exec_t*,
            List<const ib_rule_t*>
        )
    > rule_injection_t;

    /**
     * Register a rule injection function.
     *
     * Function that can inject rules for execution.
     *
     * @param[in] name      Name of owner to use in logging.
     * @param[in] phase     Phase to register for.
     * @param[in] injection Function to ask about injection.
     **/
    void register_rule_injection(
        const char*         name,
        ib_rule_phase_num_t phase,
        rule_injection_t    injection
    ) const;

    //! Block handler function.
    typedef boost::function<
        void(Transaction, ib_block_info_t&)
    > block_handler_t;

    /**
     * Register a block handler.
     *
     * There can be at most one block handler per engine.  The block handler
     * is responsible for determining how to block.
     *
     * @param[in] name    Name to use for logging.
     * @param[in] handler Handler to register.
     **/
    void register_block_handler(
        const char*     name,
        block_handler_t handler
    ) const;

    //! Block pre-block hook.
    typedef boost::function<void(Transaction)> block_pre_hook_t;

    /**
     * Register a pre-block hook.
     *
     * Pre-block hooks are called when a hook is requested, before the block
     * handler is called.  They are allowed to change whether blocking is
     * enabled.
     *
     * @param[in] name Name to use for logging.
     * @param[in] hook Handler to register.
     **/
    void register_block_pre_hook(
        const char*      name,
        block_pre_hook_t hook
    ) const;

    //! Block post-block hook.
    typedef boost::function<
        void(Transaction, const ib_block_info_t&)
    > block_post_hook_t;

    /**
     * Register a post-block hook.
     *
     * Post-block hooks are called after the handler.  The handler and
     * post-block hooks are only called if blocking is enabled.
     *
     * @param[in] name Name to use for logging.
     * @param[in] hook Handler to register.
     **/
    void register_block_post_hook(
        const char*       name,
        block_post_hook_t hook
    ) const;

private:
    ib_engine_t* m_ib;
};

/**
 * Output operator for Engine.
 *
 * Outputs Engine[@e value] to @a o where @e value is replaced with
 * the value of the bytestring.
 *
 * @param[in] o      Ostream to output to.
 * @param[in] engine Engine to output.
 * @return @a o
 **/
std::ostream& operator<<(std::ostream& o, const ConstEngine& engine);

} // IronBee

#endif

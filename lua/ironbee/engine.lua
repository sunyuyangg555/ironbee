-- =========================================================================
-- Licensed to Qualys, Inc. (QUALYS) under one or more
-- contributor license agreements.  See the NOTICE file distributed with
-- this work for additional information regarding copyright ownership.
-- QUALYS licenses this file to You under the Apache License, Version 2.0
-- (the "License"); you may not use this file except in compliance with
-- the License.  You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--
-- =========================================================================
--
-- LogEvent - A security event generated by IronBee rules.
--
-- Author: Sam Baskinger <sbaskinger@qualys.com>
--
-- =========================================================================

local ibutil = require('ironbee/util')
local ffi = require('ffi')
local ironbee = require('ironbee-ffi')

local _M = {}
_M.__index = _M
_M._COPYRIGHT = "Copyright (C) 2010-2013 Qualys, Inc."
_M._DESCRIPTION = "IronBee Lua Engine"
_M._VERSION = "1.0"

_M.new = function(self, ib_engine)
    local o = {}

    -- Store raw C values.
    o.ib_engine = ib_engine

    return setmetatable(o, self)
end

-- Given an ib_field_t*, this will convert the data into a Lua type or
-- nil if the value is not supported.
_M.fieldToLua = function(self, field)

    -- Nil, guard against undefined fields.
    if field == nil then
        return nil
    -- Protect against structures without a type field.
    elseif not ffi.istype("ib_field_t*", field) then
        self:logError(
            "Cdata type  ib_field_t * exepcted. Got %s",
            tostring(field))
        return nil
    -- Number
    elseif field.type == ffi.C.IB_FTYPE_NUM then
        local value = ffi.new("ib_num_t[1]")
        ffi.C.ib_field_value(field, value)
        return tonumber(value[0])

    -- Float Number
    elseif field.type == ffi.C.IB_FTYPE_FLOAT then
        local value = ffi.new("ib_float_t[1]")
        ffi.C.ib_field_value(field, value)
        return tonumber(value[0])

    -- String
    elseif field.type == ffi.C.IB_FTYPE_NULSTR then
        local value = ffi.new("const char*[1]")
        ffi.C.ib_field_value(field, value)
        return ffi.string(value[0])

    -- Byte String
    elseif field.type == ffi.C.IB_FTYPE_BYTESTR then
        local value = ffi.new("const ib_bytestr_t*[1]")
        ffi.C.ib_field_value(field, value)
        return ffi.string(ffi.C.ib_bytestr_const_ptr(value[0]),
                          ffi.C.ib_bytestr_length(value[0]))

    -- Lists
    elseif field.type == ffi.C.IB_FTYPE_LIST then
        local t = {}
        local value = ffi.new("ib_list_t*[1]")
        
        ffi.C.ib_field_value(field, value)
        ibutil.each_list_node(
            value[0],
            function(data)
                t[#t+1] = { ffi.string(data.name, data.nlen),
                            self:fieldToLua(data) }
            end)

        return t

    -- Stream buffers - not handled.
    elseif field.type == ffi.C.IB_FTYPE_SBUFFER then
        return nil

    -- Anything else - not handled.
    else
        return nil
    end
end

-- Log an error.
_M.logError = function(self, msg, ...) 
    self:log(ffi.C.IB_LOG_ERROR, "LuaAPI - [ERROR]", msg, ...)
end

-- Log a warning.
_M.logWarn = function(self, msg, ...) 
    -- Note: Extra space after "INFO " is for text alignment.
    -- It should be there.
    self:log(ffi.C.IB_LOG_WARNING, "LuaAPI - [WARN ]", msg, ...)
end

-- Log an info message.
_M.logInfo = function(self, msg, ...) 
    -- Note: Extra space after "INFO " is for text alignment.
    -- It should be there.
    self:log(ffi.C.IB_LOG_INFO, "LuaAPI - [INFO ]", msg, ...)
end

-- Log debug information at level 3.
_M.logDebug = function(self, msg, ...) 
    self:log(ffi.C.IB_LOG_DEBUG, "LuaAPI - [DEBUG]", msg, ...)
end

_M.log = function(self, level, prefix, msg, ...) 
    local debug_table = debug.getinfo(3, "Sl")
    local file = debug_table.short_src
    local line = debug_table.currentline

    -- Msg must not be nil.
    if msg == nil then
        msg = "(nil)"
    elseif type(msg) ~= 'string' then
        msg = tostring(msg)
    end

    -- If we have more arguments, format msg with them.
    if ... ~= nil then
        msg = string.format(msg, ...)
    end

    -- Prepend prefix.
    msg = prefix .. " " .. msg

    ffi.C.ib_log_ex(self.ib_engine, level, file, line, msg);
end


-- ###########################################################################
return _M


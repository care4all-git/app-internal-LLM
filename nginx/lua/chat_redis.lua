local redis = require "resty.redis"
local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- Configuration
local REDIS_HOST = os.getenv("REDIS_HOST") or "redis"
local REDIS_PORT = tonumber(os.getenv("REDIS_PORT") or "6379")

-- Connect to Redis with error handling
function _M.connect()
    local red = redis:new()
    
    red:set_timeouts(1000, 1000, 1000) -- 1 second timeouts
    
    local ok, err = red:connect(REDIS_HOST, REDIS_PORT)
    if not ok then
        utils.log_error("chat_redis", "connect", "Failed to connect to Redis: " .. (err or "unknown"))
        return nil, err
    end
    
    utils.log_info("chat_redis", "connect", "Connected to Redis successfully")
    return red, nil
end

-- Close Redis connection with keepalive
function _M.close(red)
    if not red then return end
    
    local ok, err = red:set_keepalive(10000, 100)
    if not ok then
        utils.log_error("chat_redis", "close", "Failed to set Redis keepalive: " .. (err or "unknown"))
        red:close()
    end
end

-- Execute Redis command with connection management
function _M.execute(command_func)
    local red, err = _M.connect()
    if not red then
        return nil, "Redis connection failed: " .. (err or "unknown")
    end
    
    local ok, result = pcall(command_func, red)
    _M.close(red)
    
    if not ok then
        utils.log_error("chat_redis", "execute", "Redis command failed: " .. tostring(result))
        return nil, result
    end
    
    return result, nil
end

-- Save message with proper structure including artifact references
function _M.save_message(chat_id, message_id, role, content, files, artifacts)
    return _M.execute(function(red)
        local message_data = {
            id = message_id,
            role = role,
            content = content,
            files = files or {},
            artifacts = artifacts or {},
            timestamp = ngx.time(),
            chat_id = chat_id
        }
        
        -- Save individual message
        local message_key = "message:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. message_id
        red:set(message_key, cjson.encode(message_data))
        red:expire(message_key, 86400 * 365) -- Expire after 1 year
        
        -- Add to ordered chat message list (newest first for easy retrieval)
        local chat_messages_key = "chat:messages:" .. utils.USER_ID .. ":" .. chat_id
        red:lpush(chat_messages_key, message_id)
        red:expire(chat_messages_key, 86400 * 365)
        
        -- Update chat metadata
        local chat_meta_key = "chat:meta:" .. utils.USER_ID .. ":" .. chat_id
        local chat_meta = {
            id = chat_id,
            last_updated = ngx.time(),
            message_count = red:llen(chat_messages_key),
            last_message_preview = string.sub(content or "", 1, 100)
        }
        red:set(chat_meta_key, cjson.encode(chat_meta))
        red:expire(chat_meta_key, 86400 * 365)
        
        utils.log_info("chat_redis", "save_message", {
            chat_id = chat_id,
            message_id = message_id,
            role = role,
            artifacts_count = #(artifacts or {})
        })
        
        return message_data
    end)
end

-- Save artifact (code block) with proper parent relationship
function _M.save_artifact(chat_id, artifact_id, parent_message_id, code, language, metadata)
    return _M.execute(function(red)
        local artifact_data = {
            id = artifact_id,
            parent_id = parent_message_id,
            type = "code_block",
            code = code,
            language = language or "",
            metadata = metadata or {},
            timestamp = ngx.time(),
            chat_id = chat_id
        }
        
        -- Save individual artifact
        local artifact_key = "artifact:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. artifact_id
        red:set(artifact_key, cjson.encode(artifact_data))
        red:expire(artifact_key, 86400 * 365)
        
        -- Add to chat artifacts list
        local chat_artifacts_key = "chat:artifacts:" .. utils.USER_ID .. ":" .. chat_id
        red:lpush(chat_artifacts_key, artifact_id)
        red:expire(chat_artifacts_key, 86400 * 365)
        
        utils.log_info("chat_redis", "save_artifact", {
            chat_id = chat_id,
            artifact_id = artifact_id,
            parent_id = parent_message_id,
            language = language
        })
        
        return artifact_data
    end)
end

-- Get chat messages in chronological order (for display)
function _M.get_chat_messages(chat_id, limit)
    return _M.execute(function(red)
        local chat_messages_key = "chat:messages:" .. utils.USER_ID .. ":" .. chat_id
        local message_ids = red:lrange(chat_messages_key, 0, (limit or 50) - 1)
        
        local messages = {}
        if message_ids and type(message_ids) == "table" then
            -- Reverse to get chronological order (oldest first for chat display)
            for i = #message_ids, 1, -1 do
                local message_key = "message:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. message_ids[i]
                local message_json = red:get(message_key)
                
                if message_json and message_json ~= ngx.null then
                    local ok, message = pcall(cjson.decode, message_json)
                    if ok then
                        table.insert(messages, message)
                    end
                end
            end
        end
        
        utils.log_info("chat_redis", "get_chat_messages", {
            chat_id = chat_id,
            message_count = #messages,
            limit = limit
        })
        
        return messages
    end)
end

-- Get chat context for AI (last N messages in correct format for model)
function _M.get_chat_context(chat_id, context_limit)
    local messages, err = _M.get_chat_messages(chat_id, context_limit or 10)
    if not messages then
        return nil, err
    end
    
    local context_messages = {}
    
    for _, msg in ipairs(messages) do
        local role = msg.role == "user" and "user" or "assistant"
        table.insert(context_messages, {
            role = role,
            content = msg.content
        })
    end
    
    return context_messages, nil
end

-- Get all artifacts for a chat
function _M.get_chat_artifacts(chat_id)
    return _M.execute(function(red)
        local chat_artifacts_key = "chat:artifacts:" .. utils.USER_ID .. ":" .. chat_id
        local artifact_ids = red:lrange(chat_artifacts_key, 0, -1)
        
        local artifacts = {}
        if artifact_ids and type(artifact_ids) == "table" then
            for _, artifact_id in ipairs(artifact_ids) do
                local artifact_key = "artifact:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. artifact_id
                local artifact_json = red:get(artifact_key)
                
                if artifact_json and artifact_json ~= ngx.null then
                    local ok, artifact = pcall(cjson.decode, artifact_json)
                    if ok then
                        table.insert(artifacts, artifact)
                    end
                end
            end
        end
        
        -- Also include message artifacts (admin/jai messages themselves)
        local messages = _M.get_chat_messages(chat_id, 1000)
        if messages then
            for _, message in ipairs(messages) do
                -- Determine artifact type from message ID
                local artifact_type = "unknown"
                if message.id and string.match(message.id, "^admin%(") then
                    artifact_type = "admin"
                elseif message.id and string.match(message.id, "^jai%(") then
                    artifact_type = "jai"
                end
                
                if artifact_type ~= "unknown" then
                    table.insert(artifacts, {
                        id = message.id,
                        type = artifact_type,
                        content = message.content,
                        files = message.files or {},
                        timestamp = message.timestamp,
                        chat_id = message.chat_id
                    })
                end
            end
        end
        
        return artifacts
    end)
end

-- Get message details including artifacts
function _M.get_message_details(chat_id, message_id)
    return _M.execute(function(red)
        -- Get message
        local message_key = "message:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. message_id
        local message_json = red:get(message_key)
        
        local result = {}
        if message_json and message_json ~= ngx.null then
            local ok, message = pcall(cjson.decode, message_json)
            if ok then
                result.message = message
                
                -- Get artifacts for this message
                local artifacts = {}
                if message.artifacts and type(message.artifacts) == "table" then
                    for _, artifact_id in ipairs(message.artifacts) do
                        local artifact_key = "artifact:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. artifact_id
                        local artifact_json = red:get(artifact_key)
                        
                        if artifact_json and artifact_json ~= ngx.null then
                            local ok_artifact, artifact = pcall(cjson.decode, artifact_json)
                            if ok_artifact then
                                table.insert(artifacts, artifact)
                            end
                        end
                    end
                end
                result.artifacts = artifacts
            end
        end
        
        return result
    end)
end

-- Create new chat metadata
function _M.create_chat(chat_id)
    return _M.execute(function(red)
        local chat_meta_key = "chat:meta:" .. utils.USER_ID .. ":" .. chat_id
        local chat_meta = {
            id = chat_id,
            last_updated = ngx.time(),
            message_count = 0,
            last_message_preview = ""
        }
        red:set(chat_meta_key, cjson.encode(chat_meta))
        red:expire(chat_meta_key, 86400 * 365)
        
        utils.log_info("chat_redis", "create_chat", {
            chat_id = chat_id,
            timestamp = ngx.time()
        })
        
        return {
            chat_id = chat_id,
            created_at = ngx.time()
        }
    end)
end

-- Get list of all chats for a user
function _M.get_chat_list()
    return _M.execute(function(red)
        -- Get all chat metadata
        local pattern = "chat:meta:" .. utils.USER_ID .. ":*"
        local keys = red:keys(pattern)
        
        local chats = {}
        
        if keys and type(keys) == "table" then
            for _, key in ipairs(keys) do
                local meta_json = red:get(key)
                if meta_json and meta_json ~= ngx.null then
                    local ok, meta = pcall(cjson.decode, meta_json)
                    if ok then
                        table.insert(chats, {
                            id = meta.id,
                            message_count = meta.message_count or 0,
                            last_updated = meta.last_updated or ngx.time(),
                            preview = meta.last_message_preview or ""
                        })
                    end
                end
            end
        end
        
        -- Sort chats by last updated (newest first)
        table.sort(chats, function(a, b)
            return (a.last_updated or 0) > (b.last_updated or 0)
        end)
        
        utils.log_info("chat_redis", "get_chat_list", {
            chat_count = #chats
        })
        
        return chats
    end)
end

-- Clear all data for a specific chat
function _M.clear_chat(chat_id)
    return _M.execute(function(red)
        local chat_messages_key = "chat:messages:" .. utils.USER_ID .. ":" .. chat_id
        local chat_artifacts_key = "chat:artifacts:" .. utils.USER_ID .. ":" .. chat_id
        local chat_meta_key = "chat:meta:" .. utils.USER_ID .. ":" .. chat_id
        local counter_admin_key = "chat:counter:" .. utils.USER_ID .. ":" .. chat_id .. ":admin"
        local counter_jai_key = "chat:counter:" .. utils.USER_ID .. ":" .. chat_id .. ":jai"
        
        local deleted_items = 0
        
        -- Get all message and artifact IDs to delete individual records
        local message_ids = red:lrange(chat_messages_key, 0, -1)
        local artifact_ids = red:lrange(chat_artifacts_key, 0, -1)
        
        -- Delete individual messages
        if message_ids and type(message_ids) == "table" then
            for _, message_id in ipairs(message_ids) do
                local message_key = "message:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. message_id
                deleted_items = deleted_items + red:del(message_key)
            end
        end
        
        -- Delete individual artifacts
        if artifact_ids and type(artifact_ids) == "table" then
            for _, artifact_id in ipairs(artifact_ids) do
                local artifact_key = "artifact:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. artifact_id
                deleted_items = deleted_items + red:del(artifact_key)
            end
        end
        
        -- Delete all chat keys
        deleted_items = deleted_items + red:del(chat_messages_key)
        deleted_items = deleted_items + red:del(chat_artifacts_key)
        deleted_items = deleted_items + red:del(chat_meta_key)
        deleted_items = deleted_items + red:del(counter_admin_key)
        deleted_items = deleted_items + red:del(counter_jai_key)
        
        utils.log_info("chat_redis", "clear_chat", {
            chat_id = chat_id,
            deleted_items = deleted_items
        })
        
        return {
            success = true,
            deleted_count = deleted_items
        }
    end)
end

-- Delete all chats for a user
function _M.delete_all_chats()
    return _M.execute(function(red)
        local deleted_count = 0
        
        -- Get all patterns for this user
        local patterns = {
            "message:" .. utils.USER_ID .. ":*",
            "artifact:" .. utils.USER_ID .. ":*",
            "chat:messages:" .. utils.USER_ID .. ":*",
            "chat:artifacts:" .. utils.USER_ID .. ":*",
            "chat:meta:" .. utils.USER_ID .. ":*",
            "chat:counter:" .. utils.USER_ID .. ":*"
        }
        
        for _, pattern in ipairs(patterns) do
            local keys = red:keys(pattern)
            if keys and type(keys) == "table" then
                for _, key in ipairs(keys) do
                    deleted_count = deleted_count + red:del(key)
                end
            end
        end
        
        utils.log_info("chat_redis", "delete_all_chats", {
            deleted_count = deleted_count
        })
        
        return {
            success = true,
            deleted_count = deleted_count
        }
    end)
end

-- Health check for Redis connection
function _M.health_check()
    return _M.execute(function(red)
        local pong = red:ping()
        return {
            status = "healthy",
            response = pong,
            timestamp = ngx.time()
        }
    end)
end

-- Get Redis statistics
function _M.get_stats()
    return _M.execute(function(red)
        local info = red:info("memory")
        local dbsize = red:dbsize()
        
        return {
            db_size = dbsize,
            memory_info = info,
            timestamp = ngx.time()
        }
    end)
end

return _M
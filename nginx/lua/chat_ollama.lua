local http = require "resty.http"
local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- Create HTTP client for Ollama requests with no timeout limits
function _M.create_client()
    local httpc = http.new()
    -- CRITICAL: Do NOT set any timeouts here - use per-request timeouts
    -- httpc:set_timeout() -- Let each request set its own timeout
    return httpc
end

-- Prepare Ollama request payload with unlimited response length
function _M.prepare_request(context_messages, options)
    local request_data = {
        model = utils.MODEL_NAME,
        messages = context_messages,
        stream = true,
        options = {
            temperature = options and options.temperature or utils.MODEL_TEMPERATURE,
            top_p = options and options.top_p or utils.MODEL_TOP_P,
            top_k = options and options.top_k or utils.MODEL_TOP_K,
            num_ctx = options and options.num_ctx or utils.MODEL_NUM_CTX,
            -- ENHANCED: Allow unlimited response length
            num_predict = -1, -- -1 means no limit, let AI complete fully
            repeat_penalty = options and options.repeat_penalty or utils.MODEL_REPEAT_PENALTY or 1.1,
            stop = {} -- No stop sequences to prevent premature truncation
        }
    }
    
    utils.log_info("chat_ollama", "prepare_request", {
        model = request_data.model,
        message_count = #context_messages,
        options = request_data.options,
        unlimited_response = true
    })
    
    return request_data
end

-- FIXED: Added missing prepare_context_with_files function
function _M.prepare_context_with_files(context_messages, files)
    local enhanced_context = {}
    
    -- Copy existing messages
    for _, msg in ipairs(context_messages) do
        table.insert(enhanced_context, {
            role = msg.role,
            content = msg.content
        })
    end
    
    -- Add file context to the last user message if files are provided
    if files and #files > 0 then
        local file_context = utils.format_files_for_context(files)
        if file_context ~= "" and #enhanced_context > 0 then
            local last_message = enhanced_context[#enhanced_context]
            if last_message.role == "user" then
                last_message.content = last_message.content .. file_context
            end
        end
    end
    
    utils.log_info("chat_ollama", "prepare_context_with_files", {
        original_message_count = #context_messages,
        enhanced_message_count = #enhanced_context,
        files_count = files and #files or 0
    })
    
    return enhanced_context
end

-- Send request to Ollama API with enhanced error handling
function _M.send_request(httpc, request_data)
    local url = utils.MODEL_URL .. "/api/chat"
    
    utils.log_info("chat_ollama", "send_request", {
        url = url,
        model = request_data.model,
        stream = request_data.stream,
        unlimited_mode = true
    })
    
    local res, err = httpc:request_uri(url, {
        method = "POST",
        body = cjson.encode(request_data),
        headers = {
            ["Content-Type"] = "application/json"
        }
        -- CRITICAL: Remove all timeout settings - let nginx handle it
        -- read_timeout, send_timeout, connect_timeout removed
    })
    
    if not res then
        utils.log_error("chat_ollama", "send_request", "Request failed", {
            error = err,
            url = url
        })
        return nil, "Failed to connect to Ollama: " .. (err or "unknown error")
    end
    
    if res.status ~= 200 then
        local error_body = res.body and res.body:sub(1, 200) or "No response body"
        utils.log_error("chat_ollama", "send_request", "HTTP error", {
            status = res.status,
            body = error_body,
            url = url
        })
        return nil, "Ollama returned HTTP " .. res.status .. ": " .. error_body
    end
    
    utils.log_info("chat_ollama", "send_request", {
        status = res.status,
        response_length = res.body and #res.body or 0
    })
    
    return res, nil
end

-- ENHANCED: Parse streaming response with completion detection
function _M.parse_streaming_response(response_body)
    local full_response = ""
    local lines = {}
    
    -- Split response into lines
    for line in response_body:gmatch("[^\r\n]+") do
        table.insert(lines, line)
    end
    
    utils.log_info("chat_ollama", "parse_streaming_response", {
        line_count = #lines
    })
    
    local chunks = {}
    local is_complete = false
    local completion_reason = "unknown"
    
    -- Process each line from Ollama
    for _, line in ipairs(lines) do
        if line and line ~= "" then
            local ok_chunk, chunk_data = pcall(cjson.decode, line)
            if ok_chunk and chunk_data.message then
                local content = chunk_data.message.content or ""
                
                if content ~= "" then
                    full_response = full_response .. content
                    
                    table.insert(chunks, {
                        content = content,
                        done = chunk_data.done or false
                    })
                end
                
                -- Check for completion indicators
                if chunk_data.done then
                    is_complete = true
                    completion_reason = chunk_data.done_reason or "finished"
                    
                    -- Log completion details
                    utils.log_info("chat_ollama", "response_complete", {
                        reason = completion_reason,
                        total_length = #full_response,
                        chunk_count = #chunks
                    })
                    
                    break
                end
                
            elseif ok_chunk and chunk_data.error then
                utils.log_error("chat_ollama", "parse_streaming_response", "Ollama error", {
                    error = chunk_data.error
                })
                return nil, nil, "Ollama error: " .. chunk_data.error
            end
        end
    end
    
    -- ENHANCED: Check if response seems truncated
    local seems_truncated = false
    if not is_complete and #full_response > 0 then
        -- Check for common truncation indicators
        local last_part = string.sub(full_response, -50)
        if string.find(last_part, "%.%.%.$") or 
           string.find(last_part, "%[continued%]") or
           string.find(last_part, "%[truncated%]") then
            seems_truncated = true
        end
    end
    
    utils.log_info("chat_ollama", "parse_streaming_response", {
        full_response_length = #full_response,
        chunk_count = #chunks,
        is_complete = is_complete,
        completion_reason = completion_reason,
        seems_truncated = seems_truncated
    })
    
    return full_response, chunks, nil, {
        is_complete = is_complete,
        completion_reason = completion_reason,
        seems_truncated = seems_truncated
    }
end

-- ENHANCED: Stream chat completion with full response handling
function _M.stream_chat(context_messages, options)
    -- Validate configuration
    local config_valid, config_issues = _M.validate_config()
    if not config_valid then
        return nil, "Configuration error: " .. table.concat(config_issues, ", ")
    end
    
    -- Create HTTP client with no timeouts
    local httpc = _M.create_client()
    
    -- Prepare request for unlimited response
    local request_data = _M.prepare_request(context_messages, options)
    
    -- Send request
    local response, err = _M.send_request(httpc, request_data)
    if not response then
        httpc:close()
        return nil, err
    end
    
    -- Parse streaming response with completion detection
    local full_response, chunks, parse_err, completion_info = _M.parse_streaming_response(response.body)
    httpc:close()
    
    if parse_err then
        return nil, parse_err
    end
    
    -- ENHANCED: Return completion information
    return {
        full_response = full_response,
        chunks = chunks,
        model = utils.MODEL_NAME,
        options = request_data.options,
        completion_info = completion_info or {
            is_complete = true,
            completion_reason = "finished"
        }
    }, nil
end

-- NEW: Check if response needs continuation
function _M.needs_continuation(response_result)
    if not response_result or not response_result.completion_info then
        return false
    end
    
    local info = response_result.completion_info
    return not info.is_complete or info.seems_truncated
end

-- NEW: Generate continuation prompt
function _M.create_continuation_prompt(original_response)
    return "Please continue your previous response from where you left off. Complete your full answer without repeating what you already said."
end

-- Validate Ollama model configuration
function _M.validate_config()
    local issues = {}
    
    if not utils.MODEL_URL or utils.MODEL_URL == "" then
        table.insert(issues, "MODEL_URL is not configured")
    end
    
    if not utils.MODEL_NAME or utils.MODEL_NAME == "" then
        table.insert(issues, "MODEL_NAME is not configured")
    end
    
    if utils.MODEL_TEMPERATURE < 0 or utils.MODEL_TEMPERATURE > 2 then
        table.insert(issues, "MODEL_TEMPERATURE should be between 0 and 2")
    end
    
    if utils.MODEL_TOP_P < 0 or utils.MODEL_TOP_P > 1 then
        table.insert(issues, "MODEL_TOP_P should be between 0 and 1")
    end
    
    if utils.MODEL_NUM_CTX <= 0 then
        table.insert(issues, "MODEL_NUM_CTX should be positive")
    end
    
    if #issues > 0 then
        utils.log_error("chat_ollama", "validate_config", "Configuration issues", {
            issues = issues
        })
        return false, issues
    end
    
    return true, nil
end

-- Test Ollama connection and model availability
function _M.health_check()
    local httpc = _M.create_client()
    
    -- Check if Ollama is responding - with reasonable timeout for health check
    local res, err = httpc:request_uri(utils.MODEL_URL .. "/api/tags", {
        method = "GET",
        headers = {
            ["Content-Type"] = "application/json"
        }
        -- Use default OpenResty timeout for health checks only
    })
    
    httpc:close()
    
    if not res then
        utils.log_error("chat_ollama", "health_check", "Connection failed", {
            error = err,
            url = utils.MODEL_URL
        })
        return false, "Cannot connect to Ollama: " .. (err or "unknown error")
    end
    
    if res.status ~= 200 then
        utils.log_error("chat_ollama", "health_check", "HTTP error", {
            status = res.status,
            url = utils.MODEL_URL
        })
        return false, "Ollama returned HTTP " .. res.status
    end
    
    -- Parse response to check available models
    local ok, models_data = pcall(cjson.decode, res.body)
    if not ok then
        utils.log_error("chat_ollama", "health_check", "Invalid response", {
            body = res.body and res.body:sub(1, 200) or "empty"
        })
        return false, "Invalid response from Ollama"
    end
    
    -- Check if our configured model is available
    local model_found = false
    if models_data.models then
        for _, model in ipairs(models_data.models) do
            if model.name == utils.MODEL_NAME then
                model_found = true
                break
            end
        end
    end
    
    if not model_found then
        utils.log_error("chat_ollama", "health_check", "Model not found", {
            configured_model = utils.MODEL_NAME,
            available_models = models_data.models
        })
        return false, "Model '" .. utils.MODEL_NAME .. "' not found in Ollama"
    end
    
    utils.log_info("chat_ollama", "health_check", {
        status = "healthy",
        model = utils.MODEL_NAME,
        available_models_count = models_data.models and #models_data.models or 0,
        unlimited_responses = true
    })
    
    return true, "Ollama is healthy and model is available"
end

-- Get default model options for unlimited responses
function _M.get_default_options()
    return {
        temperature = utils.MODEL_TEMPERATURE,
        top_p = utils.MODEL_TOP_P,
        top_k = utils.MODEL_TOP_K,
        num_ctx = utils.MODEL_NUM_CTX,
        num_predict = -1, -- Unlimited response length
        repeat_penalty = utils.MODEL_REPEAT_PENALTY or 1.1,
        stop = {} -- No stop sequences
    }
end

-- Format error message for user display
function _M.format_error(error_msg)
    if not error_msg then
        return "Unknown error occurred"
    end
    
    -- Connection errors
    if string.find(error_msg, "connection refused") or 
       string.find(error_msg, "timeout") or
       string.find(error_msg, "connect") then
        return "Cannot connect to AI service. Please check if Ollama is running."
    end
    
    -- Model errors
    if string.find(error_msg, "model") and string.find(error_msg, "not found") then
        return "AI model not available. Please check Ollama configuration."
    end
    
    -- HTTP errors
    if string.find(error_msg, "HTTP 4") then
        return "Invalid request to AI service. Please try again."
    end
    
    if string.find(error_msg, "HTTP 5") then
        return "AI service error. Please try again in a moment."
    end
    
    -- Configuration errors
    if string.find(error_msg, "Configuration error") then
        return "AI service configuration error. Please contact administrator."
    end
    
    -- Generic fallback
    return "AI service error: " .. error_msg
end

return _M
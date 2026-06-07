local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- Set up Server-Sent Events headers with no timeout limits
function _M.setup_sse_headers()
    ngx.header["Content-Type"] = "text/event-stream"
    ngx.header["Cache-Control"] = "no-cache"
    ngx.header["Connection"] = "keep-alive"
    ngx.header["X-Accel-Buffering"] = "no" -- Disable nginx buffering for real-time streaming
    utils.set_cors_headers()
    
    utils.log_info("chat_sse", "setup_headers", "SSE headers configured with no buffering")
end

-- Send SSE event with data
function _M.send_event(event_data, event_type)
    local data_str
    
    if type(event_data) == "table" then
        data_str = cjson.encode(event_data)
    else
        data_str = tostring(event_data)
    end
    
    if event_type then
        ngx.say("event: " .. event_type)
    end
    
    ngx.say("data: " .. data_str .. "\n")
    ngx.flush(true) -- Force flush for immediate delivery
    
    utils.log_info("chat_sse", "send_event", {
        event_type = event_type,
        data_length = #data_str
    })
end

-- Send chat ID to client
function _M.send_chat_id(chat_id)
    _M.send_event({
        chat_id = chat_id,
        type = "chat_id"
    }, "chat_id")
end

-- Send content chunk to client
function _M.send_content_chunk(content)
    _M.send_event({
        content = content,
        type = "content"
    }, "content")
end

-- NEW: Send completion status with continuation info
function _M.send_completion_status(is_complete, needs_continuation, completion_reason)
    _M.send_event({
        is_complete = is_complete,
        needs_continuation = needs_continuation,
        completion_reason = completion_reason or "finished",
        type = "completion_status"
    }, "completion_status")
end

-- Send error to client
function _M.send_error(error_msg, details)
    local error_data = {
        error = error_msg,
        type = "error"
    }
    
    if details then
        error_data.details = details
    end
    
    _M.send_event(error_data, "error")
end

-- ENHANCED: Send completion signal with continuation info
function _M.send_completion(completion_info)
    if completion_info and not completion_info.is_complete then
        -- Response was truncated, offer continuation
        _M.send_event({
            type = "continuation_needed",
            reason = completion_info.completion_reason or "truncated",
            message = "Response may be incomplete. Would you like me to continue?"
        }, "continuation_needed")
    end
    
    _M.send_event("[DONE]", "done")
    utils.log_info("chat_sse", "send_completion", {
        completed = "Stream completed",
        needs_continuation = completion_info and not completion_info.is_complete
    })
end

-- Send status update
function _M.send_status(status, message)
    _M.send_event({
        status = status,
        message = message,
        type = "status",
        timestamp = ngx.time()
    }, "status")
end

-- ENHANCED: Stream Ollama response chunks with completion detection
function _M.stream_ollama_response(ollama_result)
    if not ollama_result or not ollama_result.chunks then
        _M.send_error("No response from AI service")
        return false
    end
    
    local total_chunks = #ollama_result.chunks
    
    utils.log_info("chat_sse", "stream_ollama_response", {
        chunk_count = total_chunks,
        full_response_length = ollama_result.full_response and #ollama_result.full_response or 0,
        completion_info = ollama_result.completion_info
    })
    
    -- Stream each chunk with minimal delay
    for i, chunk in ipairs(ollama_result.chunks) do
        if chunk.content and chunk.content ~= "" then
            _M.send_content_chunk(chunk.content)
            
            -- Minimal delay only every 20 chunks to prevent overwhelming
            if i % 20 == 0 then
                ngx.sleep(0.001) -- 1ms delay every 20 chunks
            end
        end
        
        if chunk.done then
            break
        end
    end
    
    -- Send completion info
    local completion_info = ollama_result.completion_info or {
        is_complete = true,
        completion_reason = "finished"
    }
    
    _M.send_completion_status(
        completion_info.is_complete,
        not completion_info.is_complete,
        completion_info.completion_reason
    )
    
    return true, completion_info
end

-- ENHANCED: Handle streaming chat request with continuation support
function _M.handle_streaming_chat(chat_id, user_message, files, context_messages, ollama)
    -- Set up SSE with no buffering
    _M.setup_sse_headers()
    
    -- Send chat ID immediately
    _M.send_chat_id(chat_id)
    
    -- Send status update
    _M.send_status("processing", "Generating complete response...")
    
    -- Prepare context with files
    local enhanced_context = ollama.prepare_context_with_files(context_messages, files)
    
    -- Add current message with file context to context
    local complete_message = user_message
    local file_context = utils.format_files_for_context(files)
    
    if file_context ~= "" then
        complete_message = complete_message .. file_context
    end
    
    table.insert(enhanced_context, {
        role = "user",
        content = complete_message
    })
    
    utils.log_info("chat_sse", "handle_streaming_chat", {
        chat_id = chat_id,
        context_size = #enhanced_context,
        files_count = files and #files or 0,
        message_length = #user_message,
        unlimited_mode = true
    })
    
    -- Stream response from Ollama with unlimited length
    local ollama_result, err = ollama.stream_chat(enhanced_context)
    
    if not ollama_result then
        local user_friendly_error = ollama.format_error(err)
        _M.send_error(user_friendly_error, err)
        utils.log_error("chat_sse", "handle_streaming_chat", "Ollama error", {
            error = err,
            chat_id = chat_id
        })
        return nil, err
    end
    
    -- Stream the response to client with completion info
    local stream_success, completion_info = _M.stream_ollama_response(ollama_result)
    
    if stream_success then
        -- Send final completion signal with continuation info
        _M.send_completion(completion_info)
        
        utils.log_info("chat_sse", "handle_streaming_chat", {
            chat_id = chat_id,
            response_length = #ollama_result.full_response,
            status = "completed",
            is_complete = completion_info and completion_info.is_complete,
            needs_continuation = completion_info and not completion_info.is_complete
        })
        
        return ollama_result.full_response, nil, completion_info
    else
        _M.send_error("Failed to stream response")
        return nil, "Stream processing failed", nil
    end
end

-- NEW: Handle continuation request
function _M.handle_continuation_request(chat_id, previous_response, context_messages, ollama)
    -- Set up SSE
    _M.setup_sse_headers()
    
    -- Send status
    _M.send_status("continuing", "Continuing previous response...")
    
    -- Create continuation context
    local continuation_context = {}
    
    -- Add previous context (excluding the last AI response to avoid repetition)
    for i, msg in ipairs(context_messages) do
        if i < #context_messages then -- Skip the last incomplete response
            table.insert(continuation_context, msg)
        end
    end
    
    -- Add continuation prompt
    local continuation_prompt = ollama.create_continuation_prompt(previous_response)
    table.insert(continuation_context, {
        role = "user",
        content = continuation_prompt
    })
    
    utils.log_info("chat_sse", "handle_continuation_request", {
        chat_id = chat_id,
        context_size = #continuation_context,
        previous_response_length = #previous_response
    })
    
    -- Stream continuation from Ollama
    local ollama_result, err = ollama.stream_chat(continuation_context)
    
    if not ollama_result then
        local user_friendly_error = ollama.format_error(err)
        _M.send_error(user_friendly_error, err)
        return nil, err
    end
    
    -- Stream the continuation
    local stream_success, completion_info = _M.stream_ollama_response(ollama_result)
    
    if stream_success then
        _M.send_completion(completion_info)
        
        -- Combine with previous response
        local combined_response = previous_response .. ollama_result.full_response
        
        utils.log_info("chat_sse", "handle_continuation_request", {
            chat_id = chat_id,
            continuation_length = #ollama_result.full_response,
            combined_length = #combined_response,
            status = "continued"
        })
        
        return combined_response, nil, completion_info
    else
        _M.send_error("Failed to stream continuation")
        return nil, "Continuation failed", nil
    end
end

-- Handle connection errors gracefully
function _M.handle_connection_error(error_msg)
    -- Log the error
    utils.log_error("chat_sse", "connection_error", error_msg)
    
    -- Try to send error if connection is still alive
    local ok, err = pcall(function()
        _M.send_error("Connection error", error_msg)
    end)
    
    if not ok then
        utils.log_error("chat_sse", "send_error_failed", "Could not send error to client: " .. tostring(err))
    end
end

-- Send heartbeat to keep connection alive during long responses
function _M.send_heartbeat()
    _M.send_event({
        type = "heartbeat",
        timestamp = ngx.time()
    }, "heartbeat")
end

-- Send progress update during long operations
function _M.send_progress(current, total, message)
    _M.send_event({
        type = "progress",
        current = current,
        total = total,
        percentage = math.floor((current / total) * 100),
        message = message or "",
        timestamp = ngx.time()
    }, "progress")
end

-- Handle client disconnect
function _M.handle_client_disconnect()
    utils.log_info("chat_sse", "client_disconnect", "Client disconnected during streaming")
    
    -- Cleanup any resources here
    -- This would be called if ngx.eof() returns true
end

-- ENHANCED: More lenient SSE connection validation
function _M.validate_connection()
    local accept = ngx.var.http_accept or ""
    local user_agent = ngx.var.http_user_agent or ""
    
    -- Accept if client explicitly requests event-stream OR sends */* OR no Accept header
    local accepts_sse = string.find(accept, "text/event%-stream") or 
                       string.find(accept, "%*/%*") or
                       accept == ""
    
    if not accepts_sse then
        utils.log_error("chat_sse", "validate_connection", "Client doesn't accept event-stream", {
            accept = accept,
            user_agent = user_agent
        })
        return false, "Client doesn't support Server-Sent Events"
    end
    
    return true, nil
end

-- Setup SSE with validation and no timeouts
function _M.setup_validated_sse()
    local valid, err = _M.validate_connection()
    if not valid then
        return false, err
    end
    
    _M.setup_sse_headers()
    return true, nil
end

return _M
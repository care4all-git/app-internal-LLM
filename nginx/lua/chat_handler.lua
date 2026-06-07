-- Enhanced Chat Handler - With continuation support and no timeouts
local utils = require "chat_utils"
local redis = require "chat_redis"
local ollama = require "chat_ollama"
local sse = require "chat_sse"
local view = require "chat_view"
local artifacts = require "chat_artifacts"
local files = require "chat_files"

local _M = {}

-- Serve the chat HTML page
function _M.serve_chat_page()
    view.serve_chat_page()
end

-- Create new chat endpoint
function _M.handle_create_chat()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    -- Generate new chat ID
    local chat_id = utils.generate_chat_id()
    
    -- Create chat in Redis
    local result, err = redis.create_chat(chat_id)
    if not result then
        utils.log_error("chat_handler", "create_chat", "Failed to create chat", {
            error = err,
            chat_id = chat_id
        })
        return view.render_api_error(500, "Failed to create chat", err)
    end
    
    view.render_success(result, "Chat created successfully", 201)
end

-- ENHANCED: Streaming chat endpoint with unlimited AI responses
function _M.handle_chat_stream()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    -- Validate SSE connection
    local sse_valid, sse_err = sse.validate_connection()
    if not sse_valid then
        return view.render_api_error(400, "SSE not supported", sse_err)
    end
    
    -- Read and parse request
    local body, body_err = utils.read_request_body()
    if not body then
        return view.render_api_error(400, "No request body", body_err.error)
    end
    
    local request_data, parse_err = utils.parse_json_request(body)
    if not request_data then
        return view.render_api_error(400, "Invalid JSON", parse_err.error)
    end
    
    local user_message = request_data.message or ""
    local request_files = request_data.files or {}
    local chat_id = request_data.chat_id
    
    -- Validate input
    if user_message == "" and #request_files == 0 then
        return view.render_api_error(400, "No message or files provided", "Either message text or file attachments are required")
    end
    
    -- Generate chat ID if not provided
    if not chat_id or chat_id == "" then
        chat_id = utils.generate_chat_id()
        local create_result, create_err = redis.create_chat(chat_id)
        if not create_result then
            return view.render_api_error(500, "Failed to create chat", create_err)
        end
        utils.log_info("chat_handler", "generated_chat_id", { chat_id = chat_id })
    elseif not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat ID format", "Chat ID must be in format chat(timestamp)")
    end
    
    -- Process and validate files
    local processed_files = {}
    if #request_files > 0 then
        local validation_result = files.validate_file_batch(request_files)
        if not validation_result.batch_valid then
            return view.render_api_error(400, "File validation failed", table.concat(validation_result.errors, "; "))
        end
        
        -- Process file contents
        for _, file_data in ipairs(request_files) do
            local processed, process_err = files.process_file_content(file_data)
            if processed then
                table.insert(processed_files, processed)
            else
                utils.log_error("chat_handler", "file_processing", "Failed to process file", {
                    filename = file_data.name,
                    error = process_err
                })
            end
        end
        
        utils.log_info("chat_handler", "file_processing_complete", {
            original_count = #request_files,
            processed_count = #processed_files
        })
    end
    
    -- Get chat context from Redis
    local context_messages, context_err = redis.get_chat_context(chat_id, 10)
    if not context_messages then
        utils.log_error("chat_handler", "get_context", "Failed to get chat context", {
            error = context_err,
            chat_id = chat_id
        })
        context_messages = {}
    end
    
    -- Save user message to Redis
    local user_message_id
    local save_result, save_err = redis.execute(function(red)
        user_message_id = utils.generate_message_id(red, chat_id, "user")
        return redis.save_message(chat_id, user_message_id, "user", user_message, processed_files, {})
    end)
    
    if not save_result then
        utils.log_error("chat_handler", "save_user_message", "Failed to save user message", {
            error = save_err,
            chat_id = chat_id,
            message_id = user_message_id
        })
    end
    
    -- ENHANCED: Handle streaming response with continuation support
    local ai_response, stream_err, completion_info = sse.handle_streaming_chat(
        chat_id, 
        user_message, 
        processed_files, 
        context_messages, 
        ollama
    )
    
    if not ai_response then
        utils.log_error("chat_handler", "streaming_chat", "Streaming failed", {
            error = stream_err,
            chat_id = chat_id
        })
        return
    end
    
    -- Save AI response and extract artifacts
    local ai_message_id
    local ai_save_result, ai_save_err = redis.execute(function(red)
        ai_message_id = utils.generate_message_id(red, chat_id, "assistant")
        local artifact_ids = artifacts.extract_and_save_code_blocks(red, chat_id, ai_message_id, ai_response)
        return redis.save_message(chat_id, ai_message_id, "assistant", ai_response, {}, artifact_ids)
    end)
    
    if not ai_save_result then
        utils.log_error("chat_handler", "save_ai_message", "Failed to save AI message", {
            error = ai_save_err,
            chat_id = chat_id,
            message_id = ai_message_id
        })
    else
        -- Log completion with continuation info
        local artifact_count = 0
        if ai_response then
            for _ in string.gmatch(ai_response, "```[%w]*\n.-\n```") do
                artifact_count = artifact_count + 1
            end
        end
        
        utils.log_info("chat_handler", "chat_stream_complete", {
            chat_id = chat_id,
            user_message_id = user_message_id,
            ai_message_id = ai_message_id,
            artifact_count = artifact_count,
            response_length = #ai_response,
            is_complete = completion_info and completion_info.is_complete or true,
            needs_continuation = completion_info and not completion_info.is_complete or false
        })
    end
end

-- NEW: Handle chat continuation for incomplete responses
function _M.handle_chat_continuation()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    -- Validate SSE connection
    local sse_valid, sse_err = sse.validate_connection()
    if not sse_valid then
        return view.render_api_error(400, "SSE not supported", sse_err)
    end
    
    -- Read and parse request
    local body, body_err = utils.read_request_body()
    if not body then
        return view.render_api_error(400, "No request body", body_err.error)
    end
    
    local request_data, parse_err = utils.parse_json_request(body)
    if not request_data then
        return view.render_api_error(400, "Invalid JSON", parse_err.error)
    end
    
    local chat_id = request_data.chat_id
    local previous_response = request_data.previous_response or ""
    
    -- Validate input
    if not chat_id or chat_id == "" then
        return view.render_api_error(400, "Missing chat_id")
    end
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat ID format")
    end
    
    if previous_response == "" then
        return view.render_api_error(400, "Missing previous_response")
    end
    
    -- Get chat context from Redis
    local context_messages, context_err = redis.get_chat_context(chat_id, 10)
    if not context_messages then
        utils.log_error("chat_handler", "get_continuation_context", "Failed to get chat context", {
            error = context_err,
            chat_id = chat_id
        })
        return view.render_api_error(500, "Failed to get chat context", context_err)
    end
    
    utils.log_info("chat_handler", "handle_continuation", {
        chat_id = chat_id,
        previous_response_length = #previous_response,
        context_size = #context_messages
    })
    
    -- Handle continuation streaming
    local continued_response, continuation_err, completion_info = sse.handle_continuation_request(
        chat_id,
        previous_response,
        context_messages,
        ollama
    )
    
    if not continued_response then
        utils.log_error("chat_handler", "continuation_failed", "Continuation failed", {
            error = continuation_err,
            chat_id = chat_id
        })
        return
    end
    
    -- Update the last AI message with the combined response
    local messages, msg_err = redis.get_chat_messages(chat_id, 5)
    if messages and #messages > 0 then
        -- Find the last assistant message
        local last_ai_message = nil
        for i = #messages, 1, -1 do
            if messages[i].role == "assistant" then
                last_ai_message = messages[i]
                break
            end
        end
        
        if last_ai_message then
            -- Update the message with combined response
            local update_result, update_err = redis.execute(function(red)
                -- Extract new artifacts from the combined response
                local artifact_ids = artifacts.extract_and_save_code_blocks(red, chat_id, last_ai_message.id, continued_response)
                
                -- Update message content
                return redis.save_message(chat_id, last_ai_message.id, "assistant", continued_response, {}, artifact_ids)
            end)
            
            if not update_result then
                utils.log_error("chat_handler", "update_continued_message", "Failed to update message", {
                    error = update_err,
                    chat_id = chat_id,
                    message_id = last_ai_message.id
                })
            else
                utils.log_info("chat_handler", "continuation_complete", {
                    chat_id = chat_id,
                    message_id = last_ai_message.id,
                    final_response_length = #continued_response,
                    is_complete = completion_info and completion_info.is_complete or true
                })
            end
        end
    end
end

-- Get chat history endpoint
function _M.handle_chat_history()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local args = ngx.req.get_uri_args()
    local chat_id = args.chat_id
    
    if not chat_id or chat_id == "" then
        return view.render_api_error(400, "Missing chat_id parameter")
    end
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    local messages, err = redis.get_chat_messages(chat_id, 100)
    if not messages then
        return view.render_api_error(500, "Failed to load chat history", err)
    end
    
    view.render_chat_history(messages, chat_id)
end

-- Get chat list endpoint
function _M.handle_chat_list()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local chats, err = redis.get_chat_list()
    if not chats then
        return view.render_api_error(500, "Failed to load chat list", err)
    end
    
    view.render_chat_list(chats)
end

-- Get chat artifacts endpoint
function _M.handle_chat_artifacts()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local args = ngx.req.get_uri_args()
    local chat_id = args.chat_id
    
    if not chat_id or chat_id == "" then
        return view.render_api_error(400, "Missing chat_id parameter")
    end
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    -- Parse filter options from query parameters
    local filter_options = {
        type = args.type,
        language = args.language,
        sort_by = args.sort_by or "timestamp",
        sort_order = args.sort_order or "desc"
    }
    
    local chat_artifacts, err = artifacts.get_artifacts_with_filter(redis, chat_id, filter_options)
    if not chat_artifacts then
        return view.render_api_error(500, "Failed to load artifacts", err)
    end
    
    view.render_artifacts(chat_artifacts, chat_id)
end

-- Get message details including artifacts
function _M.handle_message_details()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local args = ngx.req.get_uri_args()
    local chat_id = args.chat_id
    local message_id = args.message_id
    
    if not chat_id or not message_id then
        return view.render_api_error(400, "Missing chat_id or message_id parameter")
    end
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    if not utils.is_valid_message_id(message_id) then
        return view.render_api_error(400, "Invalid message_id format")
    end
    
    local result, err = redis.get_message_details(chat_id, message_id)
    if not result then
        return view.render_api_error(500, "Failed to load message details", err)
    end
    
    view.render_message_details(result.message, result.artifacts, chat_id, message_id)
end

-- Clear specific chat endpoint
function _M.handle_clear_chat()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    local body, body_err = utils.read_request_body()
    if not body then
        return view.render_api_error(400, "No request body", body_err.error)
    end
    
    local request_data, parse_err = utils.parse_json_request(body)
    if not request_data or not request_data.chat_id then
        return view.render_api_error(400, "Missing chat_id")
    end
    
    local chat_id = request_data.chat_id
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    local result, err = redis.clear_chat(chat_id)
    if not result then
        return view.render_api_error(500, "Failed to clear chat", err)
    end
    
    view.render_operation_status("clear_chat", true, "Chat cleared successfully", result.deleted_count)
end

-- Delete specific chat endpoint
function _M.handle_delete_chat()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    local body, body_err = utils.read_request_body()
    if not body then
        return view.render_api_error(400, "No request body", body_err.error)
    end
    
    local request_data, parse_err = utils.parse_json_request(body)
    if not request_data or not request_data.chat_id then
        return view.render_api_error(400, "Missing chat_id")
    end
    
    local chat_id = request_data.chat_id
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    local result, err = redis.clear_chat(chat_id) -- Same as clear for now
    if not result then
        return view.render_api_error(500, "Failed to delete chat", err)
    end
    
    view.render_operation_status("delete_chat", true, "Chat deleted successfully", result.deleted_count)
end

-- Delete all chats endpoint
function _M.handle_delete_all_chats()
    if ngx.req.get_method() ~= "POST" then
        return view.handle_method_not_allowed({"POST"})
    end
    
    if ngx.req.get_method() == "OPTIONS" then
        return utils.handle_options_request()
    end
    
    local result, err = redis.delete_all_chats()
    if not result then
        return view.render_api_error(500, "Failed to delete all chats", err)
    end
    
    view.render_operation_status("delete_all_chats", true, "All chats deleted successfully", result.deleted_count)
end

-- Health check endpoint
function _M.handle_health_check()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local services_status = {}
    
    -- Check Redis
    local redis_health, redis_err = redis.health_check()
    services_status.redis = {
        healthy = redis_health ~= nil,
        error = redis_err,
        response = redis_health
    }
    
    -- Check Ollama
    local ollama_healthy, ollama_err = ollama.health_check()
    services_status.ollama = {
        healthy = ollama_healthy,
        error = ollama_err
    }
    
    -- Check file system (basic check)
    local file_check = io.open("/usr/local/openresty/nginx/static/chat.html", "r")
    services_status.filesystem = {
        healthy = file_check ~= nil,
        error = file_check and nil or "Cannot read static files"
    }
    if file_check then file_check:close() end
    
    view.render_health_check(services_status)
end

-- Export artifacts endpoint
function _M.handle_export_artifacts()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    local args = ngx.req.get_uri_args()
    local chat_id = args.chat_id
    local format = args.format or "json"
    
    if not chat_id or chat_id == "" then
        return view.render_api_error(400, "Missing chat_id parameter")
    end
    
    if not utils.is_valid_chat_id(chat_id) then
        return view.render_api_error(400, "Invalid chat_id format")
    end
    
    local chat_artifacts, err = redis.get_chat_artifacts(chat_id)
    if not chat_artifacts then
        return view.render_api_error(500, "Failed to load artifacts", err)
    end
    
    local export_options = {
        include_stats = args.include_stats == "true",
        metadata = {
            chat_id = chat_id,
            export_date = os.date("%Y-%m-%d %H:%M:%S")
        }
    }
    
    local exported_content, mime_type, filename = artifacts.export_artifacts(chat_artifacts, format, export_options)
    if not exported_content then
        return view.render_api_error(400, "Export failed", mime_type) -- mime_type contains error message
    end
    
    -- Set download headers
    ngx.header["Content-Type"] = mime_type
    ngx.header["Content-Disposition"] = "attachment; filename=" .. filename
    ngx.header["Content-Length"] = #exported_content
    utils.set_cors_headers()
    
    ngx.say(exported_content)
    
    utils.log_info("chat_handler", "export_artifacts", {
        chat_id = chat_id,
        format = format,
        artifact_count = #chat_artifacts,
        export_size = #exported_content
    })
end

-- API documentation endpoint
function _M.handle_api_docs()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    view.render_api_docs()
end

-- System status endpoint
function _M.handle_status()
    if ngx.req.get_method() ~= "GET" then
        return view.handle_method_not_allowed({"GET"})
    end
    
    view.render_status_page()
end

-- Default 404 handler
function _M.handle_not_found()
    view.render_error_page(404, "Page Not Found", "The requested resource could not be found.")
end

-- Global error handler
function _M.handle_error(error_msg, status_code)
    status_code = status_code or 500
    
    utils.log_error("chat_handler", "global_error", error_msg, {
        status = status_code,
        uri = ngx.var.uri,
        method = ngx.var.request_method
    })
    
    if status_code >= 500 then
        view.render_error_page(status_code, "Internal Server Error", "An unexpected error occurred.", error_msg)
    else
        view.render_api_error(status_code, "Request Error", error_msg)
    end
end

return _M
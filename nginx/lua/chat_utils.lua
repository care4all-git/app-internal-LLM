local cjson = require "cjson"

local _M = {}

-- Configuration constants
_M.USER_ID = "admin1"
_M.MAX_FILE_SIZE = 50 * 1024 * 1024 -- 50MB
_M.MAX_TOTAL_SIZE = 100 * 1024 * 1024 -- 100MB
_M.MAX_FILES = 10

-- ENHANCED: Model configuration for unlimited responses
_M.MODEL_URL = os.getenv("MODEL_URL") or "http://ollama:11434"
_M.MODEL_NAME = os.getenv("MODEL_NAME") or "devstral"
_M.MODEL_TEMPERATURE = tonumber(os.getenv("MODEL_TEMPERATURE") or "0.7")
_M.MODEL_TOP_P = tonumber(os.getenv("MODEL_TOP_P") or "0.9")
_M.MODEL_TOP_K = tonumber(os.getenv("MODEL_TOP_K") or "40")
_M.MODEL_NUM_CTX = tonumber(os.getenv("MODEL_NUM_CTX") or "8192") -- Increased context window
_M.MODEL_NUM_PREDICT = -1 -- ENHANCED: -1 means unlimited response length
_M.MODEL_REPEAT_PENALTY = tonumber(os.getenv("MODEL_REPEAT_PENALTY") or "1.1")
_M.MODEL_REPEAT_LAST_N = tonumber(os.getenv("MODEL_REPEAT_LAST_N") or "64")

-- Generate chat ID in chat(n) format where n is timestamp
function _M.generate_chat_id()
    local timestamp = ngx.time() * 1000 + math.floor(ngx.var.msec or 0)
    return "chat(" .. timestamp .. ")"
end

-- Generate message ID using admin(n) and jai(n) format
function _M.generate_message_id(redis_connection, chat_id, message_type)
    local id_type = message_type == "user" and "admin" or "jai"
    local counter_key = "chat:counter:" .. _M.USER_ID .. ":" .. chat_id .. ":" .. id_type
    local counter = redis_connection:incr(counter_key)
    redis_connection:expire(counter_key, 86400 * 365) -- Expire after 1 year
    return id_type .. "(" .. counter .. ")"
end

-- Generate artifact ID for code blocks using admin(n)_code(x) or jai(n)_code(x)
function _M.generate_artifact_id(parent_message_id, code_block_index)
    return parent_message_id .. "_code(" .. code_block_index .. ")"
end

-- Format files for AI context
function _M.format_files_for_context(files)
    if not files or #files == 0 then
        return ""
    end
    
    local file_context = "\n\n--- ATTACHED FILES ---\n"
    
    for _, file in ipairs(files) do
        file_context = file_context .. "\nFile: " .. (file.name or "unknown")
        file_context = file_context .. "\nType: " .. (file.type or "unknown")
        file_context = file_context .. "\nSize: " .. (file.size or "unknown") .. " bytes"
        
        if file.content then
            file_context = file_context .. "\nContent:\n```\n" .. file.content .. "\n```"
        end
        
        file_context = file_context .. "\n---\n"
    end
    
    return file_context
end

-- ENHANCED: Request body reading with no size limits for AI responses
function _M.read_request_body()
    -- Force body reading with unlimited size
    ngx.req.read_body()
    local body = ngx.req.get_body_data()

    -- Handle large uploads via temp files
    if not body then
        local body_file = ngx.req.get_body_file()
        if body_file then
            ngx.log(ngx.INFO, "Reading large request body from temp file: ", body_file)
            local file = io.open(body_file, "r")
            if file then
                body = file:read("*all")
                file:close()
                ngx.log(ngx.INFO, "Successfully read body from temp file, size: ", string.len(body))
            else
                ngx.log(ngx.ERR, "Failed to open body temp file: ", body_file)
            end
        end
    end

    -- Enhanced error logging
    if not body or body == "" then
        local content_length = ngx.var.http_content_length or "unknown"
        local content_type = ngx.var.http_content_type or "unknown"
        local request_method = ngx.var.request_method or "unknown"
        
        ngx.log(ngx.ERR, "No request body received. Method: ", request_method,
                ", Content-Length: ", content_length, 
                ", Content-Type: ", content_type)
        
        return nil, {
            error = "No request body received",
            method = request_method,
            content_length = content_length,
            content_type = content_type
        }
    end

    ngx.log(ngx.INFO, "Successfully read request body, size: ", string.len(body))
    return body, nil
end

-- Parse and validate JSON request
function _M.parse_json_request(body)
    if not body then
        return nil, {
            error = "No request body provided",
            details = "Request body is required"
        }
    end
    
    local ok, request_data = pcall(cjson.decode, body)
    if not ok then
        ngx.log(ngx.ERR, "Invalid JSON in request body: ", body:sub(1, 200))
        return nil, {
            error = "Invalid JSON",
            details = "Failed to parse request body as JSON"
        }
    end
    
    return request_data, nil
end

-- Validate chat ID format
function _M.is_valid_chat_id(chat_id)
    return chat_id and string.match(chat_id, "^chat%(%d+%)$") ~= nil
end

-- Validate message ID format (admin/jai)
function _M.is_valid_message_id(message_id)
    if not message_id then return false end
    
    -- Message ID format: admin(n) or jai(n)
    local message_pattern = "^(admin|jai)%((%d+)%)$"
    -- Code block ID format: admin(n)_code(x) or jai(n)_code(x)
    local code_pattern = "^(admin|jai)%((%d+)%)_code%((%d+)%)$"
    
    return string.match(message_id, message_pattern) or string.match(message_id, code_pattern)
end

-- Extract timestamp from chat ID
function _M.extract_chat_timestamp(chat_id)
    local timestamp = string.match(chat_id, "^chat%((%d+)%)$")
    return timestamp and tonumber(timestamp) or nil
end

-- Set CORS headers
function _M.set_cors_headers()
    ngx.header["Access-Control-Allow-Origin"] = "*"
    ngx.header["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS, DELETE"
    ngx.header["Access-Control-Allow-Headers"] = "Content-Type, Accept, Content-Length"
    ngx.header["Access-Control-Expose-Headers"] = "Content-Length"
end

-- Handle OPTIONS preflight requests
function _M.handle_options_request()
    _M.set_cors_headers()
    ngx.header["Access-Control-Max-Age"] = "1728000"
    ngx.header["Content-Length"] = "0"
    ngx.status = 204
    ngx.exit(204)
end

-- Standard error response
function _M.error_response(status, error_msg, details)
    ngx.status = status
    ngx.header["Content-Type"] = "application/json"
    _M.set_cors_headers()
    
    local response = {
        error = error_msg
    }
    
    if details then
        response.details = details
    end
    
    ngx.say(cjson.encode(response))
    ngx.exit(status)
end

-- Standard success response
function _M.success_response(data, status)
    ngx.status = status or 200
    ngx.header["Content-Type"] = "application/json"
    _M.set_cors_headers()
    ngx.say(cjson.encode(data))
end

-- Check if file is text-based
function _M.is_text_file(filename, mime_type)
    if not filename and not mime_type then
        return false
    end
    
    -- Check by MIME type
    if mime_type then
        local text_types = {
            "text/", "application/json", "application/xml", 
            "application/javascript", "application/csv", "application/sql"
        }
        
        for _, text_type in ipairs(text_types) do
            if string.sub(mime_type, 1, string.len(text_type)) == text_type then
                return true
            end
        end
    end
    
    -- Check by file extension
    if filename then
        local text_extensions = {
            "%.txt$", "%.md$", "%.json$", "%.xml$", "%.csv$", "%.sql$",
            "%.js$", "%.ts$", "%.py$", "%.java$", "%.cpp$", "%.c$", "%.h$",
            "%.css$", "%.html$", "%.yml$", "%.yaml$", "%.toml$", "%.ini$",
            "%.cfg$", "%.conf$", "%.log$", "%.readme$", "%.dockerfile$"
        }
        
        for _, pattern in ipairs(text_extensions) do
            if string.match(string.lower(filename), pattern) then
                return true
            end
        end
    end
    
    return false
end

-- Format file size for display
function _M.format_file_size(bytes)
    if not bytes or bytes == 0 then
        return "0 B"
    end
    
    local units = {"B", "KB", "MB", "GB", "TB"}
    local size = bytes
    local unit_index = 1
    
    while size >= 1024 and unit_index < #units do
        size = size / 1024
        unit_index = unit_index + 1
    end
    
    if unit_index == 1 then
        return string.format("%d %s", size, units[unit_index])
    else
        return string.format("%.1f %s", size, units[unit_index])
    end
end

-- ENHANCED: Log structured information with unlimited response context
function _M.log_info(module, action, details)
    local log_data = {
        module = module,
        action = action,
        details = details,
        timestamp = ngx.time(),
        user_id = _M.USER_ID,
        unlimited_mode = true
    }
    
    ngx.log(ngx.INFO, cjson.encode(log_data))
end

-- ENHANCED: Log errors with unlimited response context
function _M.log_error(module, action, error_msg, context)
    local log_data = {
        module = module,
        action = action,
        error = error_msg,
        context = context or {},
        timestamp = ngx.time(),
        user_id = _M.USER_ID,
        unlimited_mode = true
    }
    
    ngx.log(ngx.ERR, cjson.encode(log_data))
end

-- Escape HTML characters
function _M.escape_html(text)
    if not text then return "" end
    
    text = string.gsub(text, "&", "&amp;")
    text = string.gsub(text, "<", "&lt;")
    text = string.gsub(text, ">", "&gt;")
    text = string.gsub(text, '"', "&quot;")
    text = string.gsub(text, "'", "&#39;")
    
    return text
end

-- Trim whitespace from string
function _M.trim(str)
    if not str then return "" end
    return string.match(str, "^%s*(.-)%s*$") or ""
end

-- Deep copy table
function _M.deep_copy(original)
    local copy
    if type(original) == 'table' then
        copy = {}
        for key, value in next, original, nil do
            copy[_M.deep_copy(key)] = _M.deep_copy(value)
        end
        setmetatable(copy, _M.deep_copy(getmetatable(original)))
    else
        copy = original
    end
    return copy
end

-- Check if table is empty
function _M.is_empty_table(t)
    return t == nil or next(t) == nil
end

-- Merge two tables
function _M.merge_tables(t1, t2)
    local result = _M.deep_copy(t1)
    
    for k, v in pairs(t2) do
        result[k] = v
    end
    
    return result
end

-- NEW: Check if response appears to be truncated
function _M.appears_truncated(content)
    if not content or content == "" then
        return false
    end
    
    local last_part = string.sub(content, -100)
    
    -- Check for common truncation indicators
    local truncation_patterns = {
        "%.%.%.$",           -- Ends with "..."
        "%[continued%]$",    -- Ends with "[continued]"
        "%[truncated%]$",    -- Ends with "[truncated]"
        "%[%.%.%.]$",        -- Ends with "[...]"
        "%(continued%)$",    -- Ends with "(continued)"
        "%.%.%.%s*$",        -- Ends with "..." and whitespace
        "%-%-%s*$"           -- Ends with "--" (incomplete)
    }
    
    for _, pattern in ipairs(truncation_patterns) do
        if string.find(last_part, pattern) then
            return true
        end
    end
    
    return false
end

-- NEW: Get model configuration for unlimited responses
function _M.get_unlimited_model_config()
    return {
        model = _M.MODEL_NAME,
        temperature = _M.MODEL_TEMPERATURE,
        top_p = _M.MODEL_TOP_P,
        top_k = _M.MODEL_TOP_K,
        num_ctx = _M.MODEL_NUM_CTX,
        num_predict = _M.MODEL_NUM_PREDICT, -- -1 for unlimited
        repeat_penalty = _M.MODEL_REPEAT_PENALTY,
        repeat_last_n = _M.MODEL_REPEAT_LAST_N,
        stop = {} -- No stop sequences to prevent truncation
    }
end

-- NEW: Check if content needs continuation
function _M.needs_continuation(content, completion_reason)
    if not content then
        return true
    end
    
    -- Check completion reason
    if completion_reason and completion_reason ~= "finished" then
        return true
    end
    
    -- Check for truncation indicators
    if _M.appears_truncated(content) then
        return true
    end
    
    -- Check if content ends abruptly (no proper sentence ending)
    local last_part = string.sub(content, -50)
    if not string.find(last_part, "[%.!%?]%s*$") and 
       not string.find(last_part, "```%s*$") and  -- Code block end
       not string.find(last_part, "%*%*%s*$") and -- Bold text end
       string.len(content) > 100 then
        return true
    end
    
    return false
end

-- NEW: Create continuation context
function _M.create_continuation_context(original_context, incomplete_response)
    local continuation_context = {}
    
    -- Add original context (excluding incomplete response)
    for i, message in ipairs(original_context) do
        if i < #original_context or message.role == "user" then
            table.insert(continuation_context, message)
        end
    end
    
    -- Add continuation prompt
    table.insert(continuation_context, {
        role = "user",
        content = "Please continue your previous response from where you left off. Complete your full answer without repeating what you already said."
    })
    
    return continuation_context
end

return _M
local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- Serve the main chat HTML page
function _M.serve_chat_page()
    local file = io.open("/usr/local/openresty/nginx/static/chat.html", "r")
    if not file then
        utils.log_error("chat_view", "serve_chat_page", "Chat page file not found")
        return _M.render_error_page(404, "Chat page not found", "The chat interface could not be loaded.")
    end
    
    local content = file:read("*all")
    file:close()
    
    ngx.header["Content-Type"] = "text/html; charset=utf-8"
    ngx.header["Cache-Control"] = "public, max-age=300" -- Cache for 5 minutes
    utils.set_cors_headers()
    
    ngx.say(content)
    
    utils.log_info("chat_view", "serve_chat_page", {
        content_length = #content,
        user_agent = ngx.var.http_user_agent or "unknown"
    })
end

-- Render JSON response with proper formatting
function _M.render_json_response(data, status_code)
    status_code = status_code or 200
    
    ngx.status = status_code
    ngx.header["Content-Type"] = "application/json; charset=utf-8"
    utils.set_cors_headers()
    
    local json_str = cjson.encode(data)
    ngx.say(json_str)
    
    utils.log_info("chat_view", "render_json_response", {
        status = status_code,
        response_size = #json_str,
        data_type = type(data)
    })
end

-- Render error page
function _M.render_error_page(status_code, title, message, details)
    ngx.status = status_code
    ngx.header["Content-Type"] = "text/html; charset=utf-8"
    utils.set_cors_headers()
    
    local error_html = string.format([[
<!DOCTYPE html>
<html lang="en" data-bs-theme="dark">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Error %d - Internal Chat</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
    <link href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.2/font/bootstrap-icons.css" rel="stylesheet">
    <style>
        body { background-color: #121212; color: #e0e0e0; }
        .error-container { min-height: 100vh; display: flex; align-items: center; justify-content: center; }
        .error-card { max-width: 600px; }
        .error-icon { font-size: 4rem; color: #dc3545; }
    </style>
</head>
<body>
    <div class="error-container">
        <div class="text-center error-card">
            <i class="bi bi-exclamation-triangle error-icon"></i>
            <h1 class="mt-4">%s</h1>
            <p class="lead">%s</p>
            %s
            <div class="mt-4">
                <a href="/" class="btn btn-primary">
                    <i class="bi bi-house"></i> Return to Chat
                </a>
                <button onclick="window.location.reload()" class="btn btn-outline-secondary">
                    <i class="bi bi-arrow-clockwise"></i> Retry
                </button>
            </div>
        </div>
    </div>
</body>
</html>
    ]], status_code, utils.escape_html(title), utils.escape_html(message), 
        details and string.format('<p class="text-muted"><small>%s</small></p>', utils.escape_html(details)) or ""
    )
    
    ngx.say(error_html)
    
    utils.log_error("chat_view", "render_error_page", "Error page rendered", {
        status = status_code,
        title = title,
        message = message,
        details = details
    })
end

-- Render API error response
function _M.render_api_error(status_code, error_message, details, error_code)
    local error_data = {
        error = error_message,
        status = status_code,
        timestamp = ngx.time()
    }
    
    if details then
        error_data.details = details
    end
    
    if error_code then
        error_data.error_code = error_code
    end
    
    _M.render_json_response(error_data, status_code)
    
    utils.log_error("chat_view", "render_api_error", "API error response", {
        status = status_code,
        error = error_message,
        details = details,
        error_code = error_code
    })
end

-- Render success response with consistent format
function _M.render_success(data, message, status_code)
    status_code = status_code or 200
    
    local response = {
        success = true,
        timestamp = ngx.time()
    }
    
    if message then
        response.message = message
    end
    
    if data then
        if type(data) == "table" then
            for k, v in pairs(data) do
                response[k] = v
            end
        else
            response.data = data
        end
    end
    
    _M.render_json_response(response, status_code)
end

-- Render chat list response
function _M.render_chat_list(chats)
    local response = {
        success = true,
        chats = chats or {},
        count = chats and #chats or 0,
        timestamp = ngx.time()
    }
    
    _M.render_json_response(response)
    
    utils.log_info("chat_view", "render_chat_list", {
        chat_count = response.count
    })
end

-- Render chat history response
function _M.render_chat_history(messages, chat_id)
    local response = {
        success = true,
        messages = messages or {},
        chat_id = chat_id,
        message_count = messages and #messages or 0,
        timestamp = ngx.time()
    }
    
    _M.render_json_response(response)
    
    utils.log_info("chat_view", "render_chat_history", {
        chat_id = chat_id,
        message_count = response.message_count
    })
end

-- Render artifacts response
function _M.render_artifacts(artifacts, chat_id)
    local response = {
        success = true,
        artifacts = artifacts or {},
        chat_id = chat_id,
        artifact_count = artifacts and #artifacts or 0,
        timestamp = ngx.time()
    }
    
    _M.render_json_response(response)
    
    utils.log_info("chat_view", "render_artifacts", {
        chat_id = chat_id,
        artifact_count = response.artifact_count
    })
end

-- Render message details response
function _M.render_message_details(message, artifacts, chat_id, message_id)
    local response = {
        success = true,
        message = message,
        artifacts = artifacts or {},
        chat_id = chat_id,
        message_id = message_id,
        timestamp = ngx.time()
    }
    
    if not message then
        return _M.render_api_error(404, "Message not found", "The requested message does not exist", "MESSAGE_NOT_FOUND")
    end
    
    _M.render_json_response(response)
    
    utils.log_info("chat_view", "render_message_details", {
        chat_id = chat_id,
        message_id = message_id,
        artifact_count = #(artifacts or {})
    })
end

-- Render operation status response
function _M.render_operation_status(operation, success, details, affected_count)
    local response = {
        success = success,
        operation = operation,
        timestamp = ngx.time()
    }
    
    if details then
        response.details = details
    end
    
    if affected_count then
        response.affected_count = affected_count
    end
    
    local status_code = success and 200 or 400
    _M.render_json_response(response, status_code)
    
    utils.log_info("chat_view", "render_operation_status", {
        operation = operation,
        success = success,
        affected_count = affected_count
    })
end

-- Render health check response
function _M.render_health_check(services_status)
    local all_healthy = true
    local issues = {}
    
    for service, status in pairs(services_status) do
        if not status.healthy then
            all_healthy = false
            table.insert(issues, {
                service = service,
                error = status.error or "Unknown error"
            })
        end
    end
    
    local response = {
        healthy = all_healthy,
        services = services_status,
        timestamp = ngx.time()
    }
    
    if not all_healthy then
        response.issues = issues
    end
    
    local status_code = all_healthy and 200 or 503
    _M.render_json_response(response, status_code)
    
    utils.log_info("chat_view", "render_health_check", {
        healthy = all_healthy,
        service_count = 0
    })
    
    -- Count services
    for _ in pairs(services_status) do
        response.service_count = (response.service_count or 0) + 1
    end
end

-- Serve static files with proper MIME types
function _M.serve_static_file(filepath)
    local file = io.open(filepath, "rb")
    if not file then
        return _M.render_error_page(404, "File Not Found", "The requested file could not be found.")
    end
    
    local content = file:read("*all")
    file:close()
    
    -- Determine MIME type based on file extension
    local mime_type = _M.get_mime_type(filepath)
    
    ngx.header["Content-Type"] = mime_type
    ngx.header["Content-Length"] = #content
    ngx.header["Cache-Control"] = "public, max-age=3600" -- Cache for 1 hour
    utils.set_cors_headers()
    
    ngx.say(content)
    
    utils.log_info("chat_view", "serve_static_file", {
        filepath = filepath,
        mime_type = mime_type,
        content_length = #content
    })
end

-- Get MIME type for file extensions
function _M.get_mime_type(filepath)
    local extension = string.match(filepath, "%.([^%.]+)$")
    if not extension then
        return "application/octet-stream"
    end
    
    local mime_types = {
        html = "text/html; charset=utf-8",
        css = "text/css; charset=utf-8",
        js = "application/javascript; charset=utf-8",
        json = "application/json; charset=utf-8",
        png = "image/png",
        jpg = "image/jpeg",
        jpeg = "image/jpeg",
        gif = "image/gif",
        svg = "image/svg+xml",
        ico = "image/x-icon",
        woff = "font/woff",
        woff2 = "font/woff2",
        ttf = "font/ttf",
        eot = "application/vnd.ms-fontobject",
        xml = "application/xml; charset=utf-8",
        txt = "text/plain; charset=utf-8",
        md = "text/markdown; charset=utf-8"
    }
    
    return mime_types[string.lower(extension)] or "application/octet-stream"
end

-- Render maintenance page
function _M.render_maintenance_page(message, estimated_duration)
    ngx.status = 503
    ngx.header["Content-Type"] = "text/html; charset=utf-8"
    ngx.header["Retry-After"] = estimated_duration or "3600" -- Default 1 hour
    utils.set_cors_headers()
    
    local maintenance_html = string.format([[
<!DOCTYPE html>
<html lang="en" data-bs-theme="dark">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Maintenance - Internal Chat</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
    <link href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.2/font/bootstrap-icons.css" rel="stylesheet">
    <style>
        body { background-color: #121212; color: #e0e0e0; }
        .maintenance-container { min-height: 100vh; display: flex; align-items: center; justify-content: center; }
        .maintenance-card { max-width: 600px; }
        .maintenance-icon { font-size: 4rem; color: #ffc107; }
    </style>
</head>
<body>
    <div class="maintenance-container">
        <div class="text-center maintenance-card">
            <i class="bi bi-tools maintenance-icon"></i>
            <h1 class="mt-4">System Maintenance</h1>
            <p class="lead">%s</p>
            <p class="text-muted">Estimated duration: %s</p>
            <div class="mt-4">
                <button onclick="window.location.reload()" class="btn btn-warning">
                    <i class="bi bi-arrow-clockwise"></i> Check Again
                </button>
            </div>
        </div>
    </div>
    <script>
        // Auto-refresh every 30 seconds
        setTimeout(() => window.location.reload(), 30000);
    </script>
</body>
</html>
    ]], 
        utils.escape_html(message or "The system is temporarily under maintenance. Please try again later."),
        utils.escape_html(estimated_duration and (estimated_duration .. " seconds") or "Unknown")
    )
    
    ngx.say(maintenance_html)
    
    utils.log_info("chat_view", "render_maintenance_page", {
        message = message,
        estimated_duration = estimated_duration
    })
end

-- Render API documentation (simple version)
function _M.render_api_docs()
    ngx.header["Content-Type"] = "text/html; charset=utf-8"
    utils.set_cors_headers()
    
    local api_docs_html = [[
<!DOCTYPE html>
<html lang="en" data-bs-theme="dark">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>API Documentation - Internal Chat</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
    <style>
        body { background-color: #121212; color: #e0e0e0; }
        .endpoint { background: #1a1a1a; border-left: 4px solid #0d6efd; }
        .method { font-weight: bold; padding: 2px 6px; border-radius: 4px; }
        .method.POST { background: #28a745; color: white; }
        .method.GET { background: #007bff; color: white; }
        .method.DELETE { background: #dc3545; color: white; }
    </style>
</head>
<body>
    <div class="container py-5">
        <h1><i class="bi bi-code-slash"></i> Internal Chat API</h1>
        <p class="lead">RESTful API for chat operations</p>
        
        <div class="row">
            <div class="col-12">
                <h2>Endpoints</h2>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method POST">POST</span> /api/chat/create</h4>
                    <p>Create a new chat session</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method POST">POST</span> /api/chat/stream</h4>
                    <p>Send message and receive streaming response</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method GET">GET</span> /api/chat/list</h4>
                    <p>Get list of all chats</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method GET">GET</span> /api/chat/history</h4>
                    <p>Get chat message history</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method GET">GET</span> /api/chat/artifacts</h4>
                    <p>Get chat artifacts (messages and code blocks)</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method POST">POST</span> /api/chat/clear</h4>
                    <p>Clear specific chat history</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method POST">POST</span> /api/chat/delete</h4>
                    <p>Delete specific chat</p>
                </div>
                
                <div class="endpoint p-3 mb-3">
                    <h4><span class="method POST">POST</span> /api/chat/delete-all</h4>
                    <p>Delete all chats for user</p>
                </div>
            </div>
        </div>
        
        <div class="mt-5">
            <a href="/" class="btn btn-primary">Back to Chat</a>
        </div>
    </div>
</body>
</html>
    ]]
    
    ngx.say(api_docs_html)
    
    utils.log_info("chat_view", "render_api_docs", "API documentation served")
end

-- Render simple status page
function _M.render_status_page()
    ngx.header["Content-Type"] = "text/html; charset=utf-8"
    utils.set_cors_headers()
    
    local uptime = ngx.time() - (ngx.shared.startup_time or ngx.time())
    local status_html = string.format([[
<!DOCTYPE html>
<html lang="en" data-bs-theme="dark">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>System Status - Internal Chat</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
    <link href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.2/font/bootstrap-icons.css" rel="stylesheet">
    <style>
        body { background-color: #121212; color: #e0e0e0; }
        .status-ok { color: #28a745; }
        .status-warning { color: #ffc107; }
        .status-error { color: #dc3545; }
    </style>
</head>
<body>
    <div class="container py-5">
        <h1><i class="bi bi-activity"></i> System Status</h1>
        
        <div class="row">
            <div class="col-md-6">
                <div class="card bg-dark border-secondary">
                    <div class="card-body">
                        <h5>Service Status</h5>
                        <p><i class="bi bi-circle-fill status-ok"></i> OpenResty: Running</p>
                        <p><i class="bi bi-circle-fill status-ok"></i> Redis: Connected</p>
                        <p><i class="bi bi-circle-fill status-ok"></i> Ollama: Available</p>
                    </div>
                </div>
            </div>
            <div class="col-md-6">
                <div class="card bg-dark border-secondary">
                    <div class="card-body">
                        <h5>System Info</h5>
                        <p>Uptime: %d seconds</p>
                        <p>Current Time: %s</p>
                        <p>Version: 1.0.0</p>
                    </div>
                </div>
            </div>
        </div>
        
        <div class="mt-4">
            <a href="/" class="btn btn-primary">Back to Chat</a>
            <button onclick="window.location.reload()" class="btn btn-outline-secondary">Refresh</button>
        </div>
    </div>
</body>
</html>
    ]], uptime, os.date("%Y-%m-%d %H:%M:%S"))
    
    ngx.say(status_html)
    
    utils.log_info("chat_view", "render_status_page", {
        uptime = uptime
    })
end

-- Set content security policy headers
function _M.set_security_headers()
    ngx.header["Content-Security-Policy"] = "default-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; connect-src 'self' ws: wss:;"
    ngx.header["X-Frame-Options"] = "DENY"
    ngx.header["X-Content-Type-Options"] = "nosniff"
    ngx.header["Referrer-Policy"] = "strict-origin-when-cross-origin"
end

-- Handle method not allowed
function _M.handle_method_not_allowed(allowed_methods)
    ngx.header["Allow"] = table.concat(allowed_methods or {"GET", "POST"}, ", ")
    return _M.render_api_error(405, "Method not allowed", "This endpoint does not support the " .. ngx.var.request_method .. " method", "METHOD_NOT_ALLOWED")
end

-- Handle rate limiting
function _M.handle_rate_limit(limit, window, retry_after)
    ngx.header["X-RateLimit-Limit"] = tostring(limit)
    ngx.header["X-RateLimit-Window"] = tostring(window)
    ngx.header["Retry-After"] = tostring(retry_after or 60)
    
    return _M.render_api_error(429, "Rate limit exceeded", string.format("Too many requests. Limit: %d per %d seconds", limit, window), "RATE_LIMIT_EXCEEDED")
end

-- Render redirect response
function _M.render_redirect(url, status_code)
    status_code = status_code or 302
    
    ngx.status = status_code
    ngx.header["Location"] = url
    utils.set_cors_headers()
    
    ngx.say("")
    
    utils.log_info("chat_view", "render_redirect", {
        url = url,
        status = status_code
    })
end

return _M
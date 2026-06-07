local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- Extract and save code blocks from content and create artifacts with proper IDs
function _M.extract_and_save_code_blocks(redis_connection, chat_id, message_id, content)
    local artifacts = {}
    local code_block_index = 0
    
    -- Match code blocks: ```language\ncode\n```
    for lang, code in string.gmatch(content, "```([%w]*)\n(.-)\n```") do
        code_block_index = code_block_index + 1
        local artifact_id = utils.generate_artifact_id(message_id, code_block_index)
        
        -- Save artifact directly using Redis connection
        local artifact_data = {
            id = artifact_id,
            parent_id = message_id,
            type = "code_block",
            code = code,
            language = lang or "",
            metadata = {
                extracted_from_response = true,
                block_index = code_block_index,
                extraction_timestamp = ngx.time()
            },
            timestamp = ngx.time(),
            chat_id = chat_id
        }
        
        -- Save individual artifact
        local artifact_key = "artifact:" .. utils.USER_ID .. ":" .. chat_id .. ":" .. artifact_id
        redis_connection:set(artifact_key, require("cjson").encode(artifact_data))
        redis_connection:expire(artifact_key, 86400 * 365)
        
        -- Add to chat artifacts list
        local chat_artifacts_key = "chat:artifacts:" .. utils.USER_ID .. ":" .. chat_id
        redis_connection:lpush(chat_artifacts_key, artifact_id)
        redis_connection:expire(chat_artifacts_key, 86400 * 365)
        
        table.insert(artifacts, artifact_id)
        
        utils.log_info("chat_artifacts", "extract_code_block", {
            chat_id = chat_id,
            artifact_id = artifact_id,
            language = lang,
            code_length = #code
        })
    end
    
    utils.log_info("chat_artifacts", "extract_and_save_code_blocks", {
        chat_id = chat_id,
        message_id = message_id,
        extracted_count = #artifacts
    })
    
    return artifacts
end

-- Get all artifacts for a chat with filtering options
function _M.get_artifacts_with_filter(redis, chat_id, filter_options)
    local artifacts, err = redis.get_chat_artifacts(chat_id)
    if not artifacts then
        return nil, err
    end
    
    -- Apply filters if provided
    if filter_options then
        artifacts = _M.apply_artifact_filters(artifacts, filter_options)
    end
    
    -- Sort artifacts
    local sort_by = filter_options and filter_options.sort_by or "timestamp"
    local sort_order = filter_options and filter_options.sort_order or "desc"
    
    _M.sort_artifacts(artifacts, sort_by, sort_order)
    
    utils.log_info("chat_artifacts", "get_artifacts_with_filter", {
        chat_id = chat_id,
        total_count = #artifacts,
        filter_options = filter_options
    })
    
    return artifacts, nil
end

-- Apply filters to artifact list
function _M.apply_artifact_filters(artifacts, filters)
    local filtered = {}
    
    for _, artifact in ipairs(artifacts) do
        local include = true
        
        -- Filter by type
        if filters.type and artifact.type ~= filters.type then
            include = false
        end
        
        -- Filter by language (for code blocks)
        if filters.language and artifact.language ~= filters.language then
            include = false
        end
        
        -- Filter by parent message ID
        if filters.parent_id and artifact.parent_id ~= filters.parent_id then
            include = false
        end
        
        -- Filter by date range
        if filters.from_date and artifact.timestamp < filters.from_date then
            include = false
        end
        
        if filters.to_date and artifact.timestamp > filters.to_date then
            include = false
        end
        
        -- Filter by content search
        if filters.search_content then
            local content = artifact.content or artifact.code or ""
            if not string.find(string.lower(content), string.lower(filters.search_content)) then
                include = false
            end
        end
        
        if include then
            table.insert(filtered, artifact)
        end
    end
    
    return filtered
end

-- Sort artifacts by different criteria
function _M.sort_artifacts(artifacts, sort_by, sort_order)
    sort_order = sort_order or "desc"
    
    local comparison_func
    
    if sort_by == "timestamp" then
        comparison_func = function(a, b)
            local a_time = a.timestamp or 0
            local b_time = b.timestamp or 0
            if sort_order == "desc" then
                return a_time > b_time
            else
                return a_time < b_time
            end
        end
    elseif sort_by == "id" then
        comparison_func = function(a, b)
            local a_id = a.id or ""
            local b_id = b.id or ""
            if sort_order == "desc" then
                return a_id > b_id
            else
                return a_id < b_id
            end
        end
    elseif sort_by == "type" then
        comparison_func = function(a, b)
            local a_type = a.type or ""
            local b_type = b.type or ""
            if sort_order == "desc" then
                return a_type > b_type
            else
                return a_type < b_type
            end
        end
    elseif sort_by == "size" then
        comparison_func = function(a, b)
            local a_size = #(a.content or a.code or "")
            local b_size = #(b.content or b.code or "")
            if sort_order == "desc" then
                return a_size > b_size
            else
                return a_size < b_size
            end
        end
    else
        -- Default to timestamp
        comparison_func = function(a, b)
            local a_time = a.timestamp or 0
            local b_time = b.timestamp or 0
            if sort_order == "desc" then
                return a_time > b_time
            else
                return a_time < b_time
            end
        end
    end
    
    -- Add safety check to ensure we have artifacts to sort
    if artifacts and #artifacts > 1 then
        table.sort(artifacts, comparison_func)
    end
end

-- Get artifact statistics
function _M.get_artifact_statistics(artifacts)
    local stats = {
        total = #artifacts,
        by_type = {},
        by_language = {},
        total_size = 0,
        date_range = {
            earliest = nil,
            latest = nil
        }
    }
    
    for _, artifact in ipairs(artifacts) do
        -- Count by type
        local artifact_type = artifact.type or "unknown"
        stats.by_type[artifact_type] = (stats.by_type[artifact_type] or 0) + 1
        
        -- Count by language (for code blocks)
        if artifact.language and artifact.language ~= "" then
            stats.by_language[artifact.language] = (stats.by_language[artifact.language] or 0) + 1
        end
        
        -- Calculate total size
        local content = artifact.content or artifact.code or ""
        stats.total_size = stats.total_size + #content
        
        -- Track date range
        if artifact.timestamp then
            if not stats.date_range.earliest or artifact.timestamp < stats.date_range.earliest then
                stats.date_range.earliest = artifact.timestamp
            end
            if not stats.date_range.latest or artifact.timestamp > stats.date_range.latest then
                stats.date_range.latest = artifact.timestamp
            end
        end
    end
    
    utils.log_info("chat_artifacts", "get_artifact_statistics", {
        total = stats.total,
        total_size = stats.total_size,
        type_count = 0,
        language_count = 0
    })
    
    -- Count types and languages
    for _ in pairs(stats.by_type) do
        stats.type_count = (stats.type_count or 0) + 1
    end
    for _ in pairs(stats.by_language) do
        stats.language_count = (stats.language_count or 0) + 1
    end
    
    return stats
end

-- Export artifacts to different formats
function _M.export_artifacts(artifacts, format, options)
    format = format or "json"
    options = options or {}
    
    if format == "json" then
        return _M.export_artifacts_json(artifacts, options)
    elseif format == "markdown" then
        return _M.export_artifacts_markdown(artifacts, options)
    elseif format == "text" then
        return _M.export_artifacts_text(artifacts, options)
    else
        return nil, "Unsupported export format: " .. format
    end
end

-- Export artifacts as JSON
function _M.export_artifacts_json(artifacts, options)
    local export_data = {
        export_timestamp = ngx.time(),
        export_format = "json",
        artifact_count = #artifacts,
        artifacts = artifacts
    }
    
    if options.include_stats then
        export_data.statistics = _M.get_artifact_statistics(artifacts)
    end
    
    if options.metadata then
        export_data.metadata = options.metadata
    end
    
    local json_str = cjson.encode(export_data)
    
    utils.log_info("chat_artifacts", "export_artifacts_json", {
        artifact_count = #artifacts,
        export_size = #json_str
    })
    
    return json_str, "application/json", "artifacts_export.json"
end

-- Export artifacts as Markdown
function _M.export_artifacts_markdown(artifacts, options)
    local lines = {
        "# Chat Artifacts Export",
        "",
        "**Export Date:** " .. os.date("%Y-%m-%d %H:%M:%S", ngx.time()),
        "**Total Artifacts:** " .. #artifacts,
        ""
    }
    
    -- Add statistics if requested
    if options.include_stats then
        local stats = _M.get_artifact_statistics(artifacts)
        table.insert(lines, "## Statistics")
        table.insert(lines, "")
        table.insert(lines, "- **Total Size:** " .. utils.format_file_size(stats.total_size))
        
        table.insert(lines, "- **By Type:**")
        for type_name, count in pairs(stats.by_type) do
            table.insert(lines, "  - " .. type_name .. ": " .. count)
        end
        
        if next(stats.by_language) then
            table.insert(lines, "- **By Language:**")
            for lang, count in pairs(stats.by_language) do
                table.insert(lines, "  - " .. lang .. ": " .. count)
            end
        end
        table.insert(lines, "")
    end
    
    -- Add artifacts
    table.insert(lines, "## Artifacts")
    table.insert(lines, "")
    
    for i, artifact in ipairs(artifacts) do
        table.insert(lines, "### " .. (artifact.id or ("Artifact " .. i)))
        table.insert(lines, "")
        table.insert(lines, "- **Type:** " .. (artifact.type or "unknown"))
        table.insert(lines, "- **Timestamp:** " .. os.date("%Y-%m-%d %H:%M:%S", artifact.timestamp or 0))
        
        if artifact.language and artifact.language ~= "" then
            table.insert(lines, "- **Language:** " .. artifact.language)
        end
        
        if artifact.parent_id then
            table.insert(lines, "- **Parent:** " .. artifact.parent_id)
        end
        
        table.insert(lines, "")
        
        local content = artifact.content or artifact.code or ""
        if content ~= "" then
            if artifact.type == "code_block" and artifact.language then
                table.insert(lines, "```" .. artifact.language)
                table.insert(lines, content)
                table.insert(lines, "```")
            else
                table.insert(lines, "```")
                table.insert(lines, content)
                table.insert(lines, "```")
            end
        end
        
        table.insert(lines, "")
        table.insert(lines, "---")
        table.insert(lines, "")
    end
    
    local markdown_content = table.concat(lines, "\n")
    
    utils.log_info("chat_artifacts", "export_artifacts_markdown", {
        artifact_count = #artifacts,
        export_size = #markdown_content
    })
    
    return markdown_content, "text/markdown", "artifacts_export.md"
end

-- Export artifacts as plain text
function _M.export_artifacts_text(artifacts, options)
    local lines = {
        "Chat Artifacts Export",
        string.rep("=", 50),
        "",
        "Export Date: " .. os.date("%Y-%m-%d %H:%M:%S", ngx.time()),
        "Total Artifacts: " .. #artifacts,
        ""
    }
    
    for i, artifact in ipairs(artifacts) do
        table.insert(lines, "Artifact " .. i .. ": " .. (artifact.id or "unknown"))
        table.insert(lines, string.rep("-", 30))
        table.insert(lines, "Type: " .. (artifact.type or "unknown"))
        table.insert(lines, "Timestamp: " .. os.date("%Y-%m-%d %H:%M:%S", artifact.timestamp or 0))
        
        if artifact.language and artifact.language ~= "" then
            table.insert(lines, "Language: " .. artifact.language)
        end
        
        if artifact.parent_id then
            table.insert(lines, "Parent: " .. artifact.parent_id)
        end
        
        table.insert(lines, "")
        
        local content = artifact.content or artifact.code or ""
        if content ~= "" then
            table.insert(lines, "Content:")
            table.insert(lines, content)
        end
        
        table.insert(lines, "")
        table.insert(lines, string.rep("=", 50))
        table.insert(lines, "")
    end
    
    local text_content = table.concat(lines, "\n")
    
    utils.log_info("chat_artifacts", "export_artifacts_text", {
        artifact_count = #artifacts,
        export_size = #text_content
    })
    
    return text_content, "text/plain", "artifacts_export.txt"
end

-- Search artifacts by content
function _M.search_artifacts(artifacts, query, options)
    if not query or query == "" then
        return artifacts
    end
    
    query = string.lower(query)
    local results = {}
    options = options or {}
    
    for _, artifact in ipairs(artifacts) do
        local matches = false
        local match_info = {
            artifact = artifact,
            matches = {}
        }
        
        -- Search in artifact ID
        if string.find(string.lower(artifact.id or ""), query) then
            matches = true
            table.insert(match_info.matches, {
                field = "id",
                value = artifact.id
            })
        end
        
        -- Search in content/code
        local content = artifact.content or artifact.code or ""
        if string.find(string.lower(content), query) then
            matches = true
            
            -- Find context around match if requested
            if options.include_context then
                local context = _M.extract_search_context(content, query, options.context_length or 100)
                table.insert(match_info.matches, {
                    field = "content",
                    context = context
                })
            else
                table.insert(match_info.matches, {
                    field = "content",
                    value = "Content match found"
                })
            end
        end
        
        -- Search in language
        if artifact.language and string.find(string.lower(artifact.language), query) then
            matches = true
            table.insert(match_info.matches, {
                field = "language",
                value = artifact.language
            })
        end
        
        -- Search in type
        if artifact.type and string.find(string.lower(artifact.type), query) then
            matches = true
            table.insert(match_info.matches, {
                field = "type",
                value = artifact.type
            })
        end
        
        if matches then
            table.insert(results, match_info)
        end
    end
    
    utils.log_info("chat_artifacts", "search_artifacts", {
        query = query,
        total_artifacts = #artifacts,
        results_count = #results
    })
    
    return results
end

-- Extract context around search match
function _M.extract_search_context(text, query, context_length)
    local lower_text = string.lower(text)
    local lower_query = string.lower(query)
    
    local start_pos = string.find(lower_text, lower_query)
    if not start_pos then
        return nil
    end
    
    local context_start = math.max(1, start_pos - context_length)
    local context_end = math.min(#text, start_pos + #query + context_length)
    
    local context = string.sub(text, context_start, context_end)
    
    -- Add ellipsis if truncated
    if context_start > 1 then
        context = "..." .. context
    end
    if context_end < #text then
        context = context .. "..."
    end
    
    return context
end

-- Validate artifact structure
function _M.validate_artifact(artifact)
    local errors = {}
    
    if not artifact.id or artifact.id == "" then
        table.insert(errors, "Missing or empty artifact ID")
    end
    
    if not artifact.type or artifact.type == "" then
        table.insert(errors, "Missing or empty artifact type")
    end
    
    if not artifact.timestamp or type(artifact.timestamp) ~= "number" then
        table.insert(errors, "Missing or invalid timestamp")
    end
    
    local content = artifact.content or artifact.code or ""
    if content == "" then
        table.insert(errors, "Missing content/code")
    end
    
    -- Validate ID format
    if artifact.id and not utils.is_valid_message_id(artifact.id) then
        table.insert(errors, "Invalid artifact ID format")
    end
    
    return #errors == 0, errors
end

-- Clean up orphaned artifacts (artifacts without parent messages)
function _M.cleanup_orphaned_artifacts(redis, chat_id)
    local artifacts, err = redis.get_chat_artifacts(chat_id)
    if not artifacts then
        return nil, err
    end
    
    local messages, msg_err = redis.get_chat_messages(chat_id)
    if not messages then
        return nil, msg_err
    end
    
    -- Create set of valid message IDs
    local valid_message_ids = {}
    for _, message in ipairs(messages) do
        valid_message_ids[message.id] = true
    end
    
    local orphaned_count = 0
    local cleaned_artifacts = {}
    
    for _, artifact in ipairs(artifacts) do
        if artifact.type == "code_block" and artifact.parent_id then
            -- Check if parent message exists
            if not valid_message_ids[artifact.parent_id] then
                orphaned_count = orphaned_count + 1
                utils.log_info("chat_artifacts", "cleanup_orphaned", {
                    artifact_id = artifact.id,
                    parent_id = artifact.parent_id,
                    chat_id = chat_id
                })
                -- Note: Actual deletion would be implemented here
            else
                table.insert(cleaned_artifacts, artifact)
            end
        else
            table.insert(cleaned_artifacts, artifact)
        end
    end
    
    utils.log_info("chat_artifacts", "cleanup_orphaned_artifacts", {
        chat_id = chat_id,
        total_artifacts = #artifacts,
        orphaned_count = orphaned_count,
        remaining_count = #cleaned_artifacts
    })
    
    return {
        total_checked = #artifacts,
        orphaned_found = orphaned_count,
        remaining = #cleaned_artifacts
    }, nil
end

-- Get artifact dependencies (for code blocks, find their parent messages)
function _M.get_artifact_dependencies(artifacts)
    local dependencies = {}
    local parent_map = {}
    
    -- Build parent map
    for _, artifact in ipairs(artifacts) do
        if artifact.parent_id then
            if not parent_map[artifact.parent_id] then
                parent_map[artifact.parent_id] = {}
            end
            table.insert(parent_map[artifact.parent_id], artifact.id)
        end
    end
    
    -- Build dependency information
    for _, artifact in ipairs(artifacts) do
        local deps = {
            artifact_id = artifact.id,
            children = parent_map[artifact.id] or {},
            parent = artifact.parent_id
        }
        dependencies[artifact.id] = deps
    end
    
    return dependencies
end

-- Duplicate artifact (create copy with new ID)
function _M.duplicate_artifact(redis, original_artifact, new_chat_id, new_parent_id)
    local new_artifact = utils.deep_copy(original_artifact)
    
    -- Generate new ID based on new parent
    if new_parent_id then
        -- Extract code block index from original ID
        local block_index = string.match(original_artifact.id, "_code%((%d+)%)$")
        if block_index then
            new_artifact.id = utils.generate_artifact_id(new_parent_id, tonumber(block_index))
            new_artifact.parent_id = new_parent_id
        end
    end
    
    new_artifact.chat_id = new_chat_id
    new_artifact.timestamp = ngx.time()
    
    -- Add duplication metadata
    if not new_artifact.metadata then
        new_artifact.metadata = {}
    end
    new_artifact.metadata.duplicated_from = original_artifact.id
    new_artifact.metadata.duplication_timestamp = ngx.time()
    
    -- Save the duplicated artifact
    local result, err = redis.save_artifact(
        new_chat_id,
        new_artifact.id,
        new_artifact.parent_id,
        new_artifact.code or new_artifact.content,
        new_artifact.language,
        new_artifact.metadata
    )
    
    if result then
        utils.log_info("chat_artifacts", "duplicate_artifact", {
            original_id = original_artifact.id,
            new_id = new_artifact.id,
            new_chat_id = new_chat_id
        })
    else
        utils.log_error("chat_artifacts", "duplicate_artifact", "Failed to save duplicated artifact", {
            error = err,
            original_id = original_artifact.id
        })
    end
    
    return result, err
end

-- Get artifact version history (if implemented)
function _M.get_artifact_history(redis, artifact_id)
    -- This would be implemented if artifact versioning is needed
    -- For now, return empty history
    return {
        artifact_id = artifact_id,
        versions = {},
        current_version = 1
    }
end

-- Archive old artifacts (mark as archived instead of deleting)
function _M.archive_old_artifacts(redis, chat_id, days_threshold)
    days_threshold = days_threshold or 30
    local cutoff_timestamp = ngx.time() - (days_threshold * 24 * 60 * 60)
    
    local artifacts, err = redis.get_chat_artifacts(chat_id)
    if not artifacts then
        return nil, err
    end
    
    local archived_count = 0
    
    for _, artifact in ipairs(artifacts) do
        if artifact.timestamp and artifact.timestamp < cutoff_timestamp then
            -- Mark as archived (would need Redis implementation)
            archived_count = archived_count + 1
            utils.log_info("chat_artifacts", "archive_artifact", {
                artifact_id = artifact.id,
                age_days = math.floor((ngx.time() - artifact.timestamp) / (24 * 60 * 60))
            })
        end
    end
    
    utils.log_info("chat_artifacts", "archive_old_artifacts", {
        chat_id = chat_id,
        days_threshold = days_threshold,
        total_artifacts = #artifacts,
        archived_count = archived_count
    })
    
    return {
        total_checked = #artifacts,
        archived_count = archived_count,
        cutoff_date = os.date("%Y-%m-%d", cutoff_timestamp)
    }, nil
end

return _M
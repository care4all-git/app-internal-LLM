local cjson = require "cjson"
local utils = require "chat_utils"

local _M = {}

-- File validation configuration
local ALLOWED_MIME_TYPES = {
    -- Text files
    "text/plain", "text/csv", "text/markdown", "text/html", "text/css", "text/javascript",
    -- Documents
    "application/json", "application/xml", "application/yaml", "application/toml",
    "application/pdf", "application/msword", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    "application/vnd.ms-excel", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    -- Code files
    "application/javascript", "application/typescript", "application/python", "application/java",
    -- Images
    "image/jpeg", "image/png", "image/gif", "image/svg+xml", "image/webp",
    -- Archives
    "application/zip", "application/x-tar", "application/gzip"
}

local DANGEROUS_EXTENSIONS = {
    "exe", "bat", "cmd", "com", "pif", "scr", "vbs", "js", "jar", "app", "deb", "pkg", "dmg"
}

-- Validate single file
function _M.validate_file(file_data)
    local errors = {}
    local warnings = {}
    
    -- Check required fields
    if not file_data.name or file_data.name == "" then
        table.insert(errors, "File name is required")
    end
    
    if not file_data.size or type(file_data.size) ~= "number" then
        table.insert(errors, "File size is required and must be a number")
    end
    
    -- Validate file size
    if file_data.size and file_data.size > utils.MAX_FILE_SIZE then
        table.insert(errors, string.format("File '%s' is too large (%s). Maximum size is %s",
            file_data.name or "unknown",
            utils.format_file_size(file_data.size),
            utils.format_file_size(utils.MAX_FILE_SIZE)
        ))
    end
    
    if file_data.size and file_data.size <= 0 then
        table.insert(errors, "File size must be greater than 0")
    end
    
    -- Validate file name
    if file_data.name then
        -- Check for dangerous characters
        if string.find(file_data.name, "[<>:\"|?*]") then
            table.insert(errors, "File name contains invalid characters")
        end
        
        -- Check for path traversal
        if string.find(file_data.name, "%.%.") or string.find(file_data.name, "/") or string.find(file_data.name, "\\") then
            table.insert(errors, "File name cannot contain path separators or relative paths")
        end
        
        -- Check file extension
        local extension = string.match(file_data.name, "%.([^%.]+)$")
        if extension then
            extension = string.lower(extension)
            for _, dangerous_ext in ipairs(DANGEROUS_EXTENSIONS) do
                if extension == dangerous_ext then
                    table.insert(errors, "File type '" .. extension .. "' is not allowed for security reasons")
                    break
                end
            end
        end
    end
    
    -- Validate MIME type
    if file_data.type then
        local mime_allowed = false
        for _, allowed_type in ipairs(ALLOWED_MIME_TYPES) do
            if string.sub(file_data.type, 1, string.len(allowed_type)) == allowed_type then
                mime_allowed = true
                break
            end
        end
        
        if not mime_allowed then
            table.insert(warnings, "MIME type '" .. file_data.type .. "' may not be supported")
        end
    end
    
    -- Check if file is text-based for content processing
    local is_text = utils.is_text_file(file_data.name, file_data.type)
    
    utils.log_info("chat_files", "validate_file", {
        filename = file_data.name,
        size = file_data.size,
        type = file_data.type,
        is_text = is_text,
        error_count = #errors,
        warning_count = #warnings
    })
    
    return {
        valid = #errors == 0,
        errors = errors,
        warnings = warnings,
        is_text = is_text,
        metadata = {
            name = file_data.name,
            size = file_data.size,
            type = file_data.type,
            formatted_size = utils.format_file_size(file_data.size or 0)
        }
    }
end

-- Validate multiple files and check total limits
function _M.validate_file_batch(files)
    local results = {
        files = {},
        batch_valid = true,
        total_size = 0,
        text_files = 0,
        binary_files = 0,
        errors = {},
        warnings = {}
    }
    
    -- Check file count limit
    if #files > utils.MAX_FILES then
        table.insert(results.errors, string.format("Too many files (%d). Maximum is %d", #files, utils.MAX_FILES))
        results.batch_valid = false
    end
    
    -- Validate each file and calculate totals
    for i, file_data in ipairs(files) do
        local file_result = _M.validate_file(file_data)
        file_result.index = i
        
        table.insert(results.files, file_result)
        
        if not file_result.valid then
            results.batch_valid = false
        end
        
        if file_data.size then
            results.total_size = results.total_size + file_data.size
        end
        
        if file_result.is_text then
            results.text_files = results.text_files + 1
        else
            results.binary_files = results.binary_files + 1
        end
        
        -- Collect errors and warnings
        for _, error in ipairs(file_result.errors) do
            table.insert(results.errors, string.format("File %d (%s): %s", i, file_data.name or "unknown", error))
        end
        
        for _, warning in ipairs(file_result.warnings) do
            table.insert(results.warnings, string.format("File %d (%s): %s", i, file_data.name or "unknown", warning))
        end
    end
    
    -- Check total size limit
    if results.total_size > utils.MAX_TOTAL_SIZE then
        table.insert(results.errors, string.format("Total file size (%s) exceeds limit (%s)",
            utils.format_file_size(results.total_size),
            utils.format_file_size(utils.MAX_TOTAL_SIZE)
        ))
        results.batch_valid = false
    end
    
    results.formatted_total_size = utils.format_file_size(results.total_size)
    
    utils.log_info("chat_files", "validate_file_batch", {
        file_count = #files,
        total_size = results.total_size,
        text_files = results.text_files,
        binary_files = results.binary_files,
        batch_valid = results.batch_valid,
        error_count = #results.errors,
        warning_count = #results.warnings
    })
    
    return results
end

-- Process file content for text files
function _M.process_file_content(file_data)
    if not file_data.content then
        return file_data, nil
    end
    
    local processed = utils.deep_copy(file_data)
    
    -- Validate content size
    if #file_data.content > utils.MAX_FILE_SIZE then
        return nil, "File content exceeds maximum size limit"
    end
    
    -- Check if content looks like text
    local is_likely_text = _M.is_content_text(file_data.content)
    
    if is_likely_text then
        -- Process text content
        processed.content = _M.sanitize_text_content(file_data.content)
        processed.line_count = _M.count_lines(processed.content)
        processed.word_count = _M.count_words(processed.content)
        processed.char_count = #processed.content
        
        -- Detect encoding issues
        local encoding_issues = _M.detect_encoding_issues(processed.content)
        if #encoding_issues > 0 then
            processed.encoding_warnings = encoding_issues
        end
        
        -- Extract metadata for specific file types
        if file_data.type then
            if string.find(file_data.type, "json") then
                processed.metadata = _M.analyze_json_content(processed.content)
            elseif string.find(file_data.type, "csv") then
                processed.metadata = _M.analyze_csv_content(processed.content)
            elseif string.find(file_data.type, "xml") then
                processed.metadata = _M.analyze_xml_content(processed.content)
            end
        end
    else
        -- For binary content, remove content and add metadata
        processed.content = nil
        processed.content_type = "binary"
        processed.content_note = "Binary content not processed for security and performance reasons"
    end
    
    utils.log_info("chat_files", "process_file_content", {
        filename = file_data.name,
        original_size = #file_data.content,
        is_text = is_likely_text,
        processed_size = processed.content and #processed.content or 0
    })
    
    return processed, nil
end

-- Check if content appears to be text
function _M.is_content_text(content)
    if not content or #content == 0 then
        return false
    end
    
    -- Sample first 1KB for analysis
    local sample_size = math.min(1024, #content)
    local sample = string.sub(content, 1, sample_size)
    
    local null_bytes = 0
    local control_chars = 0
    local printable_chars = 0
    
    for i = 1, #sample do
        local byte = string.byte(sample, i)
        
        if byte == 0 then
            null_bytes = null_bytes + 1
        elseif byte < 32 and byte ~= 9 and byte ~= 10 and byte ~= 13 then
            control_chars = control_chars + 1
        elseif byte >= 32 and byte <= 126 then
            printable_chars = printable_chars + 1
        end
    end
    
    -- Heuristic: if more than 95% is printable and no null bytes, likely text
    local printable_ratio = printable_chars / #sample
    local is_text = printable_ratio > 0.95 and null_bytes == 0
    
    return is_text
end

-- Sanitize text content
function _M.sanitize_text_content(content)
    if not content then
        return ""
    end
    
    -- Remove null bytes
    content = string.gsub(content, "\0", "")
    
    -- Normalize line endings
    content = string.gsub(content, "\r\n", "\n")
    content = string.gsub(content, "\r", "\n")
    
    -- Remove excessive whitespace but preserve formatting
    content = string.gsub(content, "\n\n\n+", "\n\n")
    
    -- Trim trailing whitespace from lines
    content = string.gsub(content, "[ \t]+\n", "\n")
    
    return content
end

-- Count lines in text content
function _M.count_lines(content)
    if not content then
        return 0
    end
    
    local count = 1
    for _ in string.gmatch(content, "\n") do
        count = count + 1
    end
    
    return count
end

-- Count words in text content
function _M.count_words(content)
    if not content then
        return 0
    end
    
    local count = 0
    for _ in string.gmatch(content, "%S+") do
        count = count + 1
    end
    
    return count
end

-- Detect encoding issues
function _M.detect_encoding_issues(content)
    local issues = {}
    
    if not content then
        return issues
    end
    
    -- Check for UTF-8 BOM
    if string.sub(content, 1, 3) == "\239\187\191" then
        table.insert(issues, "UTF-8 BOM detected at beginning of file")
    end
    
    -- Check for mixed line endings
    local has_crlf = string.find(content, "\r\n")
    local has_lf = string.find(content, "\n")
    local has_cr = string.find(content, "\r")
    
    local ending_count = 0
    if has_crlf then ending_count = ending_count + 1 end
    if has_lf then ending_count = ending_count + 1 end
    if has_cr then ending_count = ending_count + 1 end
    
    if ending_count > 1 then
        table.insert(issues, "Mixed line endings detected")
    end
    
    -- Check for suspicious byte sequences
    if string.find(content, "\255\254") or string.find(content, "\254\255") then
        table.insert(issues, "UTF-16 BOM detected - file may not be UTF-8")
    end
    
    return issues
end

-- Analyze JSON content
function _M.analyze_json_content(content)
    local metadata = {
        type = "json",
        valid = false,
        error = nil
    }
    
    local ok, parsed = pcall(cjson.decode, content)
    if ok then
        metadata.valid = true
        metadata.structure_type = type(parsed)
        
        if type(parsed) == "table" then
            metadata.key_count = 0
            for _ in pairs(parsed) do
                metadata.key_count = metadata.key_count + 1
            end
        end
    else
        metadata.error = "Invalid JSON format"
    end
    
    return metadata
end

-- Analyze CSV content
function _M.analyze_csv_content(content)
    local metadata = {
        type = "csv",
        line_count = _M.count_lines(content),
        estimated_columns = 0
    }
    
    -- Estimate column count from first line
    local first_line = string.match(content, "^([^\n]*)")
    if first_line then
        local comma_count = 0
        for _ in string.gmatch(first_line, ",") do
            comma_count = comma_count + 1
        end
        metadata.estimated_columns = comma_count + 1
        
        -- Check for quotes (indicates CSV with quoted fields)
        if string.find(first_line, '"') then
            metadata.has_quoted_fields = true
        end
    end
    
    return metadata
end

-- Analyze XML content
function _M.analyze_xml_content(content)
    local metadata = {
        type = "xml",
        has_declaration = false,
        estimated_elements = 0
    }
    
    -- Check for XML declaration
    if string.find(content, "^%s*<%?xml") then
        metadata.has_declaration = true
    end
    
    -- Count estimated elements
    for _ in string.gmatch(content, "<%w+") do
        metadata.estimated_elements = metadata.estimated_elements + 1
    end
    
    return metadata
end

-- Create file summary for AI context
function _M.create_file_summary(processed_files)
    local summaries = {}
    
    for _, file in ipairs(processed_files) do
        local summary = {
            name = file.name,
            type = file.type,
            size = utils.format_file_size(file.size or 0)
        }
        
        if file.content then
            summary.content_type = "text"
            summary.lines = file.line_count
            summary.words = file.word_count
            
            if file.metadata then
                summary.metadata = file.metadata
            end
        else
            summary.content_type = "binary"
            summary.note = file.content_note or "Binary file not processed"
        end
        
        table.insert(summaries, summary)
    end
    
    return summaries
end

-- Get file processing statistics
function _M.get_processing_stats(files, processed_files)
    local stats = {
        original_count = #files,
        processed_count = #processed_files,
        total_original_size = 0,
        total_processed_size = 0,
        text_files = 0,
        binary_files = 0,
        files_with_warnings = 0
    }
    
    for _, file in ipairs(files) do
        stats.total_original_size = stats.total_original_size + (file.size or 0)
    end
    
    for _, file in ipairs(processed_files) do
        if file.content then
            stats.total_processed_size = stats.total_processed_size + #file.content
            stats.text_files = stats.text_files + 1
        else
            stats.binary_files = stats.binary_files + 1
        end
        
        if file.encoding_warnings and #file.encoding_warnings > 0 then
            stats.files_with_warnings = stats.files_with_warnings + 1
        end
    end
    
    stats.compression_ratio = stats.total_original_size > 0 and 
        (stats.total_processed_size / stats.total_original_size) or 0
    
    return stats
end

return _M
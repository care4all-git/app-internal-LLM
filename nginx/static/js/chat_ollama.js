// ENHANCED Chat Ollama Module - Handles streaming with continuation support
class ChatOllama {
    constructor() {
        this.abortController = null;
        this.isStreaming = false;
        this.lastResponse = null;
        this.needsContinuation = false;
        console.log('Enhanced Chat Ollama module initialized with continuation support');
    }
    
    // ENHANCED: Stream message to Ollama via backend with continuation detection
    async streamMessage(message, files = [], chatId = null) {
        if (this.isStreaming) {
            throw new Error('Already streaming a message');
        }
        
        this.isStreaming = true;
        this.abortController = new AbortController();
        
        try {
            const requestBody = {
                message: message,
                files: await this.processFiles(files),
                chat_id: chatId
            };
            
            console.log('Ollama: Streaming request:', { 
                message: message.substring(0, 100) + (message.length > 100 ? '...' : ''),
                filesCount: files.length,
                chatId,
                unlimited: true
            });
            
            const response = await fetch('/api/chat/stream', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Accept': 'text/event-stream',
                    'Cache-Control': 'no-cache'
                },
                body: JSON.stringify(requestBody),
                signal: this.abortController.signal
            });
            
            if (!response.ok) {
                const errorText = await response.text();
                throw new Error(`Stream failed: ${response.status} - ${errorText}`);
            }
            
            return this.handleStreamResponse(response);
            
        } catch (error) {
            this.isStreaming = false;
            this.abortController = null;
            
            if (error.name === 'AbortError') {
                throw new Error('Stream was cancelled');
            }
            
            console.error('Ollama: Stream error:', error);
            throw error;
        }
    }
    
    // ENHANCED: Handle streaming response with continuation detection
    async handleStreamResponse(response) {
        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        
        let chatId = null;
        let accumulated = '';
        let receivedChatId = false;
        let needsContinuation = false;
        let completionInfo = null;
        
        const stream = {
            chatId: null,
            content: '',
            isComplete: true,
            needsContinuation: false,
            error: null
        };
        
        try {
            while (true) {
                const { done, value } = await reader.read();
                
                if (done) break;
                
                const chunk = decoder.decode(value, { stream: true });
                const lines = chunk.split('\n');
                
                for (const line of lines) {
                    if (line.startsWith('data: ')) {
                        const data = line.slice(6);
                        
                        if (data === '[DONE]') {
                            stream.content = accumulated;
                            stream.isComplete = !needsContinuation;
                            stream.needsContinuation = needsContinuation;
                            this.isStreaming = false;
                            this.abortController = null;
                            this.lastResponse = accumulated;
                            this.needsContinuation = needsContinuation;
                            return stream;
                        }
                        
                        try {
                            const parsed = JSON.parse(data);
                            
                            // Handle chat_id from server
                            if (parsed.chat_id && !receivedChatId) {
                                chatId = parsed.chat_id;
                                stream.chatId = chatId;
                                receivedChatId = true;
                                console.log('Ollama: Received chat_id:', chatId);
                            }
                            
                            // ENHANCED: Handle completion status
                            if (parsed.type === 'completion_status') {
                                completionInfo = {
                                    isComplete: parsed.is_complete,
                                    needsContinuation: parsed.needs_continuation,
                                    reason: parsed.completion_reason
                                };
                                needsContinuation = parsed.needs_continuation;
                                
                                console.log('Ollama: Completion status:', completionInfo);
                            }
                            
                            // Handle continuation needed event
                            if (parsed.type === 'continuation_needed') {
                                needsContinuation = true;
                                console.log('Ollama: Continuation needed:', parsed.message);
                                
                                // Show continuation prompt to user
                                if (window.chat && window.chat.ui) {
                                    setTimeout(() => {
                                        this.showContinuationPrompt(parsed.message);
                                    }, 1000);
                                }
                            }
                            
                            // Handle content chunks
                            if (parsed.content) {
                                accumulated += parsed.content;
                                stream.content = accumulated;
                                
                                // Yield intermediate result for streaming UI updates
                                if (this.onChunk) {
                                    this.onChunk({
                                        chatId: chatId,
                                        content: accumulated,
                                        chunk: parsed.content,
                                        isComplete: false,
                                        needsContinuation: needsContinuation
                                    });
                                }
                            }
                            
                            // Handle errors from server
                            if (parsed.error) {
                                stream.error = parsed.error;
                                throw new Error(parsed.error);
                            }
                            
                        } catch (e) {
                            if (e.message !== 'Unexpected end of JSON input') {
                                console.error('Ollama: JSON parse error:', e, 'Data:', data);
                                stream.error = e.message;
                                throw e;
                            }
                        }
                    }
                }
            }
            
            // Stream ended
            stream.content = accumulated;
            stream.isComplete = !needsContinuation;
            stream.needsContinuation = needsContinuation;
            this.isStreaming = false;
            this.abortController = null;
            this.lastResponse = accumulated;
            this.needsContinuation = needsContinuation;
            return stream;
            
        } catch (error) {
            this.isStreaming = false;
            this.abortController = null;
            stream.error = error.message;
            throw error;
        }
    }
    
    // NEW: Show continuation prompt to user
    showContinuationPrompt(message) {
        if (window.chat && window.chat.ui) {
            // Create continuation prompt UI
            const continuationDiv = document.createElement('div');
            continuationDiv.className = 'continuation-prompt alert alert-info d-flex justify-content-between align-items-center';
            continuationDiv.innerHTML = `
                <div>
                    <i class="bi bi-info-circle me-2"></i>
                    <strong>Response may be incomplete.</strong> ${message}
                </div>
                <div>
                    <button class="btn btn-sm btn-primary me-2" onclick="window.chat.ollama.continueResponse()">
                        <i class="bi bi-arrow-right"></i> Continue
                    </button>
                    <button class="btn btn-sm btn-outline-secondary" onclick="this.parentElement.parentElement.remove()">
                        <i class="bi bi-x"></i> Dismiss
                    </button>
                </div>
            `;
            
            // Add to messages container
            const messagesContainer = document.getElementById('messages-content');
            if (messagesContainer) {
                messagesContainer.appendChild(continuationDiv);
                window.chat.ui.scrollToBottom();
            }
        }
    }
    
    // NEW: Continue previous response
    async continueResponse() {
        if (!this.needsContinuation || !this.lastResponse) {
            console.warn('No continuation needed or no previous response');
            return;
        }
        
        if (this.isStreaming) {
            console.warn('Already streaming, cannot continue');
            return;
        }
        
        try {
            // Remove continuation prompt
            const continuationPrompt = document.querySelector('.continuation-prompt');
            if (continuationPrompt) {
                continuationPrompt.remove();
            }
            
            // Add placeholder for continuation
            if (window.chat && window.chat.ui) {
                window.chat.ui.addPlaceholderAssistantMessage();
            }
            
            // Send continuation request
            const response = await fetch('/api/chat/continue', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Accept': 'text/event-stream',
                    'Cache-Control': 'no-cache'
                },
                body: JSON.stringify({
                    chat_id: window.chat.currentChatId,
                    previous_response: this.lastResponse
                })
            });
            
            if (!response.ok) {
                throw new Error(`Continuation failed: ${response.status}`);
            }
            
            const continuationResult = await this.handleStreamResponse(response);
            
            // Combine responses
            if (continuationResult.content) {
                const combinedResponse = this.lastResponse + continuationResult.content;
                this.lastResponse = combinedResponse;
                this.needsContinuation = continuationResult.needsContinuation;
                
                // Reload messages to show complete response
                if (window.chat) {
                    await window.chat.loadChatMessages(window.chat.currentChatId);
                }
            }
            
            return continuationResult;
            
        } catch (error) {
            console.error('Ollama: Continuation error:', error);
            if (window.chat && window.chat.ui) {
                window.chat.ui.showToast('Failed to continue response', 'error');
            }
            throw error;
        }
    }
    
    // Stop current streaming
    stopStream() {
        if (this.abortController && this.isStreaming) {
            console.log('Ollama: Stopping stream');
            this.abortController.abort();
            this.isStreaming = false;
            this.abortController = null;
            return true;
        }
        return false;
    }
    
    // Check if currently streaming
    getStreamingStatus() {
        return {
            isStreaming: this.isStreaming,
            canStop: this.abortController !== null,
            needsContinuation: this.needsContinuation,
            hasLastResponse: !!this.lastResponse
        };
    }
    
    // Set callback for streaming chunks
    setChunkCallback(callback) {
        this.onChunk = callback;
    }
    
    // Clear chunk callback
    clearChunkCallback() {
        this.onChunk = null;
    }
    
    // Process files for sending to backend
    async processFiles(files) {
        const processedFiles = [];
        
        for (const file of files) {
            try {
                const fileData = {
                    name: file.name,
                    type: file.type,
                    size: file.size
                };
                
                // Read text files content
                if (this.isTextFile(file)) {
                    fileData.content = await this.readFileAsText(file);
                }
                
                processedFiles.push(fileData);
            } catch (error) {
                console.error('Ollama: Error processing file:', file.name, error);
                // Continue processing other files
            }
        }
        
        return processedFiles;
    }
    
    // File handling utilities
    isTextFile(file) {
        const textTypes = [
            'text/',
            'application/json',
            'application/xml',
            'application/javascript',
            'application/csv',
            'application/sql'
        ];
        
        return textTypes.some(type => file.type.startsWith(type)) || 
               file.name.match(/\.(txt|md|json|xml|csv|sql|js|ts|py|java|cpp|c|h|css|html|yml|yaml|toml|ini|cfg|conf|log)$/i);
    }
    
    readFileAsText(file) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = (e) => resolve(e.target.result);
            reader.onerror = (e) => reject(new Error('Failed to read file'));
            reader.readAsText(file);
        });
    }
    
    // Format file information for display
    formatFileInfo(files) {
        if (!files || files.length === 0) return '';
        
        return files.map(file => 
            `📎 ${file.name} (${this.formatFileSize(file.size)})`
        ).join('\n');
    }
    
    formatFileSize(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
    }

    // Prepare message content and files for sending
    prepareMessage(message, attachedFiles) {
        const filesToSend = [...attachedFiles];
        let messageContent = message;
        
        if (filesToSend.length > 0) {
            const fileInfo = this.formatFileInfo(filesToSend);
            messageContent = messageContent ? `${messageContent}\n\n${fileInfo}` : fileInfo;
        }
        
        return { messageContent, filesToSend };
    }

    // Generate title from message
    generateTitle(message) {
        return message.length > 30 ? message.substring(0, 30) + '...' : message;
    }
    
    // NEW: Create continuation prompt
    createContinuationPrompt(previousResponse) {
        return "Please continue your previous response from where you left off. Complete your full answer without repeating what you already said.";
    }
    
    // Error classification
    classifyError(error) {
        if (error.name === 'AbortError') {
            return {
                type: 'cancelled',
                message: 'Stream was cancelled',
                userFriendly: 'Response generation was stopped.'
            };
        }
        
        if (error.message.includes('fetch')) {
            return {
                type: 'network',
                message: error.message,
                userFriendly: 'Connection error. Please check if the AI service is running.'
            };
        }
        
        if (error.message.includes('400')) {
            return {
                type: 'bad_request',
                message: error.message,
                userFriendly: 'Invalid request. Please try again.'
            };
        }
        
        if (error.message.includes('500')) {
            return {
                type: 'server_error',
                message: error.message,
                userFriendly: 'Server error. Please try again in a moment.'
            };
        }
        
        return {
            type: 'unknown',
            message: error.message,
            userFriendly: 'An unexpected error occurred. Please try again.'
        };
    }
    
    // ENHANCED: Prepare context with files (supports unlimited responses)
    prepare_context_with_files(messages, files) {
        const context_messages = [];
        
        // Copy existing messages
        for (const msg of messages) {
            context_messages.push({
                role: msg.role,
                content: msg.content
            });
        }
        
        // Add file context to the last user message if files are provided
        if (files && files.length > 0) {
            const file_context = this.format_files_for_context(files);
            if (file_context !== "" && context_messages.length > 0) {
                const last_message = context_messages[context_messages.length - 1];
                if (last_message.role === "user") {
                    last_message.content = last_message.content + file_context;
                }
            }
        }
        
        return context_messages;
    }
    
    // Format files for AI context
    format_files_for_context(files) {
        if (!files || files.length === 0) {
            return "";
        }
        
        let file_context = "\n\n--- ATTACHED FILES ---\n";
        
        for (const file of files) {
            file_context += `\nFile: ${file.name || "unknown"}`;
            file_context += `\nType: ${file.type || "unknown"}`;
            file_context += `\nSize: ${file.size || "unknown"} bytes`;
            
            if (file.content) {
                file_context += `\nContent:\n\`\`\`\n${file.content}\n\`\`\``;
            }
            
            file_context += "\n---\n";
        }
        
        return file_context;
    }
    
    // Reset continuation state
    resetContinuationState() {
        this.lastResponse = null;
        this.needsContinuation = false;
        console.log('Ollama: Continuation state reset');
    }
    
    // Check if response needs continuation
    checkNeedsContinuation() {
        return this.needsContinuation && this.lastResponse;
    }
    
    // Get last response for continuation
    getLastResponse() {
        return this.lastResponse;
    }
}

// Export for use in other modules
if (typeof module !== 'undefined' && module.exports) {
    module.exports = ChatOllama;
} else if (typeof window !== 'undefined') {
    window.ChatOllama = ChatOllama;
}
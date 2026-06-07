// UI functionality and visual elements - Updated for chat(n) format with placeholders and enhanced code blocks
class ChatUI {
    constructor(chatInstance) {
        this.chatInstance = chatInstance;
        this.init();
    }
    
    init() {
        console.log('Chat UI system initialized (chat(n) format)');
    }
    
    setupSidebarResize() {
        // Could add resize functionality here if needed
    }
    
    toggleSidebar() {
        const sidebar = document.getElementById('sidebar');
        if (sidebar) {
            sidebar.classList.toggle('collapsed');
        }
    }
    
    updateCharCount() {
        const textarea = document.getElementById('chat-input');
        const countEl = document.getElementById('char-count');
        
        if (textarea && countEl) {
            countEl.textContent = textarea.value.length;
        }
    }
    
    autoResizeTextarea() {
        const textarea = document.getElementById('chat-input');
        if (textarea) {
            textarea.style.height = 'auto';
            const maxHeight = 120;
            const newHeight = Math.min(textarea.scrollHeight, maxHeight);
            textarea.style.height = newHeight + 'px';
        }
    }
    
    updateButtons(isTyping) {
        const sendButton = document.getElementById('send-button');
        const stopButton = document.getElementById('stop-button');
        const chatInput = document.getElementById('chat-input');
        const attachButton = document.getElementById('attach-button');
        const status = document.getElementById('status');
        
        if (sendButton) {
            sendButton.style.display = isTyping ? 'none' : 'flex';
            sendButton.disabled = isTyping;
        }
        if (stopButton) {
            stopButton.style.display = isTyping ? 'flex' : 'none';
        }
        if (chatInput) {
            chatInput.disabled = isTyping;
        }
        if (attachButton) {
            attachButton.disabled = isTyping;
        }
        if (status) {
            status.textContent = isTyping ? 'Thinking...' : 'Ready';
        }
    }
    
    scrollToBottom() {
        const messagesContainer = document.getElementById('chat-messages');
        if (messagesContainer) {
            messagesContainer.scrollTop = messagesContainer.scrollHeight;
        }
    }
    
    updateCurrentChatTitle(title) {
        const titleElement = document.getElementById('current-chat-title');
        if (titleElement) {
            titleElement.textContent = title;
        }
    }
    
    // UI Update Methods
    updateChatList() {
        const chatList = document.getElementById('chat-list');
        if (!chatList) return;
        
        const chatsArray = Array.from(this.chatInstance.chats.values())
            .sort((a, b) => new Date(b.updatedAt) - new Date(a.updatedAt));
        
        if (chatsArray.length === 0) {
            chatList.innerHTML = `
                <div class="text-center text-muted p-3">
                    <i class="bi bi-chat-dots-fill"></i>
                    <p class="mb-0 mt-2">No chats yet</p>
                    <small>Start a new conversation</small>
                </div>
            `;
            return;
        }
        
        chatList.innerHTML = '';
        
        chatsArray.forEach(chat => {
            const chatItem = this.createChatListItem(chat);
            chatList.appendChild(chatItem);
        });
    }
    
    createChatListItem(chat) {
        const chatItem = document.createElement('div');
        chatItem.className = 'chat-item';
        chatItem.dataset.chatId = chat.id;
        
        if (chat.id === this.chatInstance.currentChatId) {
            chatItem.classList.add('active');
        }
        
        // Get preview text from last message
        const lastMessage = chat.messages[chat.messages.length - 1];
        const preview = chat.preview || (lastMessage ? 
            (lastMessage.content.length > 60 ? 
                lastMessage.content.substring(0, 60) + '...' : 
                lastMessage.content) : 
            'No messages yet');
        
        const messageCount = chat.messageCount || chat.messages.length;
        const formattedDate = this.formatChatDate(chat.updatedAt);
        
        // Extract timestamp from chat ID for display
        const chatTimestamp = this.chatInstance.getChatTimestamp(chat.id);
        const chatDisplayId = chatTimestamp ? `chat(${chatTimestamp})` : chat.id;
        
        chatItem.innerHTML = `
            <div class="chat-item-header">
                <div class="chat-item-title">${this.escapeHtml(chat.title)}</div>
                <div class="chat-item-menu">
                    <button onclick="window.chat.showChatOptions('${chat.id}')" title="Options">
                        <i class="bi bi-three-dots"></i>
                    </button>
                </div>
            </div>
            <div class="chat-item-preview">${this.escapeHtml(preview)}</div>
            <div class="chat-item-meta">
                <div class="chat-item-date">${formattedDate}</div>
                <div class="chat-item-count">${messageCount}</div>
            </div>
            <div class="chat-item-id">
                <small class="text-muted">${chatDisplayId}</small>
            </div>
        `;
        
        // Add click handler for switching chats
        chatItem.addEventListener('click', (e) => {
            if (!e.target.closest('.chat-item-menu')) {
                this.chatInstance.switchToChat(chat.id);
            }
        });
        
        return chatItem;
    }
    
    updateChatListActiveState() {
        const chatItems = document.querySelectorAll('.chat-item');
        chatItems.forEach(item => {
            if (item.dataset.chatId === this.chatInstance.currentChatId) {
                item.classList.add('active');
            } else {
                item.classList.remove('active');
            }
        });
    }
    
    formatChatDate(date) {
        const now = new Date();
        const chatDate = new Date(date);
        const diffInMs = now - chatDate;
        const diffInHours = diffInMs / (1000 * 60 * 60);
        
        if (diffInHours < 1) {
            return 'Just now';
        } else if (diffInHours < 24) {
            return `${Math.floor(diffInHours)}h ago`;
        } else if (diffInHours < 24 * 7) {
            return `${Math.floor(diffInHours / 24)}d ago`;
        } else {
            return chatDate.toLocaleDateString();
        }
    }
    
    escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
    
    // ENHANCED: Code blocks with panel support
    enhanceCodeBlocks(contentDiv, originalContent) {
        if (window.Prism) {
            contentDiv.querySelectorAll('pre code').forEach((block) => {
                Prism.highlightElement(block);
            });
        }
        
        contentDiv.querySelectorAll('pre').forEach((preElement, index) => {
            this.enhanceCodeBlockWithPanel(preElement, originalContent, index);
        });
        
        this.addMessageCopyButton(contentDiv, originalContent);
    }
    
    // NEW: Enhanced code block with panel support
    enhanceCodeBlockWithPanel(preElement, originalContent, blockIndex) {
        const codeElement = preElement.querySelector('code');
        if (!codeElement) return;
        
        const codeText = codeElement.textContent || codeElement.innerText;
        const codeLines = codeText.split('\n').length;
        
        // Determine if this is a large code block (>12 lines)
        const isLargeCodeBlock = codeLines > 12;
        
        // Add large-code class and overlay for large blocks
        if (isLargeCodeBlock) {
            preElement.classList.add('large-code');
            
            // Create overlay with line count
            const overlay = document.createElement('div');
            overlay.className = 'code-block-overlay';
            overlay.innerHTML = `<i class="bi bi-arrows-fullscreen"></i> View Full Code (${codeLines} lines)`;
            
            // Get artifact ID from message element
            const messageElement = preElement.closest('.message');
            const messageId = messageElement?.dataset.messageId;
            const artifactId = messageId ? `${messageId}_code(${blockIndex + 1})` : null;
            
            console.log(`🔍 Code block overlay: messageId=${messageId}, artifactId=${artifactId}, lines=${codeLines}`);
            
            if (artifactId) {
                overlay.onclick = () => {
                    console.log(`🔍 Opening code panel for: ${artifactId}`);
                    if (window.artifactsPanel && window.artifactsPanel.showCodePanel) {
                        window.artifactsPanel.showCodePanel(artifactId);
                    } else {
                        console.warn('Artifacts panel not available');
                        this.showToast('Code panel not available', 'warning');
                    }
                };
            }
            
            preElement.appendChild(overlay);
        }
        
        // Create action buttons container
        const actionsContainer = document.createElement('div');
        actionsContainer.className = 'code-block-actions';
        
        // Copy button
        const copyButton = document.createElement('button');
        copyButton.className = 'code-block-btn btn-copy';
        copyButton.innerHTML = '<i class="bi bi-clipboard"></i>';
        copyButton.title = 'Copy code';
        copyButton.onclick = (e) => {
            e.stopPropagation();
            this.copyToClipboard(codeText, copyButton);
        };
        
        actionsContainer.appendChild(copyButton);
        
        // ENHANCED: View panel button for ALL code blocks (not just large ones)
        const viewButton = document.createElement('button');
        viewButton.className = 'code-block-btn btn-view';
        viewButton.innerHTML = '<i class="bi bi-arrows-fullscreen"></i>';
        viewButton.title = 'View in panel';
        
        // Get artifact ID from message element
        const messageElement = preElement.closest('.message');
        const messageId = messageElement?.dataset.messageId;
        const artifactId = messageId ? `${messageId}_code(${blockIndex + 1})` : null;
        
        if (artifactId) {
            viewButton.onclick = (e) => {
                e.stopPropagation();
                console.log(`🔍 View button clicked for: ${artifactId}`);
                if (window.artifactsPanel && window.artifactsPanel.showCodePanel) {
                    window.artifactsPanel.showCodePanel(artifactId);
                } else {
                    console.warn('Artifacts panel not available');
                    this.showToast('Code panel not available', 'warning');
                }
            };
        } else {
            // Disable button if no artifact ID
            viewButton.disabled = true;
            viewButton.title = 'Artifact ID not available';
        }
        
        actionsContainer.appendChild(viewButton);
        
        // Add actions to the pre element
        preElement.appendChild(actionsContainer);
        
        // Ensure the pre element has relative positioning
        preElement.style.position = 'relative';
    }
    
    addMessageCopyButton(contentDiv, content) {
        const copyButton = document.createElement('button');
        copyButton.className = 'btn btn-outline-secondary btn-sm copy-btn';
        copyButton.innerHTML = '<i class="bi bi-clipboard"></i> Copy message';
        copyButton.title = 'Copy entire message';
        copyButton.onclick = () => this.copyToClipboard(content, copyButton);
        
        const buttonWrapper = document.createElement('div');
        buttonWrapper.className = 'message-actions';
        buttonWrapper.appendChild(copyButton);
        
        contentDiv.appendChild(buttonWrapper);
    }
    
    // ENHANCED: Better copy functionality
    async copyToClipboard(text, button) {
        try {
            await navigator.clipboard.writeText(text);
            
            const originalContent = button.innerHTML;
            button.innerHTML = '<i class="bi bi-check"></i>';
            button.classList.add('text-success');
            
            setTimeout(() => {
                button.innerHTML = originalContent;
                button.classList.remove('text-success');
            }, 2000);
            
            this.showToast('Copied to clipboard!', 'success');
            
        } catch (err) {
            console.error('Failed to copy text: ', err);
            
            // Fallback for older browsers
            const textArea = document.createElement('textarea');
            textArea.value = text;
            document.body.appendChild(textArea);
            textArea.select();
            
            try {
                document.execCommand('copy');
                
                const originalContent = button.innerHTML;
                button.innerHTML = '<i class="bi bi-check"></i>';
                button.classList.add('text-success');
                
                setTimeout(() => {
                    button.innerHTML = originalContent;
                    button.classList.remove('text-success');
                }, 2000);
                
                this.showToast('Copied to clipboard!', 'success');
                
            } catch (fallbackErr) {
                console.error('Fallback copy failed: ', fallbackErr);
                this.showToast('Failed to copy to clipboard', 'error');
            }
            
            document.body.removeChild(textArea);
        }
    }
    
    // Show toast notification
    showToast(message, type = 'info') {
        // Create toast element if it doesn't exist
        let toastContainer = document.getElementById('toast-container');
        if (!toastContainer) {
            toastContainer = document.createElement('div');
            toastContainer.id = 'toast-container';
            toastContainer.className = 'position-fixed top-0 end-0 p-3';
            toastContainer.style.zIndex = '9999';
            document.body.appendChild(toastContainer);
        }
        
        const toastId = `toast-${Date.now()}`;
        const iconMap = {
            success: 'bi-check-circle-fill text-success',
            error: 'bi-x-circle-fill text-danger',
            warning: 'bi-exclamation-triangle-fill text-warning',
            info: 'bi-info-circle-fill text-info'
        };
        
        const toastHTML = `
            <div id="${toastId}" class="toast" role="alert">
                <div class="toast-header">
                    <i class="bi ${iconMap[type] || iconMap.info} me-2"></i>
                    <strong class="me-auto">Notification</strong>
                    <button type="button" class="btn-close" data-bs-dismiss="toast"></button>
                </div>
                <div class="toast-body">
                    ${this.escapeHtml(message)}
                </div>
            </div>
        `;
        
        toastContainer.insertAdjacentHTML('beforeend', toastHTML);
        
        const toastElement = document.getElementById(toastId);
        const toast = new bootstrap.Toast(toastElement, { delay: 3000 });
        toast.show();
        
        // Remove toast element after it's hidden
        toastElement.addEventListener('hidden.bs.toast', () => {
            toastElement.remove();
        });
    }

    // PLACEHOLDER MESSAGE METHODS
    // Create temporary placeholder messages for immediate UX feedback
    addPlaceholderUserMessage(content, files = []) {
        const messagesContainer = document.getElementById('messages-content');
        if (!messagesContainer) return null;
        
        const messageDiv = document.createElement('div');
        messageDiv.className = 'message message-user placeholder-message';
        messageDiv.dataset.placeholder = 'true';
        
        const timestamp = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        
        messageDiv.innerHTML = `
            <div class="message-header">
                <div class="message-role">You</div>
                <div class="message-time">${timestamp}</div>
            </div>
            <div class="message-content">${this.escapeHtml(content)}</div>
        `;
        
        // Add files if any
        if (files && files.length > 0) {
            const filesDiv = document.createElement('div');
            filesDiv.className = 'message-files';
            
            files.forEach(file => {
                const fileItem = document.createElement('div');
                fileItem.className = 'message-file-item';
                
                const icon = this.chatInstance?.fileUpload?.getFileIcon ? 
                    this.chatInstance.fileUpload.getFileIcon(file.type) : 'bi-file-earmark';
                const size = this.chatInstance?.fileUpload?.formatFileSize ? 
                    this.chatInstance.fileUpload.formatFileSize(file.size) : 
                    this.formatFileSize(file.size);
                
                fileItem.innerHTML = `
                    <i class="bi ${icon}"></i>
                    <span>${this.escapeHtml(file.name)}</span>
                    <small>(${size})</small>
                `;
                
                filesDiv.appendChild(fileItem);
            });
            
            messageDiv.insertBefore(filesDiv, messageDiv.querySelector('.message-content'));
        }
        
        messagesContainer.appendChild(messageDiv);
        this.scrollToBottom();
        
        return messageDiv;
    }

    // Create streaming placeholder for assistant response
    addPlaceholderAssistantMessage() {
        const messagesContainer = document.getElementById('messages-content');
        if (!messagesContainer) return null;
        
        const messageDiv = document.createElement('div');
        messageDiv.className = 'message message-assistant placeholder-message streaming';
        messageDiv.dataset.placeholder = 'true';
        
        const timestamp = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        
        messageDiv.innerHTML = `
            <div class="message-header">
                <div class="message-role">JAI</div>
                <div class="message-time">${timestamp}</div>
            </div>
            <div class="message-content"></div>
        `;
        
        messagesContainer.appendChild(messageDiv);
        this.scrollToBottom();
        
        return messageDiv;
    }

    // Update streaming content in placeholder
    updatePlaceholderStreaming(content) {
        const streamingMessage = document.querySelector('.placeholder-message.streaming');
        
        if (streamingMessage) {
            const contentDiv = streamingMessage.querySelector('.message-content');
            if (contentDiv && content) {
                // Update content with markdown rendering
                if (window.marked) {
                    contentDiv.innerHTML = marked.parse(content);
                } else {
                    contentDiv.textContent = content;
                }
                
                // Enhance code blocks
                this.enhanceCodeBlocks(contentDiv, content);
                
                // Scroll to keep content visible
                this.scrollToBottom();
            }
        }
    }

    // Remove all placeholder messages
    removePlaceholderMessages() {
        const placeholders = document.querySelectorAll('.placeholder-message');
        placeholders.forEach(placeholder => {
            placeholder.remove();
        });
    }

    // Modified updateStreamingContent to work with placeholders
    updateStreamingContent(content) {
        // Try placeholder first, fallback to old method
        const placeholderMessage = document.querySelector('.placeholder-message.streaming');
        
        if (placeholderMessage) {
            this.updatePlaceholderStreaming(content);
            return;
        }
        
        // Fallback to original method if no placeholder
        const messagesContainer = document.getElementById('messages-content');
        if (!messagesContainer) return;
        
        let streamingMessage = messagesContainer.querySelector('.message.streaming:not(.placeholder-message)');
        
        if (!streamingMessage && content) {
            streamingMessage = this.addMessage('assistant', '', true);
            streamingMessage.classList.add('streaming');
        }
        
        if (streamingMessage && content) {
            const contentDiv = streamingMessage.querySelector('.message-content');
            if (contentDiv) {
                if (window.marked) {
                    contentDiv.innerHTML = marked.parse(content);
                } else {
                    contentDiv.textContent = content;
                }
                
                this.enhanceCodeBlocks(contentDiv, content);
                this.scrollToBottom();
            }
        }
    }

    // UPDATED: renderChatMessages to set message ID for artifacts
    renderChatMessages(result, artifacts) {
        const messagesContainer = document.getElementById('messages-content');
        const welcomePrompt = document.getElementById('welcome-prompt');
        
        if (!messagesContainer) return;
        
        // Remove all placeholder messages first
        this.removePlaceholderMessages();
        
        // Clear and render real messages
        messagesContainer.innerHTML = '';
        if (welcomePrompt) {
            welcomePrompt.style.display = 'none';
        }
        
        const messages = result.messages;
        console.log(`Rendering ${messages.length} real messages for chat`);
        
        // Update chat title from first user message
        const firstUserMessage = messages.find(msg => msg.role === 'user');
        if (firstUserMessage && firstUserMessage.content) {
            const title = firstUserMessage.content.length > 30 ? 
                firstUserMessage.content.substring(0, 30) + '...' : 
                firstUserMessage.content;
            this.updateCurrentChatTitle(title);
        }
        
        // Load messages and process with artifacts
        for (const msg of messages) {
            // Use msg.role for sender, not msg.id
            const sender = msg.role === 'user' ? 'user' : 'assistant';
            const messageElement = this.addMessage(sender, msg.content, false, msg.files || []);
            
            if (messageElement && msg.id) {
                // CRITICAL: Set the message ID as data attribute for code block detection
                messageElement.dataset.messageId = msg.id;
                
                console.log(`🔍 Set messageId=${msg.id} on message element`);
                
                // Determine artifact type from message ID format or role
                let messageType;
                if (msg.id.startsWith('admin(')) {
                    messageType = 'admin';
                } else if (msg.id.startsWith('jai(')) {
                    messageType = 'jai';
                } else {
                    messageType = msg.role === 'user' ? 'admin' : 'jai';
                }
                
                artifacts.processMessageElement(
                    messageElement, 
                    messageType, 
                    msg.content, 
                    msg.files || [], 
                    msg.id
                );
            }
        }
        
        this.scrollToBottom();
    }

    // New method to handle empty chat state
    showEmptyChat(error = null) {
        const messagesContainer = document.getElementById('messages-content');
        const welcomePrompt = document.getElementById('welcome-prompt');
        
        // Clear messages
        if (messagesContainer) {
            messagesContainer.innerHTML = '';
        }
        
        // Show welcome prompt
        if (welcomePrompt) {
            welcomePrompt.style.display = 'block';
        }
        
        // Update title based on state
        if (error) {
            console.error('Failed to load chat messages:', error);
            this.updateCurrentChatTitle('Error Loading Chat');
            this.showToast('Failed to load chat messages', 'error');
        } else {
            this.updateCurrentChatTitle('New Chat');
        }
    }

    // Search chats with provided chats map
    searchChats(query, chatsMap) {
        const chatItems = document.querySelectorAll('.chat-item');
        const lowerQuery = query.toLowerCase();
        
        chatItems.forEach(item => {
            const title = item.querySelector('.chat-item-title').textContent.toLowerCase();
            const preview = item.querySelector('.chat-item-preview').textContent.toLowerCase();
            
            if (title.includes(lowerQuery) || preview.includes(lowerQuery)) {
                item.style.display = 'block';
            } else {
                item.style.display = 'none';
            }
        });
    }

    // Clear messages UI
    clearMessagesUI() {
        const messagesContainer = document.getElementById('messages-content');
        const welcomePrompt = document.getElementById('welcome-prompt');
        
        if (messagesContainer) {
            messagesContainer.innerHTML = '';
        }
        if (welcomePrompt) {
            welcomePrompt.style.display = 'block';
        }
    }

    // Hide welcome prompt
    hideWelcomePrompt() {
        const welcomePrompt = document.getElementById('welcome-prompt');
        if (welcomePrompt) {
            welcomePrompt.style.display = 'none';
        }
    }

    // Show chat options modal
    showChatOptionsModal(chatId, chat, modal) {
        if (chat) {
            const renameInput = document.getElementById('rename-chat-input');
            if (renameInput) {
                renameInput.value = chat.title;
            }
            modal.show();
        }
    }

    // Download file utility
    downloadFile(content, filename, mimeType) {
        const blob = new Blob([content], { type: mimeType });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }

    // MAIN addMessage method - for real messages from Redis
    addMessage(sender, content, isStreaming = false, files = []) {
        const messagesContainer = document.getElementById('messages-content');
        if (!messagesContainer) return null;
        
        // Create main message div
        const messageDiv = document.createElement('div');
        messageDiv.className = `message message-${sender}`;
        
        // Set role label
        const roleLabel = sender === 'user' ? 'You' : sender === 'assistant' ? 'JAI' : 'System';
        
        // Create timestamp
        const timestamp = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        
        // Build message structure
        messageDiv.innerHTML = `
            <div class="message-header">
                <div class="message-role">${roleLabel}</div>
                <div class="message-time">${timestamp}</div>
            </div>
            <div class="message-content"></div>
        `;
        
        // Get the content div after creating the structure
        const contentDiv = messageDiv.querySelector('.message-content');
        
        // Handle files (for user messages)
        if (files && files.length > 0) {
            const filesDiv = document.createElement('div');
            filesDiv.className = 'message-files';
            
            files.forEach(file => {
                const fileItem = document.createElement('div');
                fileItem.className = 'message-file-item';
                
                // Use proper file upload methods if available
                const icon = this.chatInstance?.fileUpload?.getFileIcon ? 
                    this.chatInstance.fileUpload.getFileIcon(file.type) : 'bi-file-earmark';
                const size = this.chatInstance?.fileUpload?.formatFileSize ? 
                    this.chatInstance.fileUpload.formatFileSize(file.size) : 
                    this.formatFileSize(file.size);
                
                fileItem.innerHTML = `
                    <i class="bi ${icon}"></i>
                    <span>${this.escapeHtml(file.name)}</span>
                    <small>(${size})</small>
                `;
                
                filesDiv.appendChild(fileItem);
            });
            
            // Insert files div before content div
            messageDiv.insertBefore(filesDiv, contentDiv);
        }
        
        // Handle content based on message type and streaming state
        if (isStreaming) {
            // For streaming messages, start with empty content
            contentDiv.innerHTML = '';
            contentDiv.classList.add('streaming');
        } else {
            // For completed messages, render content
            if (sender === 'user') {
                // User messages: render as markdown but simpler
                contentDiv.innerHTML = window.marked ? marked.parse(content) : this.escapeHtml(content);
            } else {
                // Assistant/system messages: full markdown with enhancements
                contentDiv.innerHTML = window.marked ? marked.parse(content) : this.escapeHtml(content);
                this.enhanceCodeBlocks(contentDiv, content);
            }
        }
        
        // Add to messages container
        messagesContainer.appendChild(messageDiv);
        
        // Auto-scroll to bottom
        this.scrollToBottom();
        
        return messageDiv;
    }

    // Helper method for file size formatting if not available elsewhere
    formatFileSize(bytes) {
        if (bytes === 0) return '0 Bytes';
        const k = 1024;
        const sizes = ['Bytes', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }
}
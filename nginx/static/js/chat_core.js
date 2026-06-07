// Chat Core Module - Updated with new artifacts button functionality
class InternalChat {
    constructor() {
        // State - minimal, just coordination
        this.currentChatId = null;
        this.chats = new Map();
        this.isTyping = false;
        this.selectedChatForOptions = null;
        
        // Initialize subsystems
        this.redis = new ChatRedis();
        this.ollama = new ChatOllama();
        this.artifacts = new ChatArtifacts();
        this.fileUpload = new ChatFileUpload(this);
        this.ui = new ChatUI(this);
        
        // UI components
        this.chatOptionsModal = null;
        
        this.init();
    }
    
    async init() {
        console.log('Internal Chat Core initializing...');
        
        this.setupEventListeners();
        this.setupMarkdown();
        this.setupOllamaCallbacks();
        
        // Initialize chat system - delegate to Redis
        await this.redis.initializeChatSystem(this);
    }
    
    setupEventListeners() {
        // Form submission
        const chatForm = document.getElementById('chat-form');
        if (chatForm) {
            chatForm.addEventListener('submit', (e) => {
                e.preventDefault();
                this.sendMessage();
            });
        }
        
        // Input handling
        const chatInput = document.getElementById('chat-input');
        if (chatInput) {
            chatInput.addEventListener('input', () => {
                this.ui.updateCharCount();
                this.ui.autoResizeTextarea();
            });
            
            chatInput.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' && !e.shiftKey) {
                    e.preventDefault();
                    if (chatInput.value.trim() || this.fileUpload.attachedFiles.length > 0) {
                        this.sendMessage();
                    }
                }
            });
        }
        
        // Stop button
        const stopButton = document.getElementById('stop-button');
        if (stopButton) {
            stopButton.addEventListener('click', () => this.ollama.stopStream());
        }
        
        // Sidebar toggle
        const sidebarToggle = document.getElementById('sidebar-toggle');
        if (sidebarToggle) {
            sidebarToggle.addEventListener('click', () => this.ui.toggleSidebar());
        }
        
        // Chat search
        const chatSearch = document.getElementById('chat-search');
        if (chatSearch) {
            chatSearch.addEventListener('input', (e) => this.ui.searchChats(e.target.value, this.chats));
        }
        
        // Artifacts panel button (existing small button)
        const artifactsButton = document.getElementById('artifacts-button');
        if (artifactsButton) {
            artifactsButton.addEventListener('click', () => this.openArtifactsPanel());
        }
        
        // NEW: Code & AI Artifacts button (larger button with pre-selected filters)
        const codeAiArtifactsButton = document.getElementById('code-ai-artifacts-button');
        if (codeAiArtifactsButton) {
            codeAiArtifactsButton.addEventListener('click', () => this.openCodeAiArtifactsPanel());
        }
        
        // Initialize chat options modal
        this.chatOptionsModal = new bootstrap.Modal(document.getElementById('chatOptionsModal'));
    }
    
    setupMarkdown() {
        if (window.marked) {
            marked.setOptions({
                breaks: true,
                gfm: true,
                headerIds: false,
                sanitize: false
            });
        }
    }
    
    setupOllamaCallbacks() {
        // Set up streaming callback - use placeholder streaming update
        this.ollama.setChunkCallback((data) => {
            // Update chat ID if provided
            if (data.chatId && data.chatId !== this.currentChatId) {
                this.setCurrentChatId(data.chatId);
            }
            
            // Use placeholder streaming update
            this.ui.updateStreamingContent(data.content);
        });
    }
    
    // Simple state management
    setCurrentChatId(chatId) {
        this.currentChatId = chatId;
        this.artifacts.setChatId(chatId);
    }
    
    setTypingState(isTyping) {
        this.isTyping = isTyping;
        this.ui.updateButtons(isTyping);
    }
    
    // Chat Management - delegate to Redis
    async createNewChat() {
        const result = await this.redis.createNewChat();
        if (result.success) {
            this.redis.addChatToLocalCache(result, this.chats);
            await this.switchToChat(result.chat_id);
            this.ui.updateChatList(this.chats);
            return result.chat_id;
        } else {
            this.ui.showToast('Failed to create new chat', 'error');
            return null;
        }
    }
    
    async switchToChat(chatId) {
        // Validate and switch - delegate to Redis
        if (await this.redis.switchToChat(chatId, this)) {
            this.ui.updateChatListActiveState(this.currentChatId);
            this.fileUpload.clearAllFiles();
        }
    }
    
    // Improved loadChatMessages method in chat_core.js
    async loadChatMessages(chatId) {
        // Delegate to Redis for loading
        const result = await this.redis.getChatHistory(chatId);
        
        if (result.success && result.messages && result.messages.length > 0) {
            // Only render if we have messages
            this.ui.renderChatMessages(result, this.artifacts);
        } else {
            // Handle empty chat or error case
            this.ui.showEmptyChat(result.error);
        }
    }
    
    async deleteChat(chatId) {
        const result = await this.redis.deleteChat(chatId);
        if (result.success) {
            await this.redis.handleChatDeletion(chatId, this);
            this.ui.updateChatList(this.chats);
        } else {
            this.ui.showToast('Failed to delete chat', 'error');
        }
    }
    
    async refreshChatList() {
        await this.redis.refreshChatList(this);
        this.ui.updateChatList(this.chats);
    }
    
    async clearCurrentChatArtifacts() {
        if (!this.currentChatId) return false;
        
        const result = await this.redis.clearChat(this.currentChatId);
        if (result.success) {
            this.ui.clearMessagesUI();
            await this.refreshChatList();
            this.ui.updateCurrentChatTitle('New Chat');
        }
        return result.success;
    }
    
    // Message Handling with Placeholders
    async sendMessage() {
        const input = document.getElementById('chat-input');
        if (!input) return;
        const message = input.value.trim();
        if (!message && this.fileUpload.attachedFiles.length === 0) return;
        if (this.isTyping) return;

        // Prepare message - delegate to Ollama for formatting
        const { messageContent, filesToSend } = this.ollama.prepareMessage(message, this.fileUpload.attachedFiles);
        
        // Add placeholder user message for immediate feedback
        this.ui.addPlaceholderUserMessage(messageContent, filesToSend);
        
        // Add placeholder assistant message for streaming
        this.ui.addPlaceholderAssistantMessage();

        // Clear input and files
        input.value = '';
        this.fileUpload.clearAllFiles();
        this.ui.updateCharCount();
        this.ui.autoResizeTextarea();
        this.ui.hideWelcomePrompt();

        // Set typing state
        this.setTypingState(true);

        try {
            // Send to Ollama - it handles streaming and callbacks
            const stream = await this.ollama.streamMessage(message, filesToSend, this.currentChatId);

            // Handle response - delegate to UI
            if (stream.chatId && stream.chatId !== this.currentChatId) {
                this.setCurrentChatId(stream.chatId);
                this.redis.addNewChatToCache(stream.chatId, message, this.chats);
                this.ui.updateChatList(this.chats);
            }

            if (stream.content) {
                // Load real messages (this will remove placeholders and show real messages)
                this.loadChatMessages(stream.chatId);
                this.ui.updateCurrentChatTitle(this.ollama.generateTitle(message));
            }

        } catch (error) {
            // Remove placeholders and show error
            this.ui.removePlaceholderMessages();
            
            const errorInfo = this.ollama.classifyError(error);
            this.ui.addMessage('system', `Error: ${errorInfo.userFriendly}`, false);
            this.ui.showToast(errorInfo.userFriendly, 'error');
        } finally {
            this.setTypingState(false);
        }
    }
    
    // Chat Options - minimal coordination
    showChatOptions(chatId) {
        this.selectedChatForOptions = chatId;
        this.ui.showChatOptionsModal(chatId, this.chats.get(chatId), this.chatOptionsModal);
    }
    
    async renameChat(chatId, newTitle) {
        this.redis.renameChatInCache(chatId, newTitle, this.chats);
        this.ui.updateChatList(this.chats);
        if (chatId === this.currentChatId) {
            this.ui.updateCurrentChatTitle(newTitle);
        }
    }
    
    async duplicateChat(chatId) {
        // Delegate to Redis for implementation
        return await this.redis.duplicateChat(chatId);
    }
    
    // Artifact integration - simple delegation
    async getArtifactReference(artifactId) {
        return await this.artifacts.getArtifact(artifactId);
    }
    
    async searchArtifacts(query, type = null) {
        return await this.artifacts.searchArtifacts(query, type);
    }
    
    // NEW: Open artifacts panel with default filters (small button)
    openArtifactsPanel() {
        if (window.artifactsPanel && typeof window.artifactsPanel.show === 'function') {
            window.artifactsPanel.show();
        } else {
            this.ui.showToast('Artifacts panel not available', 'warning');
            console.error('window.artifactsPanel not found or show method unavailable');
        }
    }
    
    // NEW: Open artifacts panel with AI and Code filters pre-selected (large button)
    openCodeAiArtifactsPanel() {
        if (window.artifactsPanel && typeof window.artifactsPanel.show === 'function') {
            // Open the panel first
            window.artifactsPanel.show();
            
            // Set filters after a short delay to ensure panel is loaded
            setTimeout(() => {
                this.setArtifactsFilters(['jai', 'code_block']);
            }, 300);
        } else {
            this.ui.showToast('Artifacts panel not available', 'warning');
            console.error('window.artifactsPanel not found or show method unavailable');
        }
    }
    
    // Helper method to set filters in the artifacts panel
    setArtifactsFilters(types) {
        try {
            // Set the type filter dropdown
            const typeFilter = document.getElementById('artifact-type-filter');
            if (typeFilter && types.length === 1) {
                // If only one type, set it directly
                typeFilter.value = types[0];
                
                // Trigger the change event to apply the filter
                const changeEvent = new Event('change', { bubbles: true });
                typeFilter.dispatchEvent(changeEvent);
                
                this.ui.showToast(`Filtered to ${types[0] === 'jai' ? 'AI messages' : types[0] === 'code_block' ? 'code blocks' : types[0]}`, 'info');
            } else if (typeFilter && types.length > 1) {
                // For multiple types, we'll need to handle this differently
                // Since the filter dropdown only supports one type at a time,
                // we'll filter to 'code_block' as it's more specific
                const preferredType = types.includes('code_block') ? 'code_block' : types[0];
                typeFilter.value = preferredType;
                
                const changeEvent = new Event('change', { bubbles: true });
                typeFilter.dispatchEvent(changeEvent);
                
                this.ui.showToast(`Filtered to ${preferredType === 'code_block' ? 'code blocks' : 'AI messages'}`, 'info');
            }
        } catch (error) {
            console.error('Error setting artifacts filters:', error);
        }
    }
    
    // REMOVED: exportChatArtifacts method - no longer needed
    
    // Utility - simple delegation
    getChatTimestamp(chatId) {
        return this.redis.extractChatTimestamp(chatId);
    }
}
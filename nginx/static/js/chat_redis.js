// Chat Redis Module - Handles all Redis data operations
class ChatRedis {
    constructor() {
        this.baseUrl = '';
        console.log('Chat Redis module initialized');
    }
    // Initialize the entire chat system
    async initializeChatSystem(core) {
        const result = await this.getChatList();
        
        if (result.success && result.chats.length > 0) {
            // Load existing chats into core's cache
            core.chats.clear();
            result.chats.forEach(chatInfo => {
                if (this.isValidChatId(chatInfo.id)) {
                    const chat = {
                        id: chatInfo.id,
                        title: this.generateTitleFromPreview(chatInfo.preview),
                        messages: [],
                        createdAt: new Date(this.extractChatTimestamp(chatInfo.id)),
                        updatedAt: new Date(chatInfo.last_updated * 1000),
                        messageCount: chatInfo.message_count,
                        preview: chatInfo.preview
                    };
                    core.chats.set(chatInfo.id, chat);
                }
            });
            
            // Switch to most recent chat
            const sortedChats = Array.from(core.chats.values())
                .sort((a, b) => new Date(b.updatedAt) - new Date(a.updatedAt));
            
            if (sortedChats.length > 0) {
                await core.switchToChat(sortedChats[0].id);
            }
            
            core.ui.updateChatList(core.chats);
        } else {
            // Create first chat
            await core.createNewChat();
        }
    }

    // Add newly created chat to local cache
    addChatToLocalCache(result, chatsMap) {
        const chat = {
            id: result.chat_id,
            title: 'New Chat',
            messages: [],
            createdAt: new Date(result.created_at * 1000),
            updatedAt: new Date(result.created_at * 1000),
            messageCount: 0,
            preview: ''
        };
        chatsMap.set(result.chat_id, chat);
    }

    // Add new chat from streaming response
    addNewChatToCache(chatId, message, chatsMap) {
        if (!chatsMap.has(chatId)) {
            const chat = {
                id: chatId,
                title: message.length > 30 ? message.substring(0, 30) + '...' : message,
                messages: [],
                createdAt: new Date(),
                updatedAt: new Date(),
                messageCount: 0,
                preview: message
            };
            chatsMap.set(chatId, chat);
        }
    }

    // Validate and switch to chat
    async switchToChat(chatId, core) {
        if (!chatId || !this.isValidChatId(chatId)) {
            console.error('Invalid chat ID:', chatId);
            return false;
        }
        
        console.log('Switching to chat:', chatId);
        core.setCurrentChatId(chatId);
        await core.loadChatMessages(chatId);
        return true;
    }

    // Handle chat deletion logic
    async handleChatDeletion(chatId, core) {
        core.chats.delete(chatId);
        
        // If deleting current chat, switch to another or create new
        if (core.currentChatId === chatId) {
            await this.refreshChatList(core);
            const remainingChats = Array.from(core.chats.keys());
            if (remainingChats.length > 0) {
                await core.switchToChat(remainingChats[0]);
            } else {
                await core.createNewChat();
            }
        }
        
        console.log(`Deleted chat ${chatId}`);
    }

    // Refresh chat list from Redis
    async refreshChatList(core) {
        const result = await this.getChatList();
        
        if (result.success) {
            core.chats.clear();
            result.chats.forEach(chatInfo => {
                if (this.isValidChatId(chatInfo.id)) {
                    const chat = {
                        id: chatInfo.id,
                        title: this.generateTitleFromPreview(chatInfo.preview),
                        messages: [],
                        createdAt: new Date(this.extractChatTimestamp(chatInfo.id)),
                        updatedAt: new Date(chatInfo.last_updated * 1000),
                        messageCount: chatInfo.message_count,
                        preview: chatInfo.preview
                    };
                    core.chats.set(chatInfo.id, chat);
                }
            });
        }
    }

    // Rename chat in cache
    renameChatInCache(chatId, newTitle, chatsMap) {
        const chat = chatsMap.get(chatId);
        if (chat) {
            chat.title = newTitle;
        }
    }
    
    // Chat Management
    async createNewChat() {
        try {
            const response = await fetch('/api/chat/create', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            });
            
            if (!response.ok) {
                throw new Error(`Failed to create chat: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                chat_id: data.chat_id,
                created_at: data.created_at
            };
        } catch (error) {
            console.error('Redis: Failed to create new chat:', error);
            return {
                success: false,
                error: error.message
            };
        }
    }
    
    async getChatList() {
        try {
            const response = await fetch('/api/chat/list');
            if (!response.ok) {
                throw new Error(`Failed to get chat list: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                chats: data.chats || []
            };
        } catch (error) {
            console.error('Redis: Failed to load chat list:', error);
            return {
                success: false,
                error: error.message,
                chats: []
            };
        }
    }
    
    async getChatHistory(chatId) {
        if (!chatId) {
            return {
                success: false,
                error: 'Missing chat_id',
                messages: []
            };
        }
        
        try {
            const response = await fetch(`/api/chat/history?chat_id=${encodeURIComponent(chatId)}`);
            if (!response.ok) {
                throw new Error(`Failed to load chat history: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                messages: data.messages || [],
                chat_id: data.chat_id
            };
        } catch (error) {
            console.error('Redis: Failed to load chat messages:', error);
            return {
                success: false,
                error: error.message,
                messages: []
            };
        }
    }
    
    async deleteChat(chatId) {
        if (!chatId) return { success: false, error: 'Missing chat_id' };
        
        try {
            const response = await fetch('/api/chat/delete', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ chat_id: chatId })
            });
            
            if (!response.ok) {
                throw new Error(`Failed to delete chat: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                deleted_count: data.deleted_count
            };
        } catch (error) {
            console.error('Redis: Failed to delete chat:', error);
            return {
                success: false,
                error: error.message
            };
        }
    }
    
    async clearChat(chatId) {
        if (!chatId) return { success: false, error: 'Missing chat_id' };
        
        try {
            const response = await fetch('/api/chat/clear', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ chat_id: chatId })
            });
            
            if (!response.ok) {
                throw new Error(`Failed to clear chat: ${response.status}`);
            }
            
            return { success: true };
        } catch (error) {
            console.error('Redis: Failed to clear chat:', error);
            return {
                success: false,
                error: error.message
            };
        }
    }
    
    async deleteAllChats() {
        try {
            const response = await fetch('/api/chat/delete-all', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            });
            
            if (!response.ok) {
                throw new Error(`Failed to delete all chats: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                deleted_count: data.deleted_count
            };
        } catch (error) {
            console.error('Redis: Failed to delete all chats:', error);
            return {
                success: false,
                error: error.message
            };
        }
    }
    
    // Artifact Management
    async getChatArtifacts(chatId) {
        if (!chatId) return { success: false, error: 'Missing chat_id', artifacts: [] };
        
        try {
            const response = await fetch(`/api/chat/artifacts?chat_id=${encodeURIComponent(chatId)}`);
            if (!response.ok) {
                throw new Error(`Failed to get artifacts: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                artifacts: data.artifacts || []
            };
        } catch (error) {
            console.error('Redis: Failed to fetch artifacts:', error);
            return {
                success: false,
                error: error.message,
                artifacts: []
            };
        }
    }
    
    async getMessageDetails(chatId, messageId) {
        if (!chatId || !messageId) {
            return {
                success: false,
                error: 'Missing chat_id or message_id'
            };
        }
        
        try {
            const response = await fetch(`/api/message/details?chat_id=${encodeURIComponent(chatId)}&message_id=${encodeURIComponent(messageId)}`);
            if (!response.ok) {
                if (response.status === 404) {
                    return {
                        success: false,
                        error: 'Message not found'
                    };
                }
                throw new Error(`Failed to get message details: ${response.status}`);
            }
            
            const data = await response.json();
            return {
                success: true,
                message: data.message,
                artifacts: data.artifacts || []
            };
        } catch (error) {
            console.error('Redis: Failed to fetch message details:', error);
            return {
                success: false,
                error: error.message
            };
        }
    }
    
    // Utility methods
    isValidChatId(chatId) {
        return chatId && /^chat\(\d+\)$/.test(chatId);
    }
    
    extractChatTimestamp(chatId) {
        const match = chatId.match(/^chat\((\d+)\)$/);
        return match ? parseInt(match[1]) : null;
    }
    
    generateTitleFromPreview(preview) {
        if (!preview || preview.trim() === '') {
            return 'New Chat';
        }
        return preview.length > 30 ? preview.substring(0, 30) + '...' : preview;
    }
    
    // Error handling
    handleApiError(error, operation) {
        console.error(`Redis: ${operation} failed:`, error);
        return {
            success: false,
            error: error.message || 'Unknown error occurred'
        };
    }
}

// Export for use in other modules
if (typeof module !== 'undefined' && module.exports) {
    module.exports = ChatRedis;
} else if (typeof window !== 'undefined') {
    window.ChatRedis = ChatRedis;
}
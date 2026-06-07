/**
 * Chat Artifacts System - Redis Backend with admin(n)/jai(n) format
 * Format: admin(n) / jai(n) for messages, admin(n)_code(x) / jai(n)_code(x) for code blocks
 */

class ChatArtifacts {
    constructor() {
        this.chatId = null; // Current chat ID
        this.init();
    }
    
    init() {
        console.log('Chat Artifacts system initialized (Redis backend, admin/jai format)');
    }
    
    // Set current chat ID for artifact namespacing
    setChatId(chatId) {
        this.chatId = chatId;
        console.log('Chat artifacts system set to chat:', chatId);
    }
    
    // Process message element and assign IDs (called by UI when message is loaded from Redis)
    processMessageElement(messageElement, type, content, files = [], messageId = null) {
        // Use provided messageId if available (from Redis), otherwise this shouldn't be called
        if (!messageId) {
            console.error('processMessageElement called without messageId - messages should be loaded from Redis');
            return null;
        }
        
        // Add ID to message element
        messageElement.setAttribute('data-artifact-id', messageId);
        messageElement.setAttribute('data-artifact-type', this.getArtifactTypeFromId(messageId));
        
        // Add visible ID badge to message
        this.addIdBadgeToMessage(messageElement, messageId, this.getArtifactTypeFromId(messageId));
        
        // Process code blocks within the message (artifacts should already exist in Redis)
        this.processCodeBlocksInMessage(messageElement, messageId);
        
        return messageId;
    }
    
    // Get artifact type from message ID (admin -> admin, jai -> jai)
    getArtifactTypeFromId(messageId) {
        if (messageId.startsWith('admin(')) return 'admin';
        if (messageId.startsWith('jai(')) return 'jai';
        return 'unknown';
    }
    
    // Add visible ID badge to message
    addIdBadgeToMessage(messageElement, messageId, type) {
        const header = messageElement.querySelector('.message-header');
        if (header) {
            const idBadge = document.createElement('span');
            idBadge.className = `artifact-id-badge artifact-id-${type}`;
            idBadge.textContent = messageId;
            idBadge.title = `Message ID: ${messageId} (Click to copy)`;
            
            // Add click to copy functionality
            idBadge.addEventListener('click', () => {
                navigator.clipboard.writeText(messageId).then(() => {
                    this.showCopyFeedback(idBadge);
                }).catch(err => {
                    console.error('Failed to copy ID:', err);
                    // Fallback for older browsers
                    this.fallbackCopyToClipboard(messageId, idBadge);
                });
            });
            
            header.appendChild(idBadge);
        }
    }
    
    // Process code blocks within a message (artifacts already exist in Redis)
    processCodeBlocksInMessage(messageElement, parentMessageId) {
        const codeBlocks = messageElement.querySelectorAll('pre code');
        
        codeBlocks.forEach((codeElement, index) => {
            const codeBlockIndex = index + 1;
            const codeBlockId = this.getCodeBlockId(parentMessageId, codeBlockIndex);
            
            // Add ID to code block elements
            const preElement = codeElement.closest('pre');
            if (preElement) {
                preElement.setAttribute('data-artifact-id', codeBlockId);
                preElement.setAttribute('data-artifact-type', 'code_block');
                preElement.setAttribute('data-parent-id', parentMessageId);
                
                // Add visible ID badge to code block
                this.addIdBadgeToCodeBlock(preElement, codeBlockId);
            }
        });
    }
    
    // Generate code block ID with parent message ID
    getCodeBlockId(parentMessageId, codeBlockIndex) {
        return `${parentMessageId}_code(${codeBlockIndex})`;
    }
    
    // Add visible ID badge to code block
    addIdBadgeToCodeBlock(preElement, codeBlockId) {
        // Create ID badge
        const idBadge = document.createElement('span');
        idBadge.className = 'artifact-id-badge artifact-id-code';
        idBadge.textContent = codeBlockId;
        idBadge.title = `Code Block ID: ${codeBlockId} (Click to view in panel)`;
        
        // FIXED: Add click to open code panel instead of just copying
        idBadge.addEventListener('click', (e) => {
            e.stopPropagation();
            
            // Check if artifacts panel is available and has showCodePanel method
            if (window.artifactsPanel && typeof window.artifactsPanel.showCodePanel === 'function') {
                console.log(`🔍 Opening code panel for: ${codeBlockId}`);
                window.artifactsPanel.showCodePanel(codeBlockId);
            } else {
                console.warn('Artifacts panel not available, falling back to copy');
                // Fallback to copy if panel not available
                navigator.clipboard.writeText(codeBlockId).then(() => {
                    this.showCopyFeedback(idBadge);
                }).catch(err => {
                    console.error('Failed to copy code block ID:', err);
                    this.fallbackCopyToClipboard(codeBlockId, idBadge);
                });
            }
        });
        
        // ADDED: Right-click to copy ID (for users who still want to copy)
        idBadge.addEventListener('contextmenu', (e) => {
            e.preventDefault();
            e.stopPropagation();
            
            navigator.clipboard.writeText(codeBlockId).then(() => {
                this.showCopyFeedback(idBadge);
                if (window.chat && window.chat.ui) {
                    window.chat.ui.showToast('Code block ID copied!', 'success');
                }
            }).catch(err => {
                console.error('Failed to copy code block ID:', err);
                this.fallbackCopyToClipboard(codeBlockId, idBadge);
            });
        });
        
        // Position the badge in the top-left corner
        idBadge.style.cssText = `
            position: absolute;
            top: 8px;
            left: 8px;
            z-index: 11;
            padding: 2px 6px;
            font-size: 11px;
            background: rgba(0, 0, 0, 0.7);
            color: #fff;
            border-radius: 4px;
            cursor: pointer;
            user-select: none;
            transition: all 0.2s ease;
        `;
        
        // Add hover effect
        idBadge.addEventListener('mouseenter', () => {
            idBadge.style.backgroundColor = 'rgba(13, 110, 253, 0.8)';
            idBadge.style.transform = 'scale(1.05)';
        });
        
        idBadge.addEventListener('mouseleave', () => {
            idBadge.style.backgroundColor = 'rgba(0, 0, 0, 0.7)';
            idBadge.style.transform = 'scale(1)';
        });
        
        // Ensure the pre element has relative positioning
        preElement.style.position = 'relative';
        
        // Add the badge to the pre element
        preElement.appendChild(idBadge);
    }
    
    // Show copy feedback
    showCopyFeedback(element) {
        const originalText = element.textContent;
        const originalBg = element.style.backgroundColor;
        
        element.textContent = '✓';
        element.style.backgroundColor = '#28a745';
        
        setTimeout(() => {
            element.textContent = originalText;
            element.style.backgroundColor = originalBg;
        }, 1000);
    }
    
    // Fallback copy method for older browsers
    fallbackCopyToClipboard(text, element) {
        const textArea = document.createElement('textarea');
        textArea.value = text;
        document.body.appendChild(textArea);
        textArea.select();
        
        try {
            document.execCommand('copy');
            this.showCopyFeedback(element);
        } catch (err) {
            console.error('Fallback copy failed:', err);
        }
        
        document.body.removeChild(textArea);
    }
    
    // API Methods for Redis backend
    
    // Get all artifacts for current chat from Redis
    async getAllArtifacts() {
        if (!this.chatId) return [];
        
        try {
            const response = await fetch(`/api/chat/artifacts?chat_id=${this.chatId}`);
            if (response.ok) {
                const data = await response.json();
                return data.artifacts || [];
            }
        } catch (error) {
            console.error('Failed to fetch artifacts:', error);
        }
        return [];
    }
    
    // Get message details including artifacts from Redis
    async getMessageDetails(messageId) {
        if (!this.chatId || !messageId) return null;
        
        try {
            const response = await fetch(`/api/message/details?chat_id=${this.chatId}&message_id=${messageId}`);
            if (response.ok) {
                return await response.json();
            }
        } catch (error) {
            console.error('Failed to fetch message details:', error);
        }
        return null;
    }
    
    // Get artifact by ID from Redis
    async getArtifact(artifactId) {
        const artifacts = await this.getAllArtifacts();
        return artifacts.find(artifact => artifact.id === artifactId) || null;
    }
    
    // Get all message artifacts
    async getMessageArtifacts() {
        const allArtifacts = await this.getAllArtifacts();
        return allArtifacts
            .filter(artifact => artifact.type === 'admin' || artifact.type === 'jai')
            .sort((a, b) => a.timestamp - b.timestamp);
    }
    
    // Get all code block artifacts
    async getCodeBlockArtifacts() {
        const allArtifacts = await this.getAllArtifacts();
        return allArtifacts
            .filter(artifact => artifact.type === 'code_block')
            .sort((a, b) => a.timestamp - b.timestamp);
    }
    
    // Get code blocks for a specific message
    async getCodeBlocksForMessage(messageId) {
        const allArtifacts = await this.getAllArtifacts();
        return allArtifacts
            .filter(artifact => artifact.type === 'code_block' && artifact.parent_id === messageId)
            .sort((a, b) => a.id.localeCompare(b.id)); // Sort by ID to maintain order
    }
    
    // Search artifacts by content
    async searchArtifacts(query, type = null) {
        const lowerQuery = query.toLowerCase();
        const allArtifacts = await this.getAllArtifacts();
        
        return allArtifacts
            .filter(artifact => {
                if (type && artifact.type !== type) return false;
                
                const content = artifact.content || artifact.code || '';
                return content.toLowerCase().includes(lowerQuery) ||
                       artifact.id.toLowerCase().includes(lowerQuery);
            })
            .sort((a, b) => b.timestamp - a.timestamp);
    }
    
    // Export artifacts for current chat
    async exportArtifacts() {
        const artifacts = await this.getAllArtifacts();
        const exportData = {
            chatId: this.chatId,
            timestamp: Date.now(),
            artifacts: artifacts
        };
        
        return JSON.stringify(exportData, null, 2);
    }
    
    // Get artifact statistics
    async getStats() {
        const allArtifacts = await this.getAllArtifacts();
        return {
            total: allArtifacts.length,
            messages: allArtifacts.filter(a => a.type === 'admin' || a.type === 'jai').length,
            adminMessages: allArtifacts.filter(a => a.type === 'admin').length,
            jaiMessages: allArtifacts.filter(a => a.type === 'jai').length,
            codeBlocks: allArtifacts.filter(a => a.type === 'code_block').length
        };
    }
    
    // Clear artifacts for current chat (calls Redis API)
    async clearArtifacts() {
        if (!this.chatId) return false;
        
        try {
            const response = await fetch('/api/chat/clear', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ chat_id: this.chatId })
            });
            
            return response.ok;
        } catch (error) {
            console.error('Failed to clear artifacts:', error);
            return false;
        }
    }
    
    // Validate artifact ID format (updated for admin/jai)
    static isValidArtifactId(id) {
        // Message ID format: admin(n) or jai(n)
        const messagePattern = /^(admin|jai)\(\d+\)$/;
        // Code block ID format: admin(n)_code(x) or jai(n)_code(x)
        const codePattern = /^(admin|jai)\(\d+\)_code\(\d+\)$/;
        
        return messagePattern.test(id) || codePattern.test(id);
    }
    
    // Parse artifact ID to get components (updated for admin/jai)
    static parseArtifactId(id) {
        const messageMatch = id.match(/^(admin|jai)\((\d+)\)$/);
        if (messageMatch) {
            return {
                type: messageMatch[1],
                messageIndex: parseInt(messageMatch[2]),
                isCodeBlock: false
            };
        }
        
        const codeMatch = id.match(/^(admin|jai)\((\d+)\)_code\((\d+)\)$/);
        if (codeMatch) {
            return {
                type: codeMatch[1],
                messageIndex: parseInt(codeMatch[2]),
                codeIndex: parseInt(codeMatch[3]),
                isCodeBlock: true,
                parentId: `${codeMatch[1]}(${codeMatch[2]})`
            };
        }
        
        return null;
    }
}

// Export for use in other modules
if (typeof module !== 'undefined' && module.exports) {
    module.exports = ChatArtifacts;
} else if (typeof window !== 'undefined') {
    window.ChatArtifacts = ChatArtifacts;
}
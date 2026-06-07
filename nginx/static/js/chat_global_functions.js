// Global functions for UI interactions - Redis Backend

// Initialize when DOM is loaded
document.addEventListener('DOMContentLoaded', () => {
    window.chat = new InternalChat();
});

// Chat management functions
function createNewChat() {
    if (window.chat) {
        window.chat.createNewChat();
    }
}

async function clearCurrentChat() {
    if (!window.chat || !window.chat.currentChatId) return;
    
    if (!confirm('Are you sure you want to clear this chat? This action cannot be undone.')) {
        return;
    }
    
    try {
        const success = await window.chat.clearCurrentChatArtifacts();
        
        if (success) {
            // Clear UI
            const messagesContainer = document.getElementById('messages-content');
            const welcomePrompt = document.getElementById('welcome-prompt');
            
            if (messagesContainer) {
                messagesContainer.innerHTML = '';
            }
            if (welcomePrompt) {
                welcomePrompt.style.display = 'block';
            }
            
            // Reload chat list to reflect changes
            await window.chat.loadChatList();
            window.chat.ui.updateCurrentChatTitle('New Chat');
            
            console.log('Current chat and artifacts cleared from Redis');
        } else {
            alert('Failed to clear chat. Please try again.');
        }
    } catch (error) {
        console.error('Error clearing chat:', error);
        alert('An error occurred while clearing the chat.');
    }
}

async function refreshChatList() {
    if (window.chat) {
        await window.chat.loadChatList();
    }
}

async function deleteAllChats() {
    if (!window.chat) return;
    
    if (!confirm('Are you sure you want to delete ALL chats? This action cannot be undone.')) {
        return;
    }
    
    try {
        const response = await fetch('/api/chat/delete-all', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        });
        
        if (response.ok) {
            const data = await response.json();
            
            // Clear in-memory chats
            window.chat.chats.clear();
            window.chat.currentChatId = null;
            
            // Create a new chat
            window.chat.createNewChat();
            
            console.log(`All chats deleted (${data.deleted_count} items removed from Redis)`);
        } else {
            alert('Failed to delete all chats. Please try again.');
        }
    } catch (error) {
        console.error('Error deleting all chats:', error);
        alert('An error occurred while deleting chats.');
    }
}

// File upload functions
function clearAllFiles() {
    if (window.chat && window.chat.fileUpload) {
        window.chat.fileUpload.clearAllFiles();
    }
}

// Chat options functions (unchanged)
function renameChatConfirm() {
    if (!window.chat || !window.chat.selectedChatForOptions) return;
    
    const newTitle = document.getElementById('rename-chat-input').value.trim();
    if (!newTitle) {
        alert('Please enter a valid chat name.');
        return;
    }
    
    window.chat.renameChat(window.chat.selectedChatForOptions, newTitle);
    window.chat.chatOptionsModal.hide();
    window.chat.selectedChatForOptions = null;
}

async function duplicateChat() {
    if (!window.chat || !window.chat.selectedChatForOptions) return;
    
    const newChatId = await window.chat.duplicateChat(window.chat.selectedChatForOptions);
    window.chat.chatOptionsModal.hide();
    window.chat.selectedChatForOptions = null;
    
    if (newChatId) {
        console.log('Chat duplicated:', newChatId);
    }
}

async function deleteChatConfirm() {
    if (!window.chat || !window.chat.selectedChatForOptions) return;
    
    const chat = window.chat.chats.get(window.chat.selectedChatForOptions);
    if (!chat) return;
    
    if (!confirm(`Are you sure you want to delete "${chat.title}"? This action cannot be undone.`)) {
        return;
    }
    
    await window.chat.deleteChat(window.chat.selectedChatForOptions);
    window.chat.chatOptionsModal.hide();
    window.chat.selectedChatForOptions = null;
}

// Artifact management functions (Redis-based)
function showArtifactPanel() {
    if (window.artifactsPanel) {
        window.artifactsPanel.show();
    }
}

async function refreshArtifactPanel() {
    if (window.artifactsPanel) {
        await window.artifactsPanel.refresh();
    }
}

async function exportChatArtifacts() {
    if (window.chat) {
        await window.chat.exportChatArtifacts();
    }
}

async function searchChatArtifacts(query, type = null) {
    if (window.chat) {
        return await window.chat.searchArtifacts(query, type);
    }
    return [];
}

async function getArtifactById(artifactId) {
    if (window.chat) {
        return await window.chat.getArtifactReference(artifactId);
    }
    return null;
}

async function showArtifactInfo(artifactId) {
    try {
        const artifact = await getArtifactById(artifactId);
        if (artifact) {
            console.log('Artifact Info:', artifact);
            const timestamp = new Date(artifact.timestamp * 1000).toLocaleString();
            const contentLength = (artifact.content || artifact.code || '').length;
            alert(`Artifact: ${artifact.id}\nType: ${artifact.type}\nTimestamp: ${timestamp}\nContent Length: ${contentLength} characters`);
        } else {
            alert(`Artifact with ID "${artifactId}" not found.`);
        }
    } catch (error) {
        console.error('Error fetching artifact info:', error);
        alert('Failed to fetch artifact information.');
    }
}

// Utility functions for Redis-based operations
async function getChatStats() {
    if (!window.chat || !window.chat.artifacts) return null;
    
    try {
        return await window.chat.artifacts.getStats();
    } catch (error) {
        console.error('Error getting chat stats:', error);
        return null;
    }
}

async function getAllChatArtifacts() {
    if (!window.chat || !window.chat.artifacts) return [];
    
    try {
        return await window.chat.artifacts.getAllArtifacts();
    } catch (error) {
        console.error('Error getting all artifacts:', error);
        return [];
    }
}

// Debug functions for Redis inspection
async function debugChatState() {
    if (!window.chat) {
        console.log('Chat system not initialized');
        return;
    }
    
    console.log('=== Chat System Debug Info ===');
    console.log('Current Chat ID:', window.chat.currentChatId);
    console.log('Local Cache Size:', window.chat.chats.size);
    console.log('Local Chats:', Array.from(window.chat.chats.keys()));
    
    try {
        const stats = await getChatStats();
        console.log('Artifact Stats:', stats);
        
        const artifacts = await getAllChatArtifacts();
        console.log('Current Chat Artifacts:', artifacts.length);
        
        if (artifacts.length > 0) {
            console.log('Artifact IDs:', artifacts.map(a => a.id));
        }
    } catch (error) {
        console.error('Error getting debug info:', error);
    }
    
    console.log('=== End Debug Info ===');
}
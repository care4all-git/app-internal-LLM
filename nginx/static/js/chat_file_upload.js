// File upload functionality - Enhanced with size validation and better error handling
class ChatFileUpload {
    constructor(chatInstance) {
        this.chat = chatInstance;
        this.attachedFiles = [];
        
        // File size limits
        this.MAX_FILE_SIZE = 50 * 1024 * 1024; // 50MB per file
        this.MAX_TOTAL_SIZE = 100 * 1024 * 1024; // 100MB total
        this.MAX_FILES = 10; // Maximum number of files
        
        this.init();
    }
    
    init() {
        this.setupEventListeners();
        this.setupDragAndDrop();
        this.updateFileUploadUI();
        console.log('File upload system initialized with enhanced validation');
        console.log(`Limits: ${this.formatFileSize(this.MAX_FILE_SIZE)} per file, ${this.formatFileSize(this.MAX_TOTAL_SIZE)} total, ${this.MAX_FILES} files max`);
    }
    
    setupEventListeners() {
        // Attach button
        const attachButton = document.getElementById('attach-button');
        if (attachButton) {
            attachButton.addEventListener('click', () => this.triggerFileInput());
        }
        
        // File input
        const fileInput = document.getElementById('file-input');
        if (fileInput) {
            fileInput.addEventListener('change', (e) => this.handleFileSelect(e));
        }
    }
    
    setupDragAndDrop() {
        const chatContainer = document.querySelector('.chat-input-container');
        
        ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
            chatContainer.addEventListener(eventName, this.preventDefaults, false);
            document.body.addEventListener(eventName, this.preventDefaults, false);
        });
        
        ['dragenter', 'dragover'].forEach(eventName => {
            chatContainer.addEventListener(eventName, () => this.highlight(chatContainer), false);
        });
        
        ['dragleave', 'drop'].forEach(eventName => {
            chatContainer.addEventListener(eventName, () => this.unhighlight(chatContainer), false);
        });
        
        chatContainer.addEventListener('drop', (e) => this.handleDrop(e), false);
    }
    
    preventDefaults(e) {
        e.preventDefault();
        e.stopPropagation();
    }
    
    highlight(element) {
        element.classList.add('drag-over');
        this.showDragOverlay();
    }
    
    unhighlight(element) {
        element.classList.remove('drag-over');
        this.hideDragOverlay();
    }
    
    showDragOverlay() {
        let overlay = document.getElementById('drag-overlay');
        if (!overlay) {
            overlay = document.createElement('div');
            overlay.id = 'drag-overlay';
            overlay.className = 'drag-overlay';
            overlay.innerHTML = `
                <div class="drag-overlay-content">
                    <i class="bi bi-cloud-upload" style="font-size: 3rem; margin-bottom: 1rem;"></i>
                    <h4>Drop files here</h4>
                    <p>Maximum ${this.formatFileSize(this.MAX_FILE_SIZE)} per file<br>
                    Maximum ${this.MAX_FILES} files total</p>
                </div>
            `;
            document.body.appendChild(overlay);
        }
        overlay.style.display = 'flex';
    }
    
    hideDragOverlay() {
        const overlay = document.getElementById('drag-overlay');
        if (overlay) {
            overlay.style.display = 'none';
        }
    }
    
    handleDrop(e) {
        this.hideDragOverlay();
        const dt = e.dataTransfer;
        const files = dt.files;
        this.addFiles(files);
    }
    
    triggerFileInput() {
        const fileInput = document.getElementById('file-input');
        if (fileInput) {
            fileInput.click();
        }
    }
    
    handleFileSelect(e) {
        const files = e.target.files;
        this.addFiles(files);
        e.target.value = '';
    }
    
    // ENHANCED: Add files with comprehensive validation
    addFiles(files) {
        const newFiles = [];
        const errors = [];
        
        // Convert FileList to Array for easier processing
        const fileArray = Array.from(files);
        
        // Check total file count
        if (this.attachedFiles.length + fileArray.length > this.MAX_FILES) {
            errors.push(`Cannot add ${fileArray.length} files. Maximum ${this.MAX_FILES} files allowed (currently have ${this.attachedFiles.length}).`);
            return this.showFileErrors(errors);
        }
        
        fileArray.forEach(file => {
            // Check individual file size
            if (file.size > this.MAX_FILE_SIZE) {
                errors.push(`"${file.name}" is too large (${this.formatFileSize(file.size)}). Maximum size is ${this.formatFileSize(this.MAX_FILE_SIZE)}.`);
                return;
            }
            
            // Check for duplicate files
            const existingFile = this.attachedFiles.find(f => 
                f.name === file.name && 
                f.size === file.size && 
                f.lastModified === file.lastModified
            );
            
            if (existingFile) {
                errors.push(`"${file.name}" is already attached.`);
                return;
            }
            
            // File passed validation
            newFiles.push(file);
        });
        
        // Check total size after adding new files
        const currentTotalSize = this.attachedFiles.reduce((sum, file) => sum + file.size, 0);
        const newTotalSize = newFiles.reduce((sum, file) => sum + file.size, 0);
        
        if (currentTotalSize + newTotalSize > this.MAX_TOTAL_SIZE) {
            const remainingSpace = this.MAX_TOTAL_SIZE - currentTotalSize;
            errors.push(`Adding these files would exceed the total size limit of ${this.formatFileSize(this.MAX_TOTAL_SIZE)}. Available space: ${this.formatFileSize(remainingSpace)}.`);
            return this.showFileErrors(errors);
        }
        
        // Add validated files
        if (newFiles.length > 0) {
            this.attachedFiles.push(...newFiles);
            this.updateFileUploadUI();
            
            // Show success message
            if (this.chat && this.chat.ui) {
                const fileText = newFiles.length === 1 ? 'file' : 'files';
                this.chat.ui.showToast(`Added ${newFiles.length} ${fileText} successfully`, 'success');
            }
            
            console.log(`Added ${newFiles.length} files:`, newFiles.map(f => f.name));
        }
        
        // Show any errors
        if (errors.length > 0) {
            this.showFileErrors(errors);
        }
    }
    
    // Show file validation errors
    showFileErrors(errors) {
        if (this.chat && this.chat.ui) {
            const errorMessage = errors.length === 1 ? 
                errors[0] : 
                `Multiple file errors:\n• ${errors.join('\n• ')}`;
            
            this.chat.ui.showToast(errorMessage, 'error');
        } else {
            // Fallback to alert if toast system not available
            alert(errors.join('\n'));
        }
        
        console.warn('File upload errors:', errors);
    }
    
    removeFile(index) {
        if (index >= 0 && index < this.attachedFiles.length) {
            const removedFile = this.attachedFiles.splice(index, 1)[0];
            this.updateFileUploadUI();
            
            if (this.chat && this.chat.ui) {
                this.chat.ui.showToast(`Removed "${removedFile.name}"`, 'info');
            }
            
            console.log('Removed file:', removedFile.name);
        }
    }
    
    clearAllFiles() {
        const fileCount = this.attachedFiles.length;
        this.attachedFiles = [];
        this.updateFileUploadUI();
        
        if (fileCount > 0) {
            if (this.chat && this.chat.ui) {
                this.chat.ui.showToast(`Cleared ${fileCount} files`, 'info');
            }
            console.log(`Cleared ${fileCount} files`);
        }
    }
    
    // Enhanced UI update with size information
    updateFileUploadUI() {
        const fileUploadArea = document.getElementById('file-upload-area');
        const fileList = document.getElementById('file-list');
        const attachButton = document.getElementById('attach-button');
        const fileCount = document.getElementById('file-count');
        const fileCountNumber = document.getElementById('file-count-number');
        
        if (this.attachedFiles.length > 0) {
            if (fileUploadArea) {
                fileUploadArea.style.display = 'block';
            }
            
            if (fileList) {
                fileList.innerHTML = '';
                
                // Add size summary header
                const totalSize = this.attachedFiles.reduce((sum, file) => sum + file.size, 0);
                const sizeHeader = document.createElement('div');
                sizeHeader.className = 'file-size-summary';
                sizeHeader.innerHTML = `
                    <small class="text-muted">
                        Total: ${this.formatFileSize(totalSize)} / ${this.formatFileSize(this.MAX_TOTAL_SIZE)}
                        (${this.attachedFiles.length} / ${this.MAX_FILES} files)
                    </small>
                `;
                fileList.appendChild(sizeHeader);
                
                // Add individual file items
                this.attachedFiles.forEach((file, index) => {
                    const fileItem = this.createFileItem(file, index);
                    fileList.appendChild(fileItem);
                });
            }
            
            if (attachButton) {
                attachButton.classList.add('has-files');
                attachButton.title = `${this.attachedFiles.length} files attached`;
            }
            
            if (fileCount && fileCountNumber) {
                fileCount.style.display = 'inline';
                fileCountNumber.textContent = this.attachedFiles.length;
            }
        } else {
            if (fileUploadArea) {
                fileUploadArea.style.display = 'none';
            }
            
            if (attachButton) {
                attachButton.classList.remove('has-files');
                attachButton.title = 'Attach files';
            }
            
            if (fileCount) {
                fileCount.style.display = 'none';
            }
        }
    }
    
    // Enhanced file item creation with better styling
    createFileItem(file, index) {
        const fileItem = document.createElement('div');
        fileItem.className = 'file-item';
        
        const icon = this.getFileIcon(file.type);
        const size = this.formatFileSize(file.size);
        const isTextFile = this.isTextFile(file);
        
        // Add warning for large files
        const sizeWarning = file.size > this.MAX_FILE_SIZE * 0.8 ? 
            '<i class="bi bi-exclamation-triangle text-warning ms-1" title="Large file"></i>' : '';
        
        fileItem.innerHTML = `
            <i class="bi ${icon}"></i>
            <div class="file-item-info">
                <div class="file-item-name">${this.escapeHtml(file.name)}</div>
                <div class="file-item-size">
                    ${size}${sizeWarning}
                    ${isTextFile ? '<span class="badge bg-info ms-1">Text</span>' : ''}
                </div>
            </div>
            <button type="button" class="file-item-remove" onclick="window.chat.fileUpload.removeFile(${index})" title="Remove file">
                <i class="bi bi-x"></i>
            </button>
        `;
        
        return fileItem;
    }
    
    // Enhanced file icon detection
    getFileIcon(mimeType) {
        if (mimeType.startsWith('image/')) return 'bi-file-earmark-image text-primary';
        if (mimeType.startsWith('video/')) return 'bi-file-earmark-play text-danger';
        if (mimeType.startsWith('audio/')) return 'bi-file-earmark-music text-info';
        if (mimeType.includes('pdf')) return 'bi-file-earmark-pdf text-danger';
        if (mimeType.includes('word')) return 'bi-file-earmark-word text-primary';
        if (mimeType.includes('excel') || mimeType.includes('spreadsheet')) return 'bi-file-earmark-excel text-success';
        if (mimeType.includes('powerpoint') || mimeType.includes('presentation')) return 'bi-file-earmark-ppt text-warning';
        if (mimeType.includes('zip') || mimeType.includes('archive')) return 'bi-file-earmark-zip text-secondary';
        if (mimeType.includes('text') || mimeType.includes('json') || mimeType.includes('xml')) return 'bi-file-earmark-text text-success';
        return 'bi-file-earmark';
    }
    
    formatFileSize(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
    }
    
    // Enhanced text file detection
    isTextFile(file) {
        const textTypes = [
            'text/',
            'application/json',
            'application/xml',
            'application/javascript',
            'application/csv',
            'application/sql'
        ];
        
        const textExtensions = /\.(txt|md|json|xml|csv|sql|js|ts|py|java|cpp|c|h|css|html|yml|yaml|toml|ini|cfg|conf|log|readme|dockerfile)$/i;
        
        return textTypes.some(type => file.type.startsWith(type)) || 
               textExtensions.test(file.name);
    }
    
    readFileAsText(file) {
        return new Promise((resolve, reject) => {
            if (!this.isTextFile(file)) {
                reject(new Error('File is not a text file'));
                return;
            }
            
            if (file.size > 10 * 1024 * 1024) { // 10MB limit for text processing
                reject(new Error('Text file too large to process'));
                return;
            }
            
            const reader = new FileReader();
            reader.onload = (e) => resolve(e.target.result);
            reader.onerror = (e) => reject(new Error('Failed to read file: ' + e.target.error));
            reader.readAsText(file);
        });
    }
    
    // Get file upload statistics
    getUploadStats() {
        const totalSize = this.attachedFiles.reduce((sum, file) => sum + file.size, 0);
        const textFiles = this.attachedFiles.filter(file => this.isTextFile(file)).length;
        
        return {
            fileCount: this.attachedFiles.length,
            totalSize: totalSize,
            formattedTotalSize: this.formatFileSize(totalSize),
            textFiles: textFiles,
            binaryFiles: this.attachedFiles.length - textFiles,
            remainingSpace: this.MAX_TOTAL_SIZE - totalSize,
            formattedRemainingSpace: this.formatFileSize(this.MAX_TOTAL_SIZE - totalSize),
            canAddMore: this.attachedFiles.length < this.MAX_FILES && totalSize < this.MAX_TOTAL_SIZE
        };
    }
    
    // Utility function to escape HTML
    escapeHtml(text) {
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
    
    // Debug method
    logFileInfo() {
        console.log('=== File Upload Debug Info ===');
        console.log('Attached files:', this.attachedFiles.length);
        console.log('Upload stats:', this.getUploadStats());
        this.attachedFiles.forEach((file, index) => {
            console.log(`File ${index + 1}:`, {
                name: file.name,
                size: this.formatFileSize(file.size),
                type: file.type,
                isText: this.isTextFile(file)
            });
        });
        console.log('=== End Debug Info ===');
    }
}
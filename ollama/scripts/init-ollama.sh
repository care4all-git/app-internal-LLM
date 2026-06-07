#!/bin/bash

set -e

# Configuration from environment variables
MODEL_NAME="${MODEL_NAME}"
MODELFILE_PATH="${MODELFILE_PATH}"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

generate_modelfile() {
    local modelfile_path="$1"
    
    log "Generating dynamic Modelfile at: $modelfile_path"
    log "Environment variables being used:"
    log "  MODEL_GGUF_PATH: '${MODEL_GGUF_PATH}'"
    log "  MODEL_TEMPERATURE: '${MODEL_TEMPERATURE}'"
    log "  MODEL_TOP_P: '${MODEL_TOP_P}'"
    log "  MODEL_TOP_K: '${MODEL_TOP_K}'"
    log "  MODEL_MIN_P: '${MODEL_MIN_P}'"
    log "  MODEL_REPEAT_PENALTY: '${MODEL_REPEAT_PENALTY}'"
    log "  MODEL_REPEAT_LAST_N: '${MODEL_REPEAT_LAST_N}'"
    log "  MODEL_NUM_CTX: '${MODEL_NUM_CTX}'"
    log "  MODEL_NUM_PREDICT: '${MODEL_NUM_PREDICT}'"
    log "  MODEL_SEED: '${MODEL_SEED}'"
    log "  OLLAMA_GPU_LAYERS: '${OLLAMA_GPU_LAYERS}'"
    log "  OLLAMA_NUM_THREAD: '${OLLAMA_NUM_THREAD}'"
    
    # Validate required variables
    if [ -z "${MODEL_GGUF_PATH}" ]; then
        log "ERROR: MODEL_GGUF_PATH is empty!"
        return 1
    fi
    
    # Check if GGUF file exists
    if [ ! -f "${MODEL_GGUF_PATH}" ]; then
        log "ERROR: GGUF file not found at: ${MODEL_GGUF_PATH}"
        log "Contents of /root/.ollama/models/:"
        ls -la /root/.ollama/models/ || log "Directory does not exist"
        return 1
    fi
    
    # Set defaults for empty numeric values with validation
    local temp="${MODEL_TEMPERATURE}"
    local top_p="${MODEL_TOP_P}"
    local top_k="${MODEL_TOP_K}"
    local min_p="${MODEL_MIN_P}"
    local repeat_penalty="${MODEL_REPEAT_PENALTY}"
    local repeat_last_n="${MODEL_REPEAT_LAST_N}"
    local num_ctx="${MODEL_NUM_CTX}"
    local num_predict="${MODEL_NUM_PREDICT}"
    local seed="${MODEL_SEED}"
    local gpu_layers="${OLLAMA_GPU_LAYERS}"
    local num_thread="${OLLAMA_NUM_THREAD}"
    
    # Apply defaults for empty values
    [ -z "$temp" ] && temp="0.7"
    [ -z "$top_p" ] && top_p="0.9"
    [ -z "$top_k" ] && top_k="40"
    [ -z "$min_p" ] && min_p="0.05"
    [ -z "$repeat_penalty" ] && repeat_penalty="1.1"
    [ -z "$repeat_last_n" ] && repeat_last_n="64"
    [ -z "$num_ctx" ] && num_ctx="2048"
    [ -z "$num_predict" ] && num_predict="512"
    [ -z "$seed" ] && seed="0"
    [ -z "$gpu_layers" ] && gpu_layers="20"
    [ -z "$num_thread" ] && num_thread="6"
    
    log "Final values being used:"
    log "  temperature: $temp, top_p: $top_p, top_k: $top_k, min_p: $min_p"
    log "  repeat_penalty: $repeat_penalty, repeat_last_n: $repeat_last_n"
    log "  num_ctx: $num_ctx, num_predict: $num_predict, seed: $seed"
    log "  gpu_layers: $gpu_layers, num_thread: $num_thread"
    
    cat > "$modelfile_path" << EOF
FROM ${MODEL_GGUF_PATH}

# Template for chat completion
TEMPLATE """{{ if .System }}<|im_start|>system
{{ .System }}<|im_end|>
{{ end }}{{ if .Prompt }}<|im_start|>user
{{ .Prompt }}<|im_end|>
{{ end }}<|im_start|>assistant
"""

# Model parameters - ONLY VALID MODELFILE PARAMETERS
PARAMETER temperature ${temp}
PARAMETER top_p ${top_p}
PARAMETER top_k ${top_k}
PARAMETER min_p ${min_p}
PARAMETER repeat_penalty ${repeat_penalty}
PARAMETER repeat_last_n ${repeat_last_n}
PARAMETER num_ctx ${num_ctx}
PARAMETER num_predict ${num_predict}
PARAMETER seed ${seed}
PARAMETER use_mmap false
PARAMETER use_mlock true

# System message
SYSTEM """You are Devstral, a helpful AI assistant specialized in software development and coding tasks."""
EOF

    log "Modelfile generated successfully"
    log "=== GENERATED MODELFILE CONTENTS ==="
    cat "$modelfile_path"
    log "=== END MODELFILE CONTENTS ==="
}

check_model_exists() {
    local model_name="$1"
    log "Checking if model '$model_name' already exists..."
    
    if ollama list | grep -q "^$model_name"; then
        log "Model '$model_name' already exists, skipping creation"
        return 0
    else
        log "Model '$model_name' does not exist, will create it"
        return 1
    fi
}

create_model() {
    local model_name="$1"
    local modelfile_path="$2"
    
    log "Creating model '$model_name' from Modelfile: $modelfile_path"
    if ollama create "$model_name" -f "$modelfile_path"; then
        log "Model '$model_name' created successfully!"
        return 0
    else
        log "ERROR: Failed to create model '$model_name'"
        return 1
    fi
}

main() {
    log "=== Ollama Model Initialization Script ==="
    log "Model: $MODEL_NAME"
    log "Modelfile: $MODELFILE_PATH"
    
    # Export Ollama environment variables before starting server
    log "Setting Ollama environment variables..."
    export OLLAMA_HOST="${OLLAMA_HOST}"
    export OLLAMA_ORIGINS="${OLLAMA_ORIGINS}"
    export OLLAMA_MODELS="${OLLAMA_MODELS}"
    export OLLAMA_NUM_PARALLEL="${OLLAMA_NUM_PARALLEL}"
    export OLLAMA_MAX_LOADED_MODELS="${OLLAMA_MAX_LOADED_MODELS}"
    export OLLAMA_FLASH_ATTENTION="${OLLAMA_FLASH_ATTENTION}"
    export OLLAMA_MMAP="${OLLAMA_MMAP}"
    export OLLAMA_NUM_GPU="${OLLAMA_GPU_LAYERS}"
    export OLLAMA_NUM_THREAD="${OLLAMA_NUM_THREAD}"
    export OLLAMA_KEEP_ALIVE="${OLLAMA_KEEP_ALIVE}"
    
    log "Ollama environment variables set:"
    log "  OLLAMA_HOST: ${OLLAMA_HOST}"
    log "  OLLAMA_MMAP: ${OLLAMA_MMAP}"
    log "  OLLAMA_NUM_GPU: ${OLLAMA_GPU_LAYERS}"
    log "  OLLAMA_NUM_THREAD: ${OLLAMA_NUM_THREAD}"
    log "  OLLAMA_KEEP_ALIVE: ${OLLAMA_KEEP_ALIVE}"
    
    log "Starting Ollama server..."
    
    # Start Ollama server in background with exported environment
    ollama serve &
    OLLAMA_PID=$!
    
    # Give the server a moment to start
    log "Waiting for Ollama server to start..."
    sleep 5
    
    # Generate the Modelfile with environment variables
    if ! generate_modelfile "$MODELFILE_PATH"; then
        log "ERROR: Failed to generate Modelfile"
    fi
    
    # Check if model already exists
    if check_model_exists "$MODEL_NAME"; then
        log "Model initialization complete - using existing model"
    else
        # Create the model - continue even if it fails
        log "Creating model '$MODEL_NAME'..."
        if create_model "$MODEL_NAME" "$MODELFILE_PATH"; then
            log "✅ Model '$MODEL_NAME' created successfully!"
            
            # Verify the model was created
            log "Verifying model creation..."
            if ollama list | grep -q "^$MODEL_NAME"; then
                log "✅ Model '$MODEL_NAME' verified in ollama list"
            else
                log "WARNING: Model not found in ollama list after creation"
            fi
        else
            log "WARNING: Model creation failed, but keeping server running"
            log "Server will continue to run for manual debugging"
        fi
    fi
    
    log "=== Model initialization complete, keeping Ollama server running ==="
    
    # Wait for the Ollama process
    wait $OLLAMA_PID
}

# Handle shutdown gracefully
trap 'log "Received shutdown signal, stopping Ollama server..."; kill $OLLAMA_PID 2>/dev/null || true; exit 0' TERM INT

# Run main function
main "$@"
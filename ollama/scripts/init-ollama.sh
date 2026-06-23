#!/bin/bash
# Minimal Ollama entrypoint — just start the server.
# Models are managed via the web UI (/static/models.html).

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"; }

log "=== Ollama Server Start ==="

# Export server configuration from environment
export OLLAMA_HOST="${OLLAMA_HOST:-0.0.0.0:11434}"
export OLLAMA_ORIGINS="${OLLAMA_ORIGINS:-*}"
export OLLAMA_MODELS="${OLLAMA_MODELS:-/root/.ollama/models}"
export OLLAMA_NUM_PARALLEL="${OLLAMA_NUM_PARALLEL:-1}"
export OLLAMA_MAX_LOADED_MODELS="${OLLAMA_MAX_LOADED_MODELS:-1}"
export OLLAMA_FLASH_ATTENTION="${OLLAMA_FLASH_ATTENTION:-false}"
export OLLAMA_MMAP="${OLLAMA_MMAP:-false}"
export OLLAMA_NUM_GPU="${OLLAMA_GPU_LAYERS:-0}"
export OLLAMA_NUM_THREAD="${OLLAMA_NUM_THREAD:-1}"
export OLLAMA_KEEP_ALIVE="${OLLAMA_KEEP_ALIVE:-1h}"

# Pass through HuggingFace token for authenticated pulls
if [ -n "${HUGGING_FACE_HUB_TOKEN}" ]; then
    export HUGGING_FACE_HUB_TOKEN
    log "HuggingFace Hub token configured (authenticated pulls enabled)"
fi

log "OLLAMA_HOST:        ${OLLAMA_HOST}"
log "OLLAMA_NUM_GPU:     ${OLLAMA_NUM_GPU}"
log "OLLAMA_NUM_THREAD:  ${OLLAMA_NUM_THREAD}"
log "OLLAMA_KEEP_ALIVE:  ${OLLAMA_KEEP_ALIVE}"
log "OLLAMA_MODELS:      ${OLLAMA_MODELS}"
log ""
log "Use the web UI at /static/models.html to pull models."
log "Starting Ollama server..."

trap 'log "Shutting down Ollama..."; kill $OLLAMA_PID 2>/dev/null; exit 0' TERM INT

ollama serve &
OLLAMA_PID=$!

wait $OLLAMA_PID

#!/bin/bash
# Ollama startup — auto-detects GPU/CPU and configures accordingly,
# then writes hardware info to Redis for the UI calibrate tab.

set -e

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [ollama] $1"; }

REDIS_HOST="${REDIS_HOST:-internal-redis}"
REDIS_PORT="${REDIS_PORT:-6379}"

redis_set() {
    redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" "$@" >/dev/null 2>&1 || true
}

log "=== Ollama Inference Server ==="

# ── Hardware detection ────────────────────────────────────────────────────────
CPU_CORES=$(nproc)
RAM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
RAM_GB=$(awk "BEGIN {printf \"%.1f\", ${RAM_KB}/1048576}")

GPU_AVAILABLE=false
GPU_NAME=""
GPU_VRAM_MB=0
GPU_VRAM_GB=0

if nvidia-smi &>/dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -1 | xargs)
    GPU_VRAM_MB=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1 | xargs)
    GPU_VRAM_GB=$(awk "BEGIN {printf \"%.1f\", ${GPU_VRAM_MB}/1024}")
    GPU_AVAILABLE=true
    log "GPU detected: ${GPU_NAME} (${GPU_VRAM_GB} GB VRAM)"
else
    log "No GPU detected — running in CPU mode"
fi

# ── Configure Ollama based on hardware ───────────────────────────────────────
if [ "$GPU_AVAILABLE" = "true" ]; then
    DEVICE="gpu"

    # Offload all layers to GPU
    export OLLAMA_NUM_GPU=999
    export OLLAMA_FLASH_ATTENTION=true
    # Let GPU handle memory — mmap not needed
    export OLLAMA_MMAP=false
    export OLLAMA_NOPRUNE=false

    # Recommend context window based on VRAM
    if   [ "$GPU_VRAM_MB" -ge 20480 ]; then RECOMMENDED_CTX=32768
    elif [ "$GPU_VRAM_MB" -ge 12288 ]; then RECOMMENDED_CTX=16384
    elif [ "$GPU_VRAM_MB" -ge 8192  ]; then RECOMMENDED_CTX=8192
    else                                    RECOMMENDED_CTX=4096
    fi

    log "Config: GPU mode, flash_attention=true, ctx=${RECOMMENDED_CTX}"
else
    DEVICE="cpu"

    export OLLAMA_NUM_GPU=0
    export OLLAMA_NUM_THREAD=$CPU_CORES
    # mmap lets Ollama page model layers from disk — essential for large CPU models
    export OLLAMA_MMAP=true
    export OLLAMA_FLASH_ATTENTION=false
    export OLLAMA_NOPRUNE=false

    # Recommend context window based on RAM (leave 40% for OS + model overhead)
    USABLE_RAM_GB=$(awk "BEGIN {printf \"%d\", ${RAM_GB} * 0.6}")
    if   [ "$USABLE_RAM_GB" -ge 24 ]; then RECOMMENDED_CTX=16384
    elif [ "$USABLE_RAM_GB" -ge 12 ]; then RECOMMENDED_CTX=8192
    elif [ "$USABLE_RAM_GB" -ge 6  ]; then RECOMMENDED_CTX=4096
    else                                   RECOMMENDED_CTX=2048
    fi

    log "Config: CPU mode, threads=${CPU_CORES}, mmap=true, ctx=${RECOMMENDED_CTX}"
fi

# ── Write hardware info to Redis ──────────────────────────────────────────────
log "Writing hardware info to Redis..."
redis_set hset chat:hardware \
    device         "$DEVICE" \
    gpu_count      "$([ "$GPU_AVAILABLE" = "true" ] && echo 1 || echo 0)" \
    gpu_name       "$GPU_NAME" \
    gpu_vram_gb    "$GPU_VRAM_GB" \
    cpu_cores      "$CPU_CORES" \
    ram_gb         "$RAM_GB" \
    as_of          "$(date +%s)"

# Apply recommended context to chat config (only if not already set by user)
EXISTING_CTX=$(redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" hget chat:config num_ctx 2>/dev/null || echo "")
if [ -z "$EXISTING_CTX" ]; then
    redis_set hset chat:config num_ctx "$RECOMMENDED_CTX"
    log "Set default context window: ${RECOMMENDED_CTX} tokens"
fi

# ── Start Ollama ──────────────────────────────────────────────────────────────
log "Starting Ollama..."
exec ollama serve

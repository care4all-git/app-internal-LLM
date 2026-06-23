#!/bin/bash
# vLLM startup script
# Model is specified via VLLM_MODEL env var (HuggingFace model ID or local path).
# All parameters are configurable via environment variables.

set -e

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] [vLLM] $1"; }

log "=== vLLM Inference Server ==="
log "Model:              ${VLLM_MODEL:?VLLM_MODEL is required}"
log "dtype:              ${VLLM_DTYPE:-auto}"
log "Quantization:       ${VLLM_QUANTIZATION:-none}"
log "GPU memory util:    ${VLLM_GPU_MEM_UTIL:-0.90}"
log "Max model len:      ${VLLM_MAX_MODEL_LEN:-32768}"
log "Tensor parallel:    ${VLLM_TENSOR_PARALLEL:-1}"
log "Max num seqs:       ${VLLM_MAX_NUM_SEQS:-256}"

if [ -n "${HUGGING_FACE_HUB_TOKEN}" ]; then
    log "HuggingFace token:  configured (authenticated downloads)"
    export HUGGING_FACE_HUB_TOKEN
fi

# Build the vllm serve command
ARGS=(
    "${VLLM_MODEL}"
    --host 0.0.0.0
    --port 8000
    --dtype "${VLLM_DTYPE:-auto}"
    --gpu-memory-utilization "${VLLM_GPU_MEM_UTIL:-0.90}"
    --max-model-len "${VLLM_MAX_MODEL_LEN:-32768}"
    --tensor-parallel-size "${VLLM_TENSOR_PARALLEL:-1}"
    --max-num-seqs "${VLLM_MAX_NUM_SEQS:-256}"
    --trust-remote-code
    --served-model-name "${VLLM_SERVED_NAME:-${VLLM_MODEL}}"
)

# Optional quantization
if [ -n "${VLLM_QUANTIZATION}" ] && [ "${VLLM_QUANTIZATION}" != "none" ]; then
    ARGS+=(--quantization "${VLLM_QUANTIZATION}")
fi

# Optional: enable tool calling (for models that support it)
if [ "${VLLM_ENABLE_TOOLS:-false}" = "true" ]; then
    ARGS+=(--enable-auto-tool-choice --tool-call-parser mistral)
fi

# Optional: chat template override
if [ -n "${VLLM_CHAT_TEMPLATE}" ] && [ -f "${VLLM_CHAT_TEMPLATE}" ]; then
    ARGS+=(--chat-template "${VLLM_CHAT_TEMPLATE}")
fi

log "Starting: vllm serve ${ARGS[*]}"
exec vllm serve "${ARGS[@]}"

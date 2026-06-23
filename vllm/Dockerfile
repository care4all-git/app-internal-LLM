# vLLM — OpenAI-compatible inference server
# Uses the official vLLM image which includes CUDA, cuDNN, and all optimisations
# for NVIDIA GPUs (PagedAttention, continuous batching, FP8, AWQ, GPTQ, etc.)
FROM vllm/vllm-openai:latest

# Copy startup script
COPY scripts/start-vllm.sh /start-vllm.sh
RUN chmod +x /start-vllm.sh

EXPOSE 8000

ENTRYPOINT ["/start-vllm.sh"]

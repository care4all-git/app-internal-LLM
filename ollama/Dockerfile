FROM ollama/ollama:latest

# redis-cli for writing hardware info to Redis at startup
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends redis-tools && \
    rm -rf /var/lib/apt/lists/*

COPY scripts/start-ollama.sh /start-ollama.sh
RUN chmod +x /start-ollama.sh

EXPOSE 11434

ENTRYPOINT ["/start-ollama.sh"]

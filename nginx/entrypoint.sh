#!/bin/sh
set -e

REDIS_HOST="${REDIS_HOST:-internal-redis}"
REDIS_PORT="${REDIS_PORT:-6379}"

# Seed HF token into Redis so the UI HuggingFace browser works without
# the user manually entering it in Runtime Config.
if [ -n "${HF_TOKEN}" ]; then
    echo "[entrypoint] Waiting for Redis at ${REDIS_HOST}:${REDIS_PORT}..."
    for i in $(seq 1 30); do
        if redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" ping >/dev/null 2>&1; then
            redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" hset chat:config hf_token "${HF_TOKEN}" >/dev/null
            echo "[entrypoint] HF token seeded into Redis."
            break
        fi
        echo "[entrypoint] Redis not ready yet ($i/30), retrying..."
        sleep 2
    done
fi

exec /usr/local/nginx/sbin/nginx -g "daemon off;"

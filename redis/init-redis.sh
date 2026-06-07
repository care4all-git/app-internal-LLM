#!/bin/sh

echo "🚀 Starting Redis for internal chat..."

# Ensure data directory exists and has correct permissions
mkdir -p /data
chown redis:redis /data

# Start Redis with custom configuration
echo "📊 Starting Redis server..."
exec redis-server /etc/redis/redis.conf
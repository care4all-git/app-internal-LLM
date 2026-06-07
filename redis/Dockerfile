FROM redis:7.2-alpine

# Copy configuration
COPY redis.conf /etc/redis/redis.conf

# Copy initialization script
COPY init-redis.sh /usr/local/bin/init-redis.sh
RUN chmod +x /usr/local/bin/init-redis.sh

# Create data directory
RUN mkdir -p /data

# Expose port
EXPOSE 6379

# Start Redis with custom config
CMD ["/usr/local/bin/init-redis.sh"]
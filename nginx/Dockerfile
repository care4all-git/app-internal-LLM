FROM openresty/openresty:1.25.3.1-0-alpine-fat

# Install dependencies
RUN apk add --no-cache curl

# Install lua-resty-http for Ollama communication
RUN /usr/local/openresty/bin/opm get ledgetech/lua-resty-http

# Set working directory
WORKDIR /usr/local/openresty/nginx

# Remove default configuration
RUN rm -f /etc/nginx/conf.d/default.conf 2>/dev/null || true && \
    rm -f /usr/local/openresty/nginx/conf/nginx.conf

# Create directories
RUN mkdir -p /usr/local/openresty/nginx/lua && \
    mkdir -p /usr/local/openresty/nginx/static && \
    mkdir -p /var/log/nginx

# Copy configuration and files
COPY nginx.conf /usr/local/openresty/nginx/conf/nginx.conf
COPY lua/ /usr/local/openresty/nginx/lua/
COPY static/ /usr/local/openresty/nginx/static/

# Set permissions
RUN chmod -R 755 /usr/local/openresty/nginx/lua/ && \
    chmod -R 755 /usr/local/openresty/nginx/static/ && \
    chown -R nobody:nobody /var/log/nginx

# Expose port
EXPOSE 443

# Start OpenResty
CMD ["/usr/local/openresty/bin/openresty", "-g", "daemon off;"]
# ╔══════════════════════════════════════════════════════════════╗
# ║  Stage 1: Build nginx from source with the chat C module     ║
# ╚══════════════════════════════════════════════════════════════╝
FROM alpine:3.19 AS builder

ARG NGINX_VERSION=1.25.4

# Build dependencies
RUN apk add --no-cache \
        build-base \
        linux-headers \
        pcre-dev \
        openssl-dev \
        zlib-dev \
        curl \
        wget \
        hiredis-dev \
        curl-dev \
        # cJSON is embedded, but we need cmake if using system cJSON
        git

# Download nginx source
WORKDIR /build
RUN wget -q "http://nginx.org/download/nginx-${NGINX_VERSION}.tar.gz" && \
    tar xzf "nginx-${NGINX_VERSION}.tar.gz"

# Copy our C module source into the build context
COPY c_module/ /build/ngx_http_chat_module/

# Download cJSON (single-file, MIT licence) — header into headers/, source into source/
RUN wget -q -O /build/ngx_http_chat_module/headers/cJSON.h \
        "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.17/cJSON.h" && \
    wget -q -O /build/ngx_http_chat_module/source/cjson.c \
        "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.17/cJSON.c"

# Configure and build nginx with the chat module
WORKDIR /build/nginx-${NGINX_VERSION}
RUN ./configure \
        --prefix=/usr/local/nginx \
        --with-http_ssl_module \
        --with-http_v2_module \
        --with-http_realip_module \
        --with-http_stub_status_module \
        --with-threads \
        --add-module=/build/ngx_http_chat_module \
        --with-ld-opt="-lhiredis -lcurl" \
        --with-cc-opt="-I/usr/include/hiredis -I/build/ngx_http_chat_module/headers" && \
    make -j"$(nproc)" && \
    make install


# ╔══════════════════════════════════════════════════════════════╗
# ║  Stage 2: Lean runtime image                                 ║
# ╚══════════════════════════════════════════════════════════════╝
FROM alpine:3.19

# Runtime libraries only (ca-certificates needed for HTTPS to HuggingFace)
RUN apk add --no-cache \
        hiredis \
        libcurl \
        pcre \
        openssl \
        libssl3 \
        libcrypto3 \
        ca-certificates

# Copy compiled nginx
COPY --from=builder /usr/local/nginx /usr/local/nginx

# Create required directories
RUN mkdir -p \
        /usr/local/nginx/html/static/css \
        /usr/local/nginx/html/static/js \
        /var/log/nginx \
        /tmp/nginx_uploads && \
    chmod 1777 /tmp/nginx_uploads

# Copy static assets
COPY static/ /usr/local/nginx/html/static/

# Copy nginx configuration
COPY nginx.conf /usr/local/nginx/conf/nginx.conf

# Symlink logs to stdout/stderr for docker logs
RUN ln -sf /dev/stdout /var/log/nginx/access.log && \
    ln -sf /dev/stderr /var/log/nginx/error.log

EXPOSE 443

CMD ["/usr/local/nginx/sbin/nginx", "-g", "daemon off;"]

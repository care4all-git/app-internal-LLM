#ifndef NGX_HTTP_CHAT_MODULE_H
#define NGX_HTTP_CHAT_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "cJSON.h"

/* ── Path constants ─────────────────────────────────────────── */

#define CHAT_STATIC_HTML "/usr/local/nginx/html/static/chat.html"
#define CHAT_STATIC_PATH "/usr/local/nginx/html"

/* ── Main (http-level) configuration ───────────────────────── */

typedef struct {
    ngx_str_t  redis_host;
    ngx_int_t  redis_port;
    ngx_str_t  model_url;
    ngx_str_t  model_name;
    ngx_str_t  model_temperature;
    ngx_str_t  model_top_p;
    ngx_int_t  model_top_k;
    ngx_int_t  model_num_ctx;
    ngx_int_t  model_num_predict;
    ngx_str_t  model_repeat_penalty;
    ngx_str_t  admin_password;   /* plaintext, compared at runtime */
} ngx_http_chat_main_conf_t;

/* ── Location-level configuration ──────────────────────────── */

typedef struct {
    ngx_flag_t  enable;
} ngx_http_chat_loc_conf_t;

/* ── Module declaration ─────────────────────────────────────── */

extern ngx_module_t ngx_http_chat_module;

/* ── Shared helpers (used across source files) ──────────────── */

/* Return the main conf for this module from a request context. */
ngx_http_chat_main_conf_t *chat_get_main_conf(ngx_http_request_t *r);

/* Send a JSON string as an HTTP response with the given status. */
ngx_int_t chat_send_json(ngx_http_request_t *r,
    ngx_uint_t status, const char *json_str);

/* Send an HTML body as an HTTP response with the given status. */
ngx_int_t chat_send_html(ngx_http_request_t *r,
    ngx_uint_t status, const char *html, size_t html_len);

/* Collect the full request body into *out (pool-allocated, NUL-terminated). */
ngx_int_t chat_collect_body(ngx_http_request_t *r, ngx_str_t *out);

#endif /* NGX_HTTP_CHAT_MODULE_H */

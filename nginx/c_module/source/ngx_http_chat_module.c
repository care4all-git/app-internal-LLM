/*
 * ngx_http_chat_module.c
 *
 * Core nginx C module replacing all OpenResty/Lua chat logic.
 *
 * Endpoints handled:
 *   GET  /                        serve chat.html
 *   GET  /static/...              serve static files
 *   POST /api/chat/create         create new chat
 *   POST /api/chat/stream         streaming chat (SSE)
 *   POST /api/chat/continue       continuation for incomplete responses
 *   GET  /api/chat/history        message history
 *   GET  /api/chat/list           list all chats
 *   POST /api/chat/clear          clear a chat
 *   POST /api/chat/delete         delete a chat
 *   POST /api/chat/delete-all     delete all chats
 *   GET  /api/chat/artifacts      get artifacts
 *   GET  /api/chat/export         export artifacts
 *   GET  /api/message/details     message + artifacts
 *   GET  /api/health              health check
 *   GET  /api/docs                API docs HTML
 *   GET  /status                  status page
 *
 * Note on blocking: Redis (hiredis) and Ollama (libcurl) calls are
 * synchronous and will block the nginx worker.  For a single-user
 * internal tool this is acceptable.  For higher concurrency, port
 * Redis calls to ngx_thread_pool.
 */

#include "../headers/ngx_http_chat_module.h"
#include "../headers/chat_utils.h"
#include "../headers/chat_redis.h"
#include "../headers/chat_ollama.h"
#include "../headers/chat_sse.h"
#include "../headers/chat_models.h"
#include "../headers/chat_auth.h"
#include "../headers/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ════════════════════════════════════════════════════════════
 * Configuration directives
 * ════════════════════════════════════════════════════════════ */

static void *ngx_http_chat_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_chat_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_chat_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

/* Handler set functions (map directive → handler function) */
static char *ngx_http_chat_set_handler(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

/* ── Forward declarations for all handlers ───────────────── */
static ngx_int_t chat_handler_root(ngx_http_request_t *r);
static ngx_int_t chat_handler_static(ngx_http_request_t *r);
static ngx_int_t chat_handler_create(ngx_http_request_t *r);
static ngx_int_t chat_handler_stream(ngx_http_request_t *r);
static ngx_int_t chat_handler_continue(ngx_http_request_t *r);
static ngx_int_t chat_handler_history(ngx_http_request_t *r);
static ngx_int_t chat_handler_list(ngx_http_request_t *r);
static ngx_int_t chat_handler_clear(ngx_http_request_t *r);
static ngx_int_t chat_handler_delete(ngx_http_request_t *r);
static ngx_int_t chat_handler_delete_all(ngx_http_request_t *r);
static ngx_int_t chat_handler_artifacts(ngx_http_request_t *r);
static ngx_int_t chat_handler_export(ngx_http_request_t *r);
static ngx_int_t chat_handler_message_details(ngx_http_request_t *r);
static ngx_int_t chat_handler_health(ngx_http_request_t *r);
static ngx_int_t chat_handler_docs(ngx_http_request_t *r);
static ngx_int_t chat_handler_status(ngx_http_request_t *r);
/* Phase 1 — config */
static ngx_int_t chat_handler_config(ngx_http_request_t *r);
/* Phase 2 — model management */
static ngx_int_t chat_handler_models_list(ngx_http_request_t *r);
static ngx_int_t chat_handler_models_delete(ngx_http_request_t *r);
static ngx_int_t chat_handler_models_pull(ngx_http_request_t *r);
/* Phase 3 — HuggingFace browser */
static ngx_int_t chat_handler_hf_search(ngx_http_request_t *r);
static ngx_int_t chat_handler_hf_files(ngx_http_request_t *r);
/* Auth endpoints */
static ngx_int_t chat_handler_auth_status(ngx_http_request_t *r);
static ngx_int_t chat_handler_auth_login(ngx_http_request_t *r);
static ngx_int_t chat_handler_auth_logout(ngx_http_request_t *r);
/* Body callbacks */
static void chat_body_auth_login(ngx_http_request_t *r);

/* Each directive maps to one handler */
typedef struct {
    const char             *name;
    ngx_http_handler_pt     handler;
} chat_handler_map_t;

static chat_handler_map_t s_handler_map[] = {
    {"chat_root",           chat_handler_root},
    {"chat_static",         chat_handler_static},
    {"chat_create",         chat_handler_create},
    {"chat_stream",         chat_handler_stream},
    {"chat_continue",       chat_handler_continue},
    {"chat_history",        chat_handler_history},
    {"chat_list",           chat_handler_list},
    {"chat_clear",          chat_handler_clear},
    {"chat_delete",         chat_handler_delete},
    {"chat_delete_all",     chat_handler_delete_all},
    {"chat_artifacts",      chat_handler_artifacts},
    {"chat_export",         chat_handler_export},
    {"chat_message_details",chat_handler_message_details},
    {"chat_health",         chat_handler_health},
    {"chat_docs",           chat_handler_docs},
    {"chat_status",         chat_handler_status},
    {"chat_config",         chat_handler_config},
    {"chat_models_list",    chat_handler_models_list},
    {"chat_models_delete",  chat_handler_models_delete},
    {"chat_models_pull",    chat_handler_models_pull},
    {"chat_hf_search",      chat_handler_hf_search},
    {"chat_hf_files",       chat_handler_hf_files},
    {"chat_auth_status",    chat_handler_auth_status},
    {"chat_auth_login",     chat_handler_auth_login},
    {"chat_auth_logout",    chat_handler_auth_logout},
    {NULL, NULL}
};

/* Generate one ngx_command_t per handler */
static ngx_command_t ngx_http_chat_commands[] = {
    /* Main-conf options */
    { ngx_string("chat_redis_host"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, redis_host),
      NULL },

    { ngx_string("chat_redis_port"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, redis_port),
      NULL },

    { ngx_string("chat_model_url"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_url),
      NULL },

    { ngx_string("chat_model_name"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_name),
      NULL },

    { ngx_string("chat_model_temperature"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_temperature),
      NULL },

    { ngx_string("chat_model_top_p"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_top_p),
      NULL },

    { ngx_string("chat_model_top_k"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_top_k),
      NULL },

    { ngx_string("chat_model_num_ctx"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_num_ctx),
      NULL },

    { ngx_string("chat_model_repeat_penalty"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, model_repeat_penalty),
      NULL },

    { ngx_string("chat_admin_password"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_MAIN_CONF_OFFSET,
      offsetof(ngx_http_chat_main_conf_t, admin_password),
      NULL },

    /* Location-level handler directives – one per endpoint */
    { ngx_string("chat_root"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_static"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_create"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_stream"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_continue"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_history"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_list"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_clear"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_delete"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_delete_all"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_artifacts"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_export"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_message_details"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_health"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_docs"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_config"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_models_list"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_models_delete"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_models_pull"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_hf_search"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_hf_files"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_auth_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_auth_login"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    { ngx_string("chat_auth_logout"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_chat_set_handler, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },

    ngx_null_command
};

static char *ngx_http_chat_init_main_conf(ngx_conf_t *cf, void *conf);

static ngx_http_module_t ngx_http_chat_module_ctx = {
    NULL, NULL,                          /* preconfiguration / postconfiguration */
    ngx_http_chat_create_main_conf,      /* create main conf */
    ngx_http_chat_init_main_conf,        /* init main conf */
    NULL, NULL,                          /* create/merge server conf */
    ngx_http_chat_create_loc_conf,       /* create location conf */
    ngx_http_chat_merge_loc_conf         /* merge location conf */
};

ngx_module_t ngx_http_chat_module = {
    NGX_MODULE_V1,
    &ngx_http_chat_module_ctx,
    ngx_http_chat_commands,
    NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NGX_MODULE_V1_PADDING
};

/* ════════════════════════════════════════════════════════════
 * Configuration management
 * ════════════════════════════════════════════════════════════ */

static void *
ngx_http_chat_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_chat_main_conf_t *mcf;
    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_chat_main_conf_t));
    if (!mcf) return NULL;
    /* ngx_conf_set_num_slot requires NGX_CONF_UNSET (-1) so it can
       detect whether the directive appeared in nginx.conf.  Defaults are
       applied in init_main_conf, which runs after all directives parse. */
    mcf->redis_port        = NGX_CONF_UNSET;
    mcf->model_top_k       = NGX_CONF_UNSET;
    mcf->model_num_ctx     = NGX_CONF_UNSET;
    mcf->model_num_predict = NGX_CONF_UNSET;
    return mcf;
}

static char *
ngx_http_chat_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_chat_main_conf_t *mcf = conf;
    ngx_conf_init_value(mcf->redis_port,        6379);
    ngx_conf_init_value(mcf->model_top_k,       40);
    ngx_conf_init_value(mcf->model_num_ctx,     8192);
    ngx_conf_init_value(mcf->model_num_predict, -1);
    return NGX_CONF_OK;
}

static void *
ngx_http_chat_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_chat_loc_conf_t *lcf;
    lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_chat_loc_conf_t));
    if (!lcf) return NULL;
    lcf->enable = NGX_CONF_UNSET;
    return lcf;
}

static char *
ngx_http_chat_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child)
{
    ngx_http_chat_loc_conf_t *prev = parent;
    ngx_http_chat_loc_conf_t *conf = child;
    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    return NGX_CONF_OK;
}

/* ── Directive set function – registers the matching handler ─ */
static char *
ngx_http_chat_set_handler(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;
    const char               *dname;
    int                       i;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    dname = (const char *)cmd->name.data;

    for (i = 0; s_handler_map[i].name; i++) {
        if (strcmp(s_handler_map[i].name, dname) == 0) {
            clcf->handler = s_handler_map[i].handler;
            return NGX_CONF_OK;
        }
    }
    return "unknown chat directive";
}

/* ════════════════════════════════════════════════════════════
 * Shared helper: module main conf accessor
 * ════════════════════════════════════════════════════════════ */

ngx_http_chat_main_conf_t *
chat_get_main_conf(ngx_http_request_t *r)
{
    return ngx_http_get_module_main_conf(r, ngx_http_chat_module);
}

/* ════════════════════════════════════════════════════════════
 * Shared helper: send JSON response
 * ════════════════════════════════════════════════════════════ */

ngx_int_t
chat_send_json(ngx_http_request_t *r,
    ngx_uint_t status, const char *json_str)
{
    ngx_buf_t   *b;
    ngx_chain_t  out;
    size_t       len;
    u_char      *p;

    static ngx_str_t ct = ngx_string("application/json; charset=utf-8");

    len = json_str ? strlen(json_str) : 2;
    p   = ngx_pnalloc(r->pool, len);
    if (!p) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    ngx_memcpy(p, json_str ? json_str : "{}", len);

    r->headers_out.status            = status;
    r->headers_out.content_length_n  = (off_t)len;
    r->headers_out.content_type      = ct;
    r->headers_out.content_type_len  = ct.len;
    chat_set_cors_headers(r);

    ngx_http_send_header(r);

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    b->pos      = p;
    b->last     = p + len;
    b->memory   = 1;
    b->last_buf = 1;
    out.buf  = b;
    out.next = NULL;
    return ngx_http_output_filter(r, &out);
}

/* ════════════════════════════════════════════════════════════
 * Shared helper: send HTML response
 * ════════════════════════════════════════════════════════════ */

ngx_int_t
chat_send_html(ngx_http_request_t *r,
    ngx_uint_t status, const char *html, size_t html_len)
{
    ngx_buf_t   *b;
    ngx_chain_t  out;
    u_char      *p;

    static ngx_str_t ct = ngx_string("text/html; charset=utf-8");

    p = ngx_pnalloc(r->pool, html_len);
    if (!p) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    ngx_memcpy(p, html, html_len);

    r->headers_out.status            = status;
    r->headers_out.content_length_n  = (off_t)html_len;
    r->headers_out.content_type      = ct;
    r->headers_out.content_type_len  = ct.len;
    chat_set_cors_headers(r);

    ngx_http_send_header(r);

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    b->pos      = p;
    b->last     = p + html_len;
    b->memory   = 1;
    b->last_buf = 1;
    out.buf  = b;
    out.next = NULL;
    return ngx_http_output_filter(r, &out);
}

/* ════════════════════════════════════════════════════════════
 * Shared helper: read full request body into ngx_str_t
 * ════════════════════════════════════════════════════════════ */

ngx_int_t
chat_collect_body(ngx_http_request_t *r, ngx_str_t *out)
{
    ngx_chain_t  *cl;
    size_t        total = 0;
    u_char       *p;

    out->len  = 0;
    out->data = NULL;

    if (!r->request_body) return NGX_ERROR;

    for (cl = r->request_body->bufs; cl; cl = cl->next)
        total += ngx_buf_size(cl->buf);

    if (total == 0) return NGX_OK;

    p = ngx_pnalloc(r->pool, total + 1);
    if (!p) return NGX_ERROR;
    out->data = p;
    out->len  = total;

    for (cl = r->request_body->bufs; cl; cl = cl->next) {
        ngx_buf_t *b = cl->buf;
        if (b->in_file) {
            /* large body spilled to temp file */
            ssize_t n = ngx_read_file(b->file, p,
                (size_t)(b->file_last - b->file_pos), b->file_pos);
            if (n < 0) return NGX_ERROR;
            p += n;
        } else {
            size_t bsz = (size_t)(b->last - b->pos);
            ngx_memcpy(p, b->pos, bsz);
            p += bsz;
        }
    }
    *p = '\0';
    return NGX_OK;
}

/* ════════════════════════════════════════════════════════════
 * Helper: quick JSON error/success responses
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
send_api_error(ngx_http_request_t *r,
    ngx_uint_t status, const char *error, const char *details)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", error ? error : "Error");
    cJSON_AddNumberToObject(obj, "status", (double)status);
    cJSON_AddNumberToObject(obj, "timestamp", (double)time(NULL));
    if (details) cJSON_AddStringToObject(obj, "details", details);
    char *js = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    ngx_int_t rc = chat_send_json(r, status, js);
    free(js);
    return rc;
}

static ngx_int_t
send_api_success(ngx_http_request_t *r,
    ngx_uint_t status, cJSON *data, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    if (message) cJSON_AddStringToObject(resp, "message", message);
    if (data) {
        /* Merge data keys into resp */
        cJSON *item = data->child;
        while (item) {
            cJSON_AddItemToObject(resp,
                item->string, cJSON_Duplicate(item, 1));
            item = item->next;
        }
    }
    char *js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    ngx_int_t rc = chat_send_json(r, status, js);
    free(js);
    return rc;
}

/* ════════════════════════════════════════════════════════════
 * Helper: get Redis connection from main conf
 * ════════════════════════════════════════════════════════════ */

static redisContext *
get_redis(ngx_http_request_t *r)
{
    ngx_http_chat_main_conf_t *mcf = chat_get_main_conf(r);
    char host[128] = "redis";
    int  port      = 6379;

    if (mcf->redis_host.len > 0 && mcf->redis_host.len < sizeof(host)) {
        ngx_memcpy(host, mcf->redis_host.data, mcf->redis_host.len);
        host[mcf->redis_host.len] = '\0';
    }
    if (mcf->redis_port > 0) port = (int)mcf->redis_port;

    return chat_redis_connect(host, port, r->connection->log);
}

/* ════════════════════════════════════════════════════════════
 * Auth helpers
 * ════════════════════════════════════════════════════════════ */

/* Returns 1 if request has valid user session (uid cookie) or admin session */
static int
chat_check_user_auth(ngx_http_request_t *r)
{
    redisContext    *rc  = get_redis(r);
    chat_auth_info_t info;

    if (!rc) return 1;   /* Redis down — allow (fallback mode) */

    /* Admin session always counts */
    if (chat_auth_is_admin(r, rc, r->connection->log)) {
        redisFree(rc);
        return 1;
    }

    chat_auth_identify(r, rc, &info);
    redisFree(rc);

    return (info.type == CHAT_AUTH_USER || info.type == CHAT_AUTH_NEW);
}

/* Returns 1 if request has valid admin_sid cookie */
static int
chat_check_admin_auth(ngx_http_request_t *r)
{
    redisContext *rc  = get_redis(r);
    int           ok;
    if (!rc) return 0;
    ok = chat_auth_is_admin(r, rc, r->connection->log);
    redisFree(rc);
    return ok;
}

/* ════════════════════════════════════════════════════════════
 * Effective config: Redis overrides nginx.conf defaults
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    char   model_url[256];
    char   model_name[128];
    double temperature;
    double top_p;
    int    top_k;
    int    num_ctx;
    int    num_predict;
    double repeat_penalty;
    char   hf_token[256];
} chat_effective_config_t;

static void
chat_resolve_config(ngx_http_request_t *r, chat_effective_config_t *cfg)
{
    ngx_http_chat_main_conf_t *mcf = chat_get_main_conf(r);
    redisContext              *rc;
    chat_redis_config_t        redis_cfg;

    /* Hard-coded fallbacks */
    strncpy(cfg->model_url,  "http://ollama:11434", sizeof(cfg->model_url)  - 1);
    strncpy(cfg->model_name, "devstral",            sizeof(cfg->model_name) - 1);
    cfg->temperature   = 0.7;
    cfg->top_p         = 0.9;
    cfg->top_k         = 40;
    cfg->num_ctx       = 8192;
    cfg->num_predict   = -1;
    cfg->repeat_penalty = 1.1;
    cfg->hf_token[0]   = '\0';

    /* Apply nginx.conf values */
    if (mcf->model_url.len > 0)
        snprintf(cfg->model_url, sizeof(cfg->model_url),
            "%.*s", (int)mcf->model_url.len, mcf->model_url.data);
    if (mcf->model_name.len > 0)
        snprintf(cfg->model_name, sizeof(cfg->model_name),
            "%.*s", (int)mcf->model_name.len, mcf->model_name.data);
    if (mcf->model_temperature.len > 0)
        cfg->temperature = atof((char *)mcf->model_temperature.data);
    if (mcf->model_top_p.len > 0)
        cfg->top_p = atof((char *)mcf->model_top_p.data);
    if (mcf->model_top_k > 0)   cfg->top_k  = (int)mcf->model_top_k;
    if (mcf->model_num_ctx > 0)  cfg->num_ctx = (int)mcf->model_num_ctx;
    if (mcf->model_repeat_penalty.len > 0)
        cfg->repeat_penalty = atof((char *)mcf->model_repeat_penalty.data);

    /* Override with Redis values if present */
    rc = get_redis(r);
    if (!rc) return;

    memset(&redis_cfg, 0, sizeof(redis_cfg));
    chat_redis_get_config(rc, r->connection->log, &redis_cfg);
    redisFree(rc);

    if (!redis_cfg.found) return;

    if (redis_cfg.model_url[0])
        strncpy(cfg->model_url,  redis_cfg.model_url,
            sizeof(cfg->model_url)  - 1);
    if (redis_cfg.model_name[0])
        strncpy(cfg->model_name, redis_cfg.model_name,
            sizeof(cfg->model_name) - 1);
    if (redis_cfg.temperature[0])
        cfg->temperature   = atof(redis_cfg.temperature);
    if (redis_cfg.top_p[0])
        cfg->top_p         = atof(redis_cfg.top_p);
    if (redis_cfg.top_k[0])
        cfg->top_k         = atoi(redis_cfg.top_k);
    if (redis_cfg.num_ctx[0])
        cfg->num_ctx       = atoi(redis_cfg.num_ctx);
    if (redis_cfg.num_predict[0])
        cfg->num_predict   = atoi(redis_cfg.num_predict);
    if (redis_cfg.repeat_penalty[0])
        cfg->repeat_penalty = atof(redis_cfg.repeat_penalty);
    if (redis_cfg.hf_token[0])
        strncpy(cfg->hf_token, redis_cfg.hf_token,
            sizeof(cfg->hf_token) - 1);
}

/* ════════════════════════════════════════════════════════════
 * Helper: extract code blocks and save artifacts
 * ════════════════════════════════════════════════════════════ */

/*
 * Scan `content` for ```lang\ncode\n``` blocks.
 * Returns a cJSON array of artifact_id strings (caller must free).
 */
static cJSON *
extract_and_save_artifacts(redisContext *rc,
    ngx_log_t *log,
    const char *chat_id,
    const char *message_id,
    const char *content)
{
    cJSON      *ids = cJSON_CreateArray();
    const char *p   = content;
    int         idx = 0;

    while ((p = strstr(p, "```")) != NULL) {
        const char *lang_start, *lang_end;
        const char *code_start, *code_end;
        char        lang[64], artifact_id[128], meta_json[256];
        char       *code_copy;
        size_t      code_len;

        p += 3; /* skip ``` */
        lang_start = p;
        while (*p && *p != '\n') p++;
        lang_end = p;

        /* copy language tag */
        size_t llen = (size_t)(lang_end - lang_start);
        if (llen >= sizeof(lang)) llen = sizeof(lang) - 1;
        memcpy(lang, lang_start, llen);
        lang[llen] = '\0';

        if (*p == '\n') p++;
        code_start = p;

        /* find closing ``` on its own line */
        code_end = strstr(p, "\n```");
        if (!code_end) break;
        code_len  = (size_t)(code_end - code_start);
        code_copy = malloc(code_len + 1);
        if (!code_copy) break;
        memcpy(code_copy, code_start, code_len);
        code_copy[code_len] = '\0';

        idx++;
        chat_generate_artifact_id(artifact_id, sizeof(artifact_id),
            message_id, idx);

        snprintf(meta_json, sizeof(meta_json),
            "{\"extracted_from_response\":true,"
            "\"block_index\":%d,"
            "\"extraction_timestamp\":%lld}",
            idx, (long long)time(NULL));

        chat_redis_save_artifact(rc, log,
            chat_id, artifact_id, message_id,
            code_copy, lang, meta_json);
        free(code_copy);

        cJSON_AddItemToArray(ids,
            cJSON_CreateString(artifact_id));

        p = code_end + 4; /* skip \n``` */
    }

    return ids;
}

/* ════════════════════════════════════════════════════════════
 * Body-read callback types for POST handlers
 * ════════════════════════════════════════════════════════════ */

static void chat_body_create(ngx_http_request_t *r);
static void chat_body_stream(ngx_http_request_t *r);
static void chat_body_continue(ngx_http_request_t *r);
static void chat_body_clear(ngx_http_request_t *r);
static void chat_body_delete(ngx_http_request_t *r);
static void chat_body_delete_all(ngx_http_request_t *r);
static void chat_body_config(ngx_http_request_t *r);
static void chat_body_models_delete(ngx_http_request_t *r);
static void chat_body_models_pull(ngx_http_request_t *r);

/* ════════════════════════════════════════════════════════════
 * GET /  — serve chat.html
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_root(ngx_http_request_t *r)
{
    FILE   *f;
    char   *buf;
    size_t  sz;
    ngx_int_t rc;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD)
        return NGX_HTTP_NOT_ALLOWED;

    f = fopen(CHAT_STATIC_HTML, "rb");
    if (!f) return NGX_HTTP_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    sz = (size_t)ftell(f);
    rewind(f);
    buf = malloc(sz);
    if (!buf) { fclose(f); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    fread(buf, 1, sz, f);
    fclose(f);

    chat_add_header(r, "Cache-Control", "public, max-age=300");
    rc = chat_send_html(r, NGX_HTTP_OK, buf, sz);
    free(buf);
    return rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /static/...
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_static(ngx_http_request_t *r)
{
    char        path[512];
    FILE       *f;
    char       *buf;
    size_t      sz;
    const char *mime;
    ngx_int_t   rc;
    ngx_str_t   ct;
    u_char     *ctp;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD)
        return NGX_HTTP_NOT_ALLOWED;

    /* Build filesystem path */
    snprintf(path, sizeof(path), "%s%.*s",
        CHAT_STATIC_PATH,
        (int)r->uri.len, r->uri.data);

    f = fopen(path, "rb");
    if (!f) return NGX_HTTP_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    sz = (size_t)ftell(f);
    rewind(f);
    buf = malloc(sz);
    if (!buf) { fclose(f); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    fread(buf, 1, sz, f);
    fclose(f);

    mime = chat_mime_for_path(path);
    ctp  = ngx_pnalloc(r->pool, strlen(mime));
    if (!ctp) { free(buf); return NGX_HTTP_INTERNAL_SERVER_ERROR; }
    ngx_memcpy(ctp, mime, strlen(mime));
    ct.data = ctp;
    ct.len  = strlen(mime);

    r->headers_out.content_type     = ct;
    r->headers_out.content_type_len = ct.len;
    chat_add_header(r, "Cache-Control", "public, max-age=3600");

    rc = chat_send_html(r, NGX_HTTP_OK, buf, sz);
    free(buf);
    return rc;
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/create
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_create(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_create);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_create(ngx_http_request_t *r)
{
    char       chat_id[64];
    redisContext *rc;
    cJSON      *resp;

    chat_generate_chat_id(chat_id, sizeof(chat_id));
    rc = get_redis(r);
    if (!rc) {
        send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
            "Failed to connect to Redis", NULL);
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    if (!chat_redis_create_chat(rc, r->connection->log, chat_id)) {
        redisFree(rc);
        send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
            "Failed to create chat", NULL);
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }
    redisFree(rc);

    resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "chat_id",    chat_id);
    cJSON_AddNumberToObject(resp, "created_at", (double)time(NULL));
    send_api_success(r, 201, resp, "Chat created successfully");
    cJSON_Delete(resp);

    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/stream  (SSE)
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_stream(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;

    /* Validate SSE support */
    if (!chat_sse_validate_client(r)) {
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "SSE not supported",
            "Client doesn't accept text/event-stream");
    }

    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_stream);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_stream(ngx_http_request_t *r)
{
    ngx_str_t    body;
    cJSON       *req_json;
    const char  *user_message, *req_chat_id;
    char         chat_id[64];
    cJSON       *context;
    long long    counter;
    char         msg_id[128];
    cJSON       *artifacts_json;
    char        *artifacts_str;
    chat_ollama_result_t    ollama;
    chat_effective_config_t cfg;
    redisContext *rc;

    /* Resolve effective config: Redis overrides nginx.conf */
    chat_resolve_config(r, &cfg);

    /* ── Read and parse body ────────────────────────────── */
    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "No request body", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    req_json = cJSON_Parse((char *)body.data);
    if (!req_json) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Invalid JSON", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    cJSON *msg_item  = cJSON_GetObjectItem(req_json, "message");
    cJSON *cid_item  = cJSON_GetObjectItem(req_json, "chat_id");
    user_message  = (msg_item  && msg_item->valuestring)  ? msg_item->valuestring  : "";
    req_chat_id   = (cid_item  && cid_item->valuestring)  ? cid_item->valuestring  : "";

    /* ── Resolve/create chat ID ─────────────────────────── */
    if (!req_chat_id || *req_chat_id == '\0') {
        chat_generate_chat_id(chat_id, sizeof(chat_id));
    } else if (!chat_is_valid_chat_id(req_chat_id)) {
        cJSON_Delete(req_json);
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Invalid chat ID format", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    } else {
        strncpy(chat_id, req_chat_id, sizeof(chat_id) - 1);
        chat_id[sizeof(chat_id) - 1] = '\0';
    }

    /* ── Redis: get context + save user message ─────────── */
    rc = get_redis(r);
    if (!rc) {
        cJSON_Delete(req_json);
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Redis connection failed", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    /* Ensure chat exists */
    chat_redis_create_chat(rc, r->connection->log, chat_id);

    context = chat_redis_get_context(rc, r->connection->log, chat_id, 10);
    if (!context) context = cJSON_CreateArray();

    /* Save user message */
    counter = chat_redis_incr_counter(rc, r->connection->log, chat_id, "admin");
    if (counter > 0) {
        chat_generate_message_id(msg_id, sizeof(msg_id), "user", counter);
        chat_redis_save_message(rc, r->connection->log,
            chat_id, msg_id, "user", user_message, "[]", "[]");
    }
    redisFree(rc);

    /* ── Build Ollama context ────────────────────────────── */
    cJSON *user_entry = cJSON_CreateObject();
    cJSON_AddStringToObject(user_entry, "role",    "user");
    cJSON_AddStringToObject(user_entry, "content", user_message);
    cJSON_AddItemToArray(context, user_entry);

    /* ── Set up SSE and stream ──────────────────────────── */
    chat_sse_setup_headers(r);
    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_send_header(r);

    chat_sse_send_chat_id(r, chat_id);
    chat_sse_send_status(r, "processing", "Generating complete response...");

    /* Call Ollama (blocks this worker) */
    int ok = chat_ollama_stream_chat(r->connection->log,
        cfg.model_url, cfg.model_name,
        cfg.temperature, cfg.top_p, cfg.top_k, cfg.num_ctx, cfg.repeat_penalty,
        context, &ollama);

    cJSON_Delete(context);
    cJSON_Delete(req_json);

    if (!ok) {
        char friendly[512];
        chat_ollama_friendly_error(ollama.error, friendly, sizeof(friendly));
        chat_sse_send_error(r, friendly, ollama.error);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    /* Stream chunks to client */
    chat_sse_stream_result(r, &ollama);

    /* ── Save AI response to Redis ──────────────────────── */
    rc = get_redis(r);
    if (rc && ollama.full_response) {
        char ai_msg_id[128];
        counter = chat_redis_incr_counter(rc, r->connection->log, chat_id, "jai");
        if (counter > 0) {
            chat_generate_message_id(ai_msg_id, sizeof(ai_msg_id),
                "assistant", counter);
            artifacts_json = extract_and_save_artifacts(rc,
                r->connection->log, chat_id, ai_msg_id,
                ollama.full_response);
            artifacts_str = cJSON_PrintUnformatted(artifacts_json);
            chat_redis_save_message(rc, r->connection->log,
                chat_id, ai_msg_id, "assistant",
                ollama.full_response, "[]",
                artifacts_str ? artifacts_str : "[]");
            if (artifacts_str) free(artifacts_str);
            cJSON_Delete(artifacts_json);
        }
        redisFree(rc);
    }

    /* Close SSE stream */
    chat_sse_send_done(r,
        ollama.completion.is_complete,
        ollama.completion.completion_reason);

    if (ollama.full_response) free(ollama.full_response);
    if (ollama.chunks)        cJSON_Delete(ollama.chunks);

    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/continue
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_continue(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    if (!chat_sse_validate_client(r))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "SSE not supported", NULL);
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_continue);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_continue(ngx_http_request_t *r)
{
    ngx_str_t   body;
    cJSON      *req_json;
    const char *chat_id_str, *prev_response;
    cJSON      *context;
    cJSON      *cont_context;
    chat_ollama_result_t    ollama;
    chat_effective_config_t cfg;

    /* Resolve effective config: Redis overrides nginx.conf */
    chat_resolve_config(r, &cfg);

    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "No request body", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    req_json = cJSON_Parse((char *)body.data);
    if (!req_json) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Invalid JSON", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    cJSON *cid_item  = cJSON_GetObjectItem(req_json, "chat_id");
    cJSON *prev_item = cJSON_GetObjectItem(req_json, "previous_response");
    chat_id_str   = (cid_item  && cid_item->valuestring)  ? cid_item->valuestring  : "";
    prev_response = (prev_item && prev_item->valuestring)  ? prev_item->valuestring : "";

    if (!chat_is_valid_chat_id(chat_id_str) || *prev_response == '\0') {
        cJSON_Delete(req_json);
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Missing chat_id or previous_response", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    /* Get context */
    redisContext *rdc = get_redis(r);
    context = rdc
        ? chat_redis_get_context(rdc, r->connection->log, chat_id_str, 10)
        : cJSON_CreateArray();
    if (!context) context = cJSON_CreateArray();
    if (rdc) redisFree(rdc);

    /* Build continuation context: drop last message, add continuation prompt */
    cont_context = cJSON_CreateArray();
    int n = cJSON_GetArraySize(context);
    for (int i = 0; i < n - 1; i++) {
        cJSON_AddItemToArray(cont_context,
            cJSON_Duplicate(cJSON_GetArrayItem(context, i), 1));
    }
    cJSON_Delete(context);

    cJSON *cont_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(cont_msg, "role",
        "user");
    cJSON_AddStringToObject(cont_msg, "content",
        "Please continue your previous response from where you left off. "
        "Complete your full answer without repeating what you already said.");
    cJSON_AddItemToArray(cont_context, cont_msg);

    /* Stream */
    chat_sse_setup_headers(r);
    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_send_header(r);

    chat_sse_send_status(r, "continuing", "Continuing previous response...");

    int ok = chat_ollama_stream_chat(r->connection->log,
        cfg.model_url, cfg.model_name,
        cfg.temperature, cfg.top_p, cfg.top_k, cfg.num_ctx, cfg.repeat_penalty,
        cont_context, &ollama);

    cJSON_Delete(cont_context);
    cJSON_Delete(req_json);

    if (!ok) {
        char friendly[512];
        chat_ollama_friendly_error(ollama.error, friendly, sizeof(friendly));
        chat_sse_send_error(r, friendly, ollama.error);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK);
        return;
    }

    chat_sse_stream_result(r, &ollama);

    /* Combine with previous and update Redis */
    if (ollama.full_response) {
        size_t combined_len = strlen(prev_response) + strlen(ollama.full_response) + 1;
        char *combined = malloc(combined_len);
        if (combined) {
            snprintf(combined, combined_len, "%s%s",
                prev_response, ollama.full_response);

            rdc = get_redis(r);
            if (rdc) {
                /* Update last assistant message */
                cJSON *msgs = chat_redis_get_messages(rdc,
                    r->connection->log, chat_id_str, 5);
                if (msgs) {
                    int m = cJSON_GetArraySize(msgs);
                    for (int i = m - 1; i >= 0; i--) {
                        cJSON *msg  = cJSON_GetArrayItem(msgs, i);
                        cJSON *role = cJSON_GetObjectItem(msg, "role");
                        cJSON *mid  = cJSON_GetObjectItem(msg, "id");
                        if (role && role->valuestring &&
                            strcmp(role->valuestring, "assistant") == 0 &&
                            mid && mid->valuestring)
                        {
                            cJSON *art_ids = extract_and_save_artifacts(rdc,
                                r->connection->log, chat_id_str,
                                mid->valuestring, combined);
                            char *art_str = cJSON_PrintUnformatted(art_ids);
                            chat_redis_save_message(rdc, r->connection->log,
                                chat_id_str, mid->valuestring,
                                "assistant", combined, "[]",
                                art_str ? art_str : "[]");
                            if (art_str) free(art_str);
                            cJSON_Delete(art_ids);
                            break;
                        }
                    }
                    cJSON_Delete(msgs);
                }
                redisFree(rdc);
            }
            free(combined);
        }
    }

    chat_sse_send_done(r,
        ollama.completion.is_complete,
        ollama.completion.completion_reason);

    if (ollama.full_response) free(ollama.full_response);
    if (ollama.chunks)        cJSON_Delete(ollama.chunks);

    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * GET /api/chat/history
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_history(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    ngx_str_t   chat_id_val;
    char        chat_id[128];
    redisContext *rc;
    cJSON       *messages, *resp;
    char        *js;
    ngx_int_t    n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    /* Parse ?chat_id= */
    if (ngx_http_arg(r, (u_char *)"chat_id", 7, &chat_id_val) != NGX_OK ||
        chat_id_val.len == 0)
    {
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing chat_id parameter", NULL);
    }
    if (chat_id_val.len >= sizeof(chat_id)) return NGX_HTTP_BAD_REQUEST;
    ngx_memcpy(chat_id, chat_id_val.data, chat_id_val.len);
    chat_id[chat_id_val.len] = '\0';

    if (!chat_is_valid_chat_id(chat_id))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Invalid chat_id format", NULL);

    rc = get_redis(r);
    if (!rc) return send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
        "Redis connection failed", NULL);

    messages = chat_redis_get_messages(rc, r->connection->log, chat_id, 100);
    redisFree(rc);
    if (!messages) messages = cJSON_CreateArray();

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    cJSON_AddItemToObject(resp, "messages", messages);
    cJSON_AddNumberToObject(resp, "message_count",
        (double)cJSON_GetArraySize(messages));
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/chat/list
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_list(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    redisContext *rc;
    cJSON       *chats, *resp;
    char        *js;
    ngx_int_t    n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    rc = get_redis(r);
    if (!rc) return send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
        "Redis connection failed", NULL);

    chats = chat_redis_get_chat_list(rc, r->connection->log);
    redisFree(rc);
    if (!chats) chats = cJSON_CreateArray();

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddItemToObject(resp, "chats", chats);
    cJSON_AddNumberToObject(resp, "count", (double)cJSON_GetArraySize(chats));
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/clear
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_clear(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_clear);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_clear(ngx_http_request_t *r)
{
    ngx_str_t    body;
    cJSON       *req;
    const char  *chat_id;
    redisContext *rc;
    long long    deleted;
    cJSON       *resp;
    char        *js;

    if (chat_collect_body(r, &body) != NGX_OK) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "No body", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }
    req = cJSON_Parse((char *)body.data);
    if (!req) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "Invalid JSON", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }
    cJSON *cid = cJSON_GetObjectItem(req, "chat_id");
    chat_id = (cid && cid->valuestring) ? cid->valuestring : "";
    if (!chat_is_valid_chat_id(chat_id)) {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "Invalid chat_id", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    rc = get_redis(r);
    if (!rc) {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
            "Redis connection failed", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }
    deleted = chat_redis_clear_chat(rc, r->connection->log, chat_id);
    redisFree(rc);
    cJSON_Delete(req);

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddStringToObject(resp, "operation",  "clear_chat");
    cJSON_AddNumberToObject(resp, "affected_count", (double)deleted);
    cJSON_AddNumberToObject(resp, "timestamp",  (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/delete
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_delete(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_delete);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_delete(ngx_http_request_t *r)
{
    /* Same as clear for now */
    chat_body_clear(r);
}

/* ════════════════════════════════════════════════════════════
 * POST /api/chat/delete-all
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_delete_all(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_delete_all);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_delete_all(ngx_http_request_t *r)
{
    redisContext *rc = get_redis(r);
    long long deleted = 0;
    cJSON *resp;
    char  *js;

    if (rc) {
        deleted = chat_redis_delete_all(rc, r->connection->log);
        redisFree(rc);
    }

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddStringToObject(resp, "operation",  "delete_all_chats");
    cJSON_AddNumberToObject(resp, "affected_count", (double)deleted);
    cJSON_AddNumberToObject(resp, "timestamp",  (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * GET /api/chat/artifacts
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_artifacts(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    ngx_str_t    chat_id_val;
    char         chat_id[128];
    redisContext *rc;
    cJSON       *arts, *resp;
    char        *js;
    ngx_int_t    n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    if (ngx_http_arg(r, (u_char *)"chat_id", 7, &chat_id_val) != NGX_OK ||
        chat_id_val.len == 0)
    {
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing chat_id parameter", NULL);
    }
    ngx_memcpy(chat_id, chat_id_val.data,
        chat_id_val.len < sizeof(chat_id) ? chat_id_val.len : sizeof(chat_id)-1);
    chat_id[chat_id_val.len] = '\0';

    if (!chat_is_valid_chat_id(chat_id))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Invalid chat_id format", NULL);

    rc = get_redis(r);
    if (!rc) return send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
        "Redis connection failed", NULL);

    arts = chat_redis_get_artifacts(rc, r->connection->log, chat_id);
    redisFree(rc);
    if (!arts) arts = cJSON_CreateArray();

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    cJSON_AddItemToObject(resp, "artifacts", arts);
    cJSON_AddNumberToObject(resp, "artifact_count",
        (double)cJSON_GetArraySize(arts));
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/chat/export
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_export(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    ngx_str_t    val;
    char         chat_id[128], format[16] = "json";
    redisContext *rdc;
    cJSON       *arts, *export_obj;
    char        *js;
    ngx_int_t    n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    if (ngx_http_arg(r, (u_char *)"chat_id", 7, &val) != NGX_OK)
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing chat_id parameter", NULL);

    ngx_memcpy(chat_id, val.data,
        val.len < sizeof(chat_id) ? val.len : sizeof(chat_id)-1);
    chat_id[val.len] = '\0';

    if (!chat_is_valid_chat_id(chat_id))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Invalid chat_id format", NULL);

    if (ngx_http_arg(r, (u_char *)"format", 6, &val) == NGX_OK &&
        val.len < sizeof(format))
    {
        ngx_memcpy(format, val.data, val.len);
        format[val.len] = '\0';
    }

    rdc = get_redis(r);
    if (!rdc) return send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
        "Redis connection failed", NULL);

    arts = chat_redis_get_artifacts(rdc, r->connection->log, chat_id);
    redisFree(rdc);
    if (!arts) arts = cJSON_CreateArray();

    /* Currently only JSON export */
    export_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(export_obj, "export_timestamp", (double)time(NULL));
    cJSON_AddStringToObject(export_obj, "export_format",    format);
    cJSON_AddNumberToObject(export_obj, "artifact_count",
        (double)cJSON_GetArraySize(arts));
    cJSON_AddItemToObject(export_obj, "artifacts", arts);

    js = cJSON_PrintUnformatted(export_obj);
    cJSON_Delete(export_obj);

    chat_add_header(r, "Content-Disposition",
        "attachment; filename=artifacts_export.json");
    n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/message/details
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_message_details(ngx_http_request_t *r)
{
    if (!chat_check_user_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Authentication required", NULL);
    ngx_str_t    val;
    char         chat_id[128], msg_id[128];
    redisContext *rdc;
    cJSON       *messages, *msg = NULL, *arts, *resp;
    char        *js;
    ngx_int_t    n_rc;
    int          i, n;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    if (ngx_http_arg(r, (u_char *)"chat_id", 7, &val) != NGX_OK)
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing chat_id parameter", NULL);
    ngx_memcpy(chat_id, val.data, val.len < sizeof(chat_id) ? val.len : sizeof(chat_id)-1);
    chat_id[val.len] = '\0';

    if (ngx_http_arg(r, (u_char *)"message_id", 10, &val) != NGX_OK)
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing message_id parameter", NULL);
    ngx_memcpy(msg_id, val.data, val.len < sizeof(msg_id) ? val.len : sizeof(msg_id)-1);
    msg_id[val.len] = '\0';

    if (!chat_is_valid_chat_id(chat_id) || !chat_is_valid_message_id(msg_id))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Invalid chat_id or message_id format", NULL);

    rdc = get_redis(r);
    if (!rdc) return send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
        "Redis connection failed", NULL);

    messages = chat_redis_get_messages(rdc, r->connection->log, chat_id, 200);
    arts     = chat_redis_get_artifacts(rdc, r->connection->log, chat_id);
    redisFree(rdc);

    /* Find target message */
    if (messages) {
        n = cJSON_GetArraySize(messages);
        for (i = 0; i < n; i++) {
            cJSON *m = cJSON_GetArrayItem(messages, i);
            cJSON *id = cJSON_GetObjectItem(m, "id");
            if (id && id->valuestring && strcmp(id->valuestring, msg_id) == 0) {
                msg = cJSON_Duplicate(m, 1);
                break;
            }
        }
        cJSON_Delete(messages);
    }

    if (!msg) {
        if (arts) cJSON_Delete(arts);
        return send_api_error(r, NGX_HTTP_NOT_FOUND,
            "Message not found", NULL);
    }

    /* Filter artifacts belonging to this message */
    cJSON *msg_arts = cJSON_CreateArray();
    if (arts) {
        n = cJSON_GetArraySize(arts);
        for (i = 0; i < n; i++) {
            cJSON *a = cJSON_GetArrayItem(arts, i);
            cJSON *pid = cJSON_GetObjectItem(a, "parent_id");
            if (pid && pid->valuestring &&
                strcmp(pid->valuestring, msg_id) == 0)
            {
                cJSON_AddItemToArray(msg_arts, cJSON_Duplicate(a, 1));
            }
        }
        cJSON_Delete(arts);
    }

    resp = cJSON_CreateObject();
    cJSON_AddTrueToObject(resp, "success");
    cJSON_AddStringToObject(resp, "chat_id",    chat_id);
    cJSON_AddStringToObject(resp, "message_id", msg_id);
    cJSON_AddItemToObject(resp, "message",   msg);
    cJSON_AddItemToObject(resp, "artifacts", msg_arts);
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/health
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_health(ngx_http_request_t *r)
{
    ngx_http_chat_main_conf_t *mcf;
    char         model_url[256]  = "http://ollama:11434";
    char         model_name[128] = "devstral";
    int          redis_ok, ollama_ok;
    char         ollama_err[512] = "";
    redisContext *rdc;
    cJSON       *resp, *services, *redis_svc, *ollama_svc;
    char        *js;
    ngx_int_t    n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    mcf = chat_get_main_conf(r);
    if (mcf->model_url.len > 0 && mcf->model_url.len < sizeof(model_url)) {
        ngx_memcpy(model_url, mcf->model_url.data, mcf->model_url.len);
        model_url[mcf->model_url.len] = '\0';
    }
    if (mcf->model_name.len > 0 && mcf->model_name.len < sizeof(model_name)) {
        ngx_memcpy(model_name, mcf->model_name.data, mcf->model_name.len);
        model_name[mcf->model_name.len] = '\0';
    }

    /* Check Redis */
    rdc = get_redis(r);
    redis_ok = rdc && chat_redis_ping(rdc, r->connection->log);
    if (rdc) redisFree(rdc);

    /* Check Ollama */
    ollama_ok = chat_ollama_health_check(r->connection->log,
        model_url, model_name, ollama_err, sizeof(ollama_err));

    services   = cJSON_CreateObject();
    redis_svc  = cJSON_CreateObject();
    ollama_svc = cJSON_CreateObject();

    if (redis_ok)  cJSON_AddTrueToObject(redis_svc,  "healthy");
    else           cJSON_AddFalseToObject(redis_svc,  "healthy");
    if (ollama_ok) cJSON_AddTrueToObject(ollama_svc, "healthy");
    else           cJSON_AddFalseToObject(ollama_svc, "healthy");
    if (!ollama_ok && *ollama_err)
        cJSON_AddStringToObject(ollama_svc, "error", ollama_err);

    cJSON_AddItemToObject(services, "redis",  redis_svc);
    cJSON_AddItemToObject(services, "ollama", ollama_svc);

    resp = cJSON_CreateObject();
    if (redis_ok && ollama_ok) cJSON_AddTrueToObject(resp,  "healthy");
    else                       cJSON_AddFalseToObject(resp, "healthy");
    cJSON_AddItemToObject(resp, "services", services);
    cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));
    js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    n_rc = chat_send_json(r,
        (redis_ok && ollama_ok) ? NGX_HTTP_OK : NGX_HTTP_SERVICE_UNAVAILABLE,
        js);
    free(js);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/docs — minimal HTML docs page
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_docs(ngx_http_request_t *r)
{
    static const char html[] =
        "<!DOCTYPE html><html lang=\"en\" data-bs-theme=\"dark\"><head>"
        "<meta charset=\"UTF-8\"><title>API Docs - Internal Chat</title>"
        "<link href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css\" rel=\"stylesheet\">"
        "<style>body{background:#121212;color:#e0e0e0}"
        ".endpoint{background:#1a1a1a;border-left:4px solid #0d6efd;padding:1rem;margin-bottom:1rem}"
        ".method{font-weight:bold;padding:2px 6px;border-radius:4px}"
        ".POST{background:#28a745;color:#fff}.GET{background:#007bff;color:#fff}</style></head>"
        "<body><div class=\"container py-5\">"
        "<h1>Internal Chat API</h1><p class=\"lead\">RESTful API for chat</p>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/create</h4><p>Create chat session</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/stream</h4><p>Stream chat (SSE)</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/continue</h4><p>Continue incomplete response</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method GET\">GET</span> /api/chat/list</h4><p>List all chats</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method GET\">GET</span> /api/chat/history</h4><p>Chat history</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method GET\">GET</span> /api/chat/artifacts</h4><p>Chat artifacts</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method GET\">GET</span> /api/chat/export</h4><p>Export artifacts</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/clear</h4><p>Clear chat</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/delete</h4><p>Delete chat</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method POST\">POST</span> /api/chat/delete-all</h4><p>Delete all chats</p></div>"
        "<div class=\"endpoint\"><h4><span class=\"method GET\">GET</span> /api/health</h4><p>Health check</p></div>"
        "<div class=\"mt-4\"><a href=\"/\" class=\"btn btn-primary\">Back to Chat</a></div>"
        "</div></body></html>";

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;
    return chat_send_html(r, NGX_HTTP_OK, html, sizeof(html) - 1);
}

/* ════════════════════════════════════════════════════════════
 * GET /status
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_status(ngx_http_request_t *r)
{
    char   html[2048];
    size_t pos = 0;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    chat_strbuf_append(html, sizeof(html), &pos,
        "<!DOCTYPE html><html lang=\"en\" data-bs-theme=\"dark\"><head>"
        "<meta charset=\"UTF-8\"><title>Status</title>"
        "<link href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css\" rel=\"stylesheet\">"
        "<style>body{background:#121212;color:#e0e0e0}</style></head>"
        "<body><div class=\"container py-5\"><h1>System Status</h1>"
        "<div class=\"card bg-dark border-secondary p-3\">"
        "<p>nginx C module: Running</p>"
        "<p>Build time: %s %s</p>"
        "<p>Timestamp: %lld</p>"
        "</div>"
        "<div class=\"mt-4\">"
        "<a href=\"/\" class=\"btn btn-primary\">Back to Chat</a>"
        "</div></div></body></html>",
        __DATE__, __TIME__, (long long)time(NULL));

    return chat_send_html(r, NGX_HTTP_OK, html, strlen(html));
}

/* ════════════════════════════════════════════════════════════
 * GET|POST /api/config
 *   GET  → return effective config (Redis + nginx.conf)
 *   POST → update one or more fields in Redis
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_config(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    if (r->method == NGX_HTTP_GET) {
        chat_effective_config_t cfg;
        chat_redis_config_t     redis_cfg;
        redisContext           *rc;
        cJSON                  *resp;
        char                   *js;
        ngx_int_t               n_rc;
        char                    masked_token[32] = "";
        int                     redis_ok = 0;
        int                     redis_has_overrides = 0;

        chat_resolve_config(r, &cfg);

        /* Fetch raw redis cfg to get hf_token and status */
        rc = get_redis(r);
        if (rc) {
            redis_ok = 1;
            memset(&redis_cfg, 0, sizeof(redis_cfg));
            chat_redis_get_config(rc, r->connection->log, &redis_cfg);
            redisFree(rc);
            redis_has_overrides = redis_cfg.found;
            if (redis_cfg.hf_token[0]) {
                size_t tlen = strlen(redis_cfg.hf_token);
                if (tlen > 6)
                    snprintf(masked_token, sizeof(masked_token),
                        "%.3s***%.3s",
                        redis_cfg.hf_token,
                        redis_cfg.hf_token + tlen - 3);
                else
                    strncpy(masked_token, "***", sizeof(masked_token) - 1);
            }
        }

        resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "model_url",          cfg.model_url);
        cJSON_AddStringToObject(resp, "model_name",         cfg.model_name);
        cJSON_AddNumberToObject(resp, "temperature",        cfg.temperature);
        cJSON_AddNumberToObject(resp, "top_p",              cfg.top_p);
        cJSON_AddNumberToObject(resp, "top_k",              (double)cfg.top_k);
        cJSON_AddNumberToObject(resp, "num_ctx",            (double)cfg.num_ctx);
        cJSON_AddNumberToObject(resp, "num_predict",        (double)cfg.num_predict);
        cJSON_AddNumberToObject(resp, "repeat_penalty",     cfg.repeat_penalty);
        cJSON_AddStringToObject(resp, "hf_token_masked",    masked_token);
        cJSON_AddBoolToObject  (resp, "redis_available",    redis_ok);
        cJSON_AddBoolToObject  (resp, "redis_has_overrides",redis_has_overrides);
        cJSON_AddStringToObject(resp, "source",
            redis_has_overrides ? "redis" : "nginx.conf");
        cJSON_AddNumberToObject(resp, "timestamp", (double)time(NULL));

        js   = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        n_rc = chat_send_json(r, NGX_HTTP_OK, js);
        free(js);
        return n_rc;
    }

    if (r->method == NGX_HTTP_POST) {
        ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_config);
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
        return NGX_DONE;
    }

    return NGX_HTTP_NOT_ALLOWED;
}

static void
chat_body_config(ngx_http_request_t *r)
{
    ngx_str_t    body;
    cJSON       *req;
    redisContext *rc;
    int           updated = 0;

    static const char *allowed[] = {
        "model_name", "model_url", "temperature", "top_p", "top_k",
        "num_ctx", "num_predict", "repeat_penalty", "hf_token", NULL
    };

    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "No body", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    req = cJSON_Parse((char *)body.data);
    if (!req) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "Invalid JSON", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    /* Handle reset request */
    cJSON *reset_item = cJSON_GetObjectItem(req, "reset");
    if (reset_item && cJSON_IsTrue(reset_item)) {
        rc = get_redis(r);
        if (rc) {
            chat_redis_clear_config(rc, r->connection->log);
            redisFree(rc);
        }
        cJSON_Delete(req);
        send_api_success(r, NGX_HTTP_OK, NULL, "Config reset to nginx.conf defaults");
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    rc = get_redis(r);
    if (!rc) {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
            "Redis connection failed", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    /* Set each allowed field that is present in the request */
    {
        int i;
        for (i = 0; allowed[i]; i++) {
            cJSON *item = cJSON_GetObjectItem(req, allowed[i]);
            if (!item) continue;

            char val[512] = {0};
            if (cJSON_IsString(item) && item->valuestring)
                strncpy(val, item->valuestring, sizeof(val) - 1);
            else if (cJSON_IsNumber(item))
                snprintf(val, sizeof(val), "%g", item->valuedouble);
            else
                continue;

            if (chat_redis_set_config_field(rc, r->connection->log,
                    allowed[i], val))
            {
                updated++;
            }
        }
    }

    redisFree(rc);
    cJSON_Delete(req);

    {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "fields_updated", (double)updated);
        send_api_success(r, NGX_HTTP_OK, resp, "Config updated");
        cJSON_Delete(resp);
    }
    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * GET /api/models  — list installed Ollama models
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_models_list(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    chat_effective_config_t  cfg;
    char                    *json;
    ngx_int_t                n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    chat_resolve_config(r, &cfg);
    json = chat_models_list_json(cfg.model_url, r->connection->log);

    if (!json)
        return send_api_error(r, NGX_HTTP_BAD_GATEWAY,
            "Cannot reach Ollama", NULL);

    /* json is already the full Ollama /api/tags JSON — pass straight through */
    n_rc = chat_send_json(r, NGX_HTTP_OK, json);
    free(json);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * POST /api/models/delete
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_models_delete(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_models_delete);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_models_delete(ngx_http_request_t *r)
{
    ngx_str_t                body;
    cJSON                   *req;
    chat_effective_config_t  cfg;
    const char              *model_name;
    char                     err[512] = "";

    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "No body", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    req = cJSON_Parse((char *)body.data);
    if (!req) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "Invalid JSON", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    {
        cJSON *mn = cJSON_GetObjectItem(req, "model");
        model_name = (mn && mn->valuestring && *mn->valuestring)
            ? mn->valuestring : NULL;
    }

    if (!model_name) {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing 'model' field", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    chat_resolve_config(r, &cfg);

    if (!chat_models_delete_model(cfg.model_url, model_name,
            err, sizeof(err), r->connection->log))
    {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_BAD_GATEWAY,
            "Failed to delete model", err[0] ? err : NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    cJSON_Delete(req);
    send_api_success(r, NGX_HTTP_OK, NULL, "Model deleted");
    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * POST /api/models/pull  — SSE streaming pull from Ollama/HF
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_models_pull(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;

    if (!chat_sse_validate_client(r))
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "SSE not supported", NULL);

    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_models_pull);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_models_pull(ngx_http_request_t *r)
{
    ngx_str_t                body;
    cJSON                   *req;
    chat_effective_config_t  cfg;
    const char              *model_name;
    int                      ok;

    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "No body", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    req = cJSON_Parse((char *)body.data);
    if (!req) {
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Invalid JSON", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    {
        cJSON *mn = cJSON_GetObjectItem(req, "model");
        model_name = (mn && mn->valuestring && *mn->valuestring)
            ? mn->valuestring : NULL;
    }

    if (!model_name) {
        cJSON_Delete(req);
        chat_sse_setup_headers(r);
        r->headers_out.status = NGX_HTTP_OK;
        ngx_http_send_header(r);
        chat_sse_send_error(r, "Missing 'model' field", NULL);
        chat_sse_send_done(r, 0, "error");
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    chat_resolve_config(r, &cfg);

    /* Open SSE stream and start pull */
    chat_sse_setup_headers(r);
    r->headers_out.status = NGX_HTTP_OK;
    ngx_http_send_header(r);

    chat_sse_send_status(r, "starting",
        "Connecting to Ollama for model download...");

    ok = chat_models_pull_stream(r, cfg.model_url, model_name,
        r->connection->log);

    cJSON_Delete(req);

    if (ok) {
        cJSON *done_data = cJSON_CreateObject();
        cJSON_AddTrueToObject(done_data,  "success");
        cJSON_AddStringToObject(done_data, "model", model_name);
        char *js = cJSON_PrintUnformatted(done_data);
        cJSON_Delete(done_data);
        if (js) {
            chat_sse_send_raw(r, "pull_done", js, 0);
            free(js);
        }
    } else {
        chat_sse_send_error(r, "Pull failed or incomplete", NULL);
    }

    chat_sse_send_done(r, ok, ok ? "success" : "error");
    ngx_http_finalize_request(r, NGX_OK);
}

/* ════════════════════════════════════════════════════════════
 * GET /api/hf/search?q=<query>
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_hf_search(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    ngx_str_t                q_val;
    char                     query[256] = "";
    chat_effective_config_t  cfg;
    char                    *json;
    ngx_int_t                n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    if (ngx_http_arg(r, (u_char *)"q", 1, &q_val) == NGX_OK &&
        q_val.len > 0 && q_val.len < sizeof(query))
    {
        ngx_memcpy(query, q_val.data, q_val.len);
        query[q_val.len] = '\0';
    }

    if (!query[0])
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing query parameter 'q'", NULL);

    chat_resolve_config(r, &cfg);

    json = chat_hf_search_json(query, 20,
        cfg.hf_token[0] ? cfg.hf_token : NULL,
        r->connection->log);

    if (!json)
        return send_api_error(r, NGX_HTTP_BAD_GATEWAY,
            "HuggingFace API request failed", NULL);

    n_rc = chat_send_json(r, NGX_HTTP_OK, json);
    free(json);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * GET /api/hf/files?id=<model_id>
 * ════════════════════════════════════════════════════════════ */

static ngx_int_t
chat_handler_hf_files(ngx_http_request_t *r)
{
    if (!chat_check_admin_auth(r))
        return send_api_error(r, NGX_HTTP_FORBIDDEN, "Admin access required", NULL);
    ngx_str_t                id_val;
    char                     model_id[256] = "";
    chat_effective_config_t  cfg;
    char                    *json;
    ngx_int_t                n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    if (ngx_http_arg(r, (u_char *)"id", 2, &id_val) == NGX_OK &&
        id_val.len > 0 && id_val.len < sizeof(model_id))
    {
        ngx_memcpy(model_id, id_val.data, id_val.len);
        model_id[id_val.len] = '\0';
    }

    if (!model_id[0])
        return send_api_error(r, NGX_HTTP_BAD_REQUEST,
            "Missing parameter 'id'", NULL);

    chat_resolve_config(r, &cfg);

    json = chat_hf_files_json(model_id,
        cfg.hf_token[0] ? cfg.hf_token : NULL,
        r->connection->log);

    if (!json)
        return send_api_error(r, NGX_HTTP_BAD_GATEWAY,
            "Failed to fetch model files from HuggingFace", NULL);

    n_rc = chat_send_json(r, NGX_HTTP_OK, json);
    free(json);
    return n_rc;
}

/* ════════════════════════════════════════════════════════════
 * Auth handlers
 * ════════════════════════════════════════════════════════════ */

/* GET /api/auth/status — returns current auth state, assigns slot if needed */
static ngx_int_t
chat_handler_auth_status(ngx_http_request_t *r)
{
    redisContext    *rc;
    chat_auth_info_t info;
    cJSON           *resp;
    char            *js;
    ngx_int_t        n_rc;

    if (r->method != NGX_HTTP_GET) return NGX_HTTP_NOT_ALLOWED;

    rc = get_redis(r);

    /* Admin check first */
    if (rc && chat_auth_is_admin(r, rc, r->connection->log)) {
        redisFree(rc);
        resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "admin");
        cJSON_AddStringToObject(resp, "uid",  "admin");
        cJSON_AddNumberToObject(resp, "slot", 0);
        cJSON_AddNumberToObject(resp, "total_users", 0);
        js   = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        n_rc = chat_send_json(r, NGX_HTTP_OK, js);
        free(js);
        return n_rc;
    }

    chat_auth_identify(r, rc, &info);
    if (rc) redisFree(rc);

    resp = cJSON_CreateObject();

    switch (info.type) {
        case CHAT_AUTH_ADMIN:
            cJSON_AddStringToObject(resp, "type", "admin");
            break;
        case CHAT_AUTH_USER:
        case CHAT_AUTH_NEW:
            cJSON_AddStringToObject(resp, "type", "user");
            break;
        case CHAT_AUTH_FULL:
            cJSON_AddStringToObject(resp, "type", "full");
            break;
        default:
            cJSON_AddStringToObject(resp, "type", "unknown");
    }

    cJSON_AddStringToObject(resp, "uid",         info.uid);
    cJSON_AddNumberToObject(resp, "slot",        (double)info.slot);
    cJSON_AddNumberToObject(resp, "total_users", (double)info.total_users);
    cJSON_AddNumberToObject(resp, "max_users",   (double)CHAT_AUTH_MAX_USERS);
    cJSON_AddBoolToObject  (resp, "is_new",      info.type == CHAT_AUTH_NEW);

    js   = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (info.type == CHAT_AUTH_FULL) {
        n_rc = chat_send_json(r, 503, js);
    } else {
        n_rc = chat_send_json(r, NGX_HTTP_OK, js);
    }
    free(js);
    return n_rc;
}

/* POST /api/auth/login — admin password login */
static ngx_int_t
chat_handler_auth_login(ngx_http_request_t *r)
{
    if (r->method != NGX_HTTP_POST) return NGX_HTTP_NOT_ALLOWED;
    ngx_int_t rc = ngx_http_read_client_request_body(r, chat_body_auth_login);
    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) return rc;
    return NGX_DONE;
}

static void
chat_body_auth_login(ngx_http_request_t *r)
{
    ngx_str_t                   body;
    cJSON                      *req;
    const char                 *provided_pw;
    ngx_http_chat_main_conf_t  *mcf;
    redisContext               *rc;

    if (chat_collect_body(r, &body) != NGX_OK || body.len == 0) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "No body", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    req = cJSON_Parse((char *)body.data);
    if (!req) {
        send_api_error(r, NGX_HTTP_BAD_REQUEST, "Invalid JSON", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    {
        cJSON *pw = cJSON_GetObjectItem(req, "password");
        provided_pw = (pw && pw->valuestring) ? pw->valuestring : "";
    }

    mcf = chat_get_main_conf(r);
    if (!mcf || mcf->admin_password.len == 0) {
        cJSON_Delete(req);
        send_api_error(r, NGX_HTTP_INTERNAL_SERVER_ERROR,
            "Admin password not configured in nginx.conf", NULL);
        ngx_http_finalize_request(r, NGX_OK); return;
    }

    /* NUL-terminate the ngx_str_t */
    char expected[256] = {0};
    if (mcf->admin_password.len < sizeof(expected))
        ngx_memcpy(expected, mcf->admin_password.data, mcf->admin_password.len);

    rc = get_redis(r);
    int ok = chat_auth_admin_login(r, provided_pw, expected,
        rc, r->connection->log);
    if (rc) redisFree(rc);
    cJSON_Delete(req);

    if (ok) {
        send_api_success(r, NGX_HTTP_OK, NULL, "Login successful");
    } else {
        send_api_error(r, 401, "Invalid password", NULL);
    }
    ngx_http_finalize_request(r, NGX_OK);
}

/* POST /api/auth/logout */
static ngx_int_t
chat_handler_auth_logout(ngx_http_request_t *r)
{
    redisContext *rc = get_redis(r);
    chat_auth_admin_logout(r, rc, r->connection->log);
    if (rc) redisFree(rc);
    send_api_success(r, NGX_HTTP_OK, NULL, "Logged out");
    return NGX_OK;
}

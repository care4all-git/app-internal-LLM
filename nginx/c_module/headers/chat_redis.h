#ifndef CHAT_REDIS_H
#define CHAT_REDIS_H

#include <hiredis/hiredis.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "cJSON.h"

/* ── Key constants ──────────────────────────────────────────── */

#define CHAT_USER_ID  "jai"
#define CHAT_KEY_TTL  (7 * 24 * 3600)   /* 7 days in seconds */

/* ── Connection ─────────────────────────────────────────────── */

/* Open a Redis connection with a 1-second timeout.
   Returns NULL on failure (error already logged). */
redisContext *chat_redis_connect(const char *host, int port, ngx_log_t *log);

/* ── Counters ────────────────────────────────────────────────── */

/* Increment the per-chat, per-role message counter.
   Returns the new counter value, or -1 on error. */
long long chat_redis_incr_counter(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *role_prefix);

/* ── Message operations ─────────────────────────────────────── */

/* Persist one message. files_json and artifacts_json are JSON arrays as strings. */
int chat_redis_save_message(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *message_id,
    const char *role, const char *content,
    const char *files_json, const char *artifacts_json);

/* Return up to `limit` messages as a cJSON array, chronological order.
   Caller must cJSON_Delete the result. */
cJSON *chat_redis_get_messages(redisContext *c, ngx_log_t *log,
    const char *chat_id, int limit);

/* Return the last `limit` messages as a cJSON array of {role, content} pairs
   suitable for passing to Ollama as the conversation context.
   Caller must cJSON_Delete the result. */
cJSON *chat_redis_get_context(redisContext *c, ngx_log_t *log,
    const char *chat_id, int limit);

/* ── Artifact operations ─────────────────────────────────────── */

/* Persist one code-block artifact extracted from an AI response. */
int chat_redis_save_artifact(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *artifact_id,
    const char *parent_message_id,
    const char *code, const char *language,
    const char *metadata_json);

/* Return all artifacts for a chat as a cJSON array.
   Caller must cJSON_Delete the result. */
cJSON *chat_redis_get_artifacts(redisContext *c, ngx_log_t *log,
    const char *chat_id);

/* ── Chat management ─────────────────────────────────────────── */

/* Create (or re-initialise) the metadata entry for a chat. */
int chat_redis_create_chat(redisContext *c, ngx_log_t *log,
    const char *chat_id);

/* Return all chats as a cJSON array, sorted newest-first.
   Caller must cJSON_Delete the result. */
cJSON *chat_redis_get_chat_list(redisContext *c, ngx_log_t *log);

/* Delete all messages and artifacts for one chat.
   Returns the number of Redis keys deleted. */
long long chat_redis_clear_chat(redisContext *c, ngx_log_t *log,
    const char *chat_id);

/* Delete every chat key owned by CHAT_USER_ID.
   Returns the number of Redis keys deleted. */
long long chat_redis_delete_all(redisContext *c, ngx_log_t *log);

/* ── Health ──────────────────────────────────────────────────── */

/* Send PING and verify PONG. Returns 1 on success. */
int chat_redis_ping(redisContext *c, ngx_log_t *log);

/* ── Runtime config (Redis hash chat:config) ─────────────────── */

#define CHAT_CONFIG_KEY "chat:config"

typedef struct {
    char model_name[128];
    char model_url[256];
    char temperature[32];
    char top_p[32];
    char top_k[32];
    char num_ctx[32];
    char num_predict[32];
    char repeat_penalty[32];
    char hf_token[256];
    int  found;   /* 1 if the Redis key existed (any fields set) */
} chat_redis_config_t;

/* Read all config fields from the Redis hash into *out. */
void chat_redis_get_config(redisContext *c, ngx_log_t *log,
    chat_redis_config_t *out);

/* Set a single config field by name. Returns 1 on success. */
int chat_redis_set_config_field(redisContext *c, ngx_log_t *log,
    const char *field, const char *value);

/* Delete the entire config key (resets to nginx.conf defaults). */
int chat_redis_clear_config(redisContext *c, ngx_log_t *log);

#endif /* CHAT_REDIS_H */

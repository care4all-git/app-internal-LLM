#include "../headers/chat_redis.h"
#include "../headers/chat_utils.h"
#include <string.h>
#include <time.h>

#define USER_ID CHAT_USER_ID
#define TTL     CHAT_KEY_TTL

/* ── Internal helper ────────────────────────────────────── */

static int
check_reply(redisReply *r, ngx_log_t *log, const char *ctx)
{
    if (!r) {
        chat_log_error(log, "redis", ctx, "null reply (connection lost?)");
        return 0;
    }
    if (r->type == REDIS_REPLY_ERROR) {
        chat_log_error(log, "redis", ctx, r->str);
        freeReplyObject(r);
        return 0;
    }
    return 1;
}

/* ── Connection ─────────────────────────────────────────── */

redisContext *
chat_redis_connect(const char *host, int port, ngx_log_t *log)
{
    redisContext *c;
    struct timeval timeout = {1, 0};  /* 1 second */

    c = redisConnectWithTimeout(host, port, timeout);
    if (!c || c->err) {
        chat_log_error(log, "redis", "connect",
            c ? c->errstr : "allocation failure");
        if (c) redisFree(c);
        return NULL;
    }
    return c;
}

/* ── Counter ─────────────────────────────────────────────── */

long long
chat_redis_incr_counter(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *role_prefix)
{
    char          key[256];
    redisReply   *r;
    long long     val;

    snprintf(key, sizeof(key),
        "chat:counter:%s:%s:%s", USER_ID, chat_id, role_prefix);

    r = redisCommand(c, "INCR %s", key);
    if (!check_reply(r, log, "incr_counter")) return -1;
    val = r->integer;
    freeReplyObject(r);

    r = redisCommand(c, "EXPIRE %s %d", key, TTL);
    if (r) freeReplyObject(r);

    return val;
}

/* ── Message operations ─────────────────────────────────── */

int
chat_redis_save_message(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *message_id,
    const char *role, const char *content,
    const char *files_json, const char *artifacts_json)
{
    cJSON       *msg;
    char        *json_str;
    char         key[256];
    char         list_key[256];
    char         meta_key[256];
    redisReply  *r;
    long long    msg_count;
    char         preview[101];
    cJSON       *meta;
    char        *meta_str;
    long long    ts;

    ts = (long long)time(NULL);

    /* Build message JSON */
    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "id",       message_id);
    cJSON_AddStringToObject(msg, "role",     role);
    cJSON_AddStringToObject(msg, "content",  content ? content : "");
    cJSON_AddStringToObject(msg, "chat_id",  chat_id);
    cJSON_AddNumberToObject(msg, "timestamp", (double)ts);

    /* Embed files and artifacts arrays */
    {
        cJSON *fa = files_json ? cJSON_Parse(files_json) : cJSON_CreateArray();
        cJSON *aa = artifacts_json ? cJSON_Parse(artifacts_json) : cJSON_CreateArray();
        cJSON_AddItemToObject(msg, "files",     fa ? fa : cJSON_CreateArray());
        cJSON_AddItemToObject(msg, "artifacts", aa ? aa : cJSON_CreateArray());
    }

    json_str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!json_str) return 0;

    /* Save individual message */
    snprintf(key, sizeof(key),
        "message:%s:%s:%s", USER_ID, chat_id, message_id);
    r = redisCommand(c, "SET %s %s", key, json_str);
    free(json_str);
    if (!check_reply(r, log, "save_message:SET")) return 0;
    freeReplyObject(r);
    r = redisCommand(c, "EXPIRE %s %d", key, TTL);
    if (r) freeReplyObject(r);

    /* Prepend to chat message list */
    snprintf(list_key, sizeof(list_key),
        "chat:messages:%s:%s", USER_ID, chat_id);
    r = redisCommand(c, "LPUSH %s %s", list_key, message_id);
    if (!check_reply(r, log, "save_message:LPUSH")) return 0;
    msg_count = r->integer;
    freeReplyObject(r);
    r = redisCommand(c, "EXPIRE %s %d", list_key, TTL);
    if (r) freeReplyObject(r);

    /* Update chat metadata */
    snprintf(meta_key, sizeof(meta_key),
        "chat:meta:%s:%s", USER_ID, chat_id);
    strncpy(preview, content ? content : "", 100);
    preview[100] = '\0';

    meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "id",                   chat_id);
    cJSON_AddNumberToObject(meta, "last_updated",         (double)ts);
    cJSON_AddNumberToObject(meta, "message_count",        (double)msg_count);
    cJSON_AddStringToObject(meta, "last_message_preview", preview);
    meta_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (meta_str) {
        r = redisCommand(c, "SET %s %s", meta_key, meta_str);
        free(meta_str);
        if (r) freeReplyObject(r);
        r = redisCommand(c, "EXPIRE %s %d", meta_key, TTL);
        if (r) freeReplyObject(r);
    }

    return 1;
}

cJSON *
chat_redis_get_messages(redisContext *c, ngx_log_t *log,
    const char *chat_id, int limit)
{
    char         list_key[256];
    redisReply  *ids;
    cJSON       *result;
    long         i;

    snprintf(list_key, sizeof(list_key),
        "chat:messages:%s:%s", USER_ID, chat_id);

    ids = redisCommand(c, "LRANGE %s 0 %d", list_key, limit - 1);
    if (!check_reply(ids, log, "get_messages:LRANGE")) return NULL;

    result = cJSON_CreateArray();

    /* IDs are stored newest-first (LPUSH); reverse to get chronological */
    for (i = ids->elements - 1; i >= 0; i--) {
        char         msg_key[256];
        redisReply  *val;
        cJSON       *obj;

        snprintf(msg_key, sizeof(msg_key),
            "message:%s:%s:%s", USER_ID, chat_id, ids->element[i]->str);
        val = redisCommand(c, "GET %s", msg_key);
        if (!val || val->type != REDIS_REPLY_STRING) {
            if (val) freeReplyObject(val);
            continue;
        }
        obj = cJSON_Parse(val->str);
        freeReplyObject(val);
        if (obj) cJSON_AddItemToArray(result, obj);
    }

    freeReplyObject(ids);
    return result;
}

cJSON *
chat_redis_get_context(redisContext *c, ngx_log_t *log,
    const char *chat_id, int limit)
{
    cJSON       *messages;
    cJSON       *context;
    cJSON       *msg;
    int          n;

    messages = chat_redis_get_messages(c, log, chat_id, limit);
    if (!messages) return NULL;

    context = cJSON_CreateArray();
    n = cJSON_GetArraySize(messages);

    for (int i = 0; i < n; i++) {
        cJSON *entry, *role_item, *content_item;
        msg = cJSON_GetArrayItem(messages, i);
        role_item    = cJSON_GetObjectItem(msg, "role");
        content_item = cJSON_GetObjectItem(msg, "content");
        if (!role_item || !content_item) continue;

        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role",
            strcmp(role_item->valuestring, "user") == 0 ? "user" : "assistant");
        cJSON_AddStringToObject(entry, "content",
            content_item->valuestring ? content_item->valuestring : "");
        cJSON_AddItemToArray(context, entry);
    }

    cJSON_Delete(messages);
    return context;
}

/* ── Artifact operations ─────────────────────────────────── */

int
chat_redis_save_artifact(redisContext *c, ngx_log_t *log,
    const char *chat_id, const char *artifact_id,
    const char *parent_message_id,
    const char *code, const char *language,
    const char *metadata_json)
{
    cJSON       *art;
    char        *json_str;
    char         key[256];
    char         list_key[256];
    redisReply  *r;
    long long    ts = (long long)time(NULL);

    art = cJSON_CreateObject();
    cJSON_AddStringToObject(art, "id",        artifact_id);
    cJSON_AddStringToObject(art, "parent_id", parent_message_id ? parent_message_id : "");
    cJSON_AddStringToObject(art, "type",      "code_block");
    cJSON_AddStringToObject(art, "code",      code ? code : "");
    cJSON_AddStringToObject(art, "language",  language ? language : "");
    cJSON_AddStringToObject(art, "chat_id",   chat_id);
    cJSON_AddNumberToObject(art, "timestamp", (double)ts);

    if (metadata_json) {
        cJSON *meta = cJSON_Parse(metadata_json);
        if (meta) cJSON_AddItemToObject(art, "metadata", meta);
    }

    json_str = cJSON_PrintUnformatted(art);
    cJSON_Delete(art);
    if (!json_str) return 0;

    snprintf(key, sizeof(key),
        "artifact:%s:%s:%s", USER_ID, chat_id, artifact_id);
    r = redisCommand(c, "SET %s %s", key, json_str);
    free(json_str);
    if (!check_reply(r, log, "save_artifact:SET")) return 0;
    freeReplyObject(r);
    r = redisCommand(c, "EXPIRE %s %d", key, TTL);
    if (r) freeReplyObject(r);

    snprintf(list_key, sizeof(list_key),
        "chat:artifacts:%s:%s", USER_ID, chat_id);
    r = redisCommand(c, "LPUSH %s %s", list_key, artifact_id);
    if (r) freeReplyObject(r);
    r = redisCommand(c, "EXPIRE %s %d", list_key, TTL);
    if (r) freeReplyObject(r);

    return 1;
}

cJSON *
chat_redis_get_artifacts(redisContext *c, ngx_log_t *log,
    const char *chat_id)
{
    char         list_key[256];
    redisReply  *ids;
    cJSON       *result;
    size_t       i;

    snprintf(list_key, sizeof(list_key),
        "chat:artifacts:%s:%s", USER_ID, chat_id);

    ids = redisCommand(c, "LRANGE %s 0 -1", list_key);
    if (!check_reply(ids, log, "get_artifacts:LRANGE")) return NULL;

    result = cJSON_CreateArray();
    for (i = 0; i < ids->elements; i++) {
        char         art_key[256];
        redisReply  *val;
        cJSON       *obj;

        snprintf(art_key, sizeof(art_key),
            "artifact:%s:%s:%s", USER_ID, chat_id, ids->element[i]->str);
        val = redisCommand(c, "GET %s", art_key);
        if (!val || val->type != REDIS_REPLY_STRING) {
            if (val) freeReplyObject(val);
            continue;
        }
        obj = cJSON_Parse(val->str);
        freeReplyObject(val);
        if (obj) cJSON_AddItemToArray(result, obj);
    }

    freeReplyObject(ids);
    return result;
}

/* ── Chat management ─────────────────────────────────────── */

int
chat_redis_create_chat(redisContext *c, ngx_log_t *log,
    const char *chat_id)
{
    char         meta_key[256];
    cJSON       *meta;
    char        *json_str;
    redisReply  *r;
    long long    ts = (long long)time(NULL);

    snprintf(meta_key, sizeof(meta_key),
        "chat:meta:%s:%s", USER_ID, chat_id);

    meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "id",                   chat_id);
    cJSON_AddNumberToObject(meta, "last_updated",         (double)ts);
    cJSON_AddNumberToObject(meta, "message_count",        0.0);
    cJSON_AddStringToObject(meta, "last_message_preview", "");
    json_str = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    if (!json_str) return 0;

    r = redisCommand(c, "SET %s %s", meta_key, json_str);
    free(json_str);
    if (!check_reply(r, log, "create_chat:SET")) return 0;
    freeReplyObject(r);
    r = redisCommand(c, "EXPIRE %s %d", meta_key, TTL);
    if (r) freeReplyObject(r);

    return 1;
}

cJSON *
chat_redis_get_chat_list(redisContext *c, ngx_log_t *log)
{
    char        pattern[128];
    redisReply *keys;
    cJSON      *result;
    size_t      i;

    snprintf(pattern, sizeof(pattern), "chat:meta:%s:*", USER_ID);
    keys = redisCommand(c, "KEYS %s", pattern);
    if (!check_reply(keys, log, "get_chat_list:KEYS")) return NULL;

    result = cJSON_CreateArray();
    for (i = 0; i < keys->elements; i++) {
        redisReply *val;
        cJSON      *meta, *entry;
        cJSON      *id_item, *mc_item, *lu_item, *prev_item;

        val = redisCommand(c, "GET %s", keys->element[i]->str);
        if (!val || val->type != REDIS_REPLY_STRING) {
            if (val) freeReplyObject(val);
            continue;
        }
        meta = cJSON_Parse(val->str);
        freeReplyObject(val);
        if (!meta) continue;

        id_item   = cJSON_GetObjectItem(meta, "id");
        mc_item   = cJSON_GetObjectItem(meta, "message_count");
        lu_item   = cJSON_GetObjectItem(meta, "last_updated");
        prev_item = cJSON_GetObjectItem(meta, "last_message_preview");

        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id",
            id_item && id_item->valuestring ? id_item->valuestring : "");
        cJSON_AddNumberToObject(entry, "message_count",
            mc_item ? mc_item->valuedouble : 0);
        cJSON_AddNumberToObject(entry, "last_updated",
            lu_item ? lu_item->valuedouble : 0);
        cJSON_AddStringToObject(entry, "preview",
            prev_item && prev_item->valuestring ? prev_item->valuestring : "");
        cJSON_AddItemToArray(result, entry);

        cJSON_Delete(meta);
    }

    freeReplyObject(keys);

    /* Sort newest first by last_updated via a simple pointer-swap on a temp array */
    {
        int n = cJSON_GetArraySize(result);
        if (n > 1) {
            /* Collect pointers */
            cJSON **items = malloc((size_t)n * sizeof(cJSON *));
            if (items) {
                int i, j;
                for (i = 0; i < n; i++)
                    items[i] = cJSON_GetArrayItem(result, i);
                /* Bubble sort by last_updated descending */
                for (i = 0; i < n - 1; i++) {
                    for (j = 0; j < n - 1 - i; j++) {
                        cJSON *a_lu = cJSON_GetObjectItem(items[j],   "last_updated");
                        cJSON *b_lu = cJSON_GetObjectItem(items[j+1], "last_updated");
                        double av = a_lu ? a_lu->valuedouble : 0;
                        double bv = b_lu ? b_lu->valuedouble : 0;
                        if (av < bv) {
                            cJSON *tmp = items[j];
                            items[j]   = items[j+1];
                            items[j+1] = tmp;
                        }
                    }
                }
                /* Rebuild result array in sorted order */
                cJSON *sorted = cJSON_CreateArray();
                for (i = 0; i < n; i++)
                    cJSON_AddItemToArray(sorted, cJSON_Duplicate(items[i], 1));
                free(items);
                /* Replace result contents */
                while (cJSON_GetArraySize(result) > 0)
                    cJSON_DeleteItemFromArray(result, 0);
                cJSON *child = sorted->child;
                while (child) {
                    cJSON *next = child->next;
                    cJSON_AddItemToArray(result, cJSON_Duplicate(child, 1));
                    child = next;
                }
                cJSON_Delete(sorted);
            }
        }
    }

    return result;
}

long long
chat_redis_clear_chat(redisContext *c, ngx_log_t *log,
    const char *chat_id)
{
    char         list_key[256];
    char         art_list_key[256];
    char         meta_key[256];
    char         cnt_admin_key[256];
    char         cnt_jai_key[256];
    redisReply  *ids;
    long long    deleted = 0;
    size_t       i;

    snprintf(list_key,      sizeof(list_key),
        "chat:messages:%s:%s",  USER_ID, chat_id);
    snprintf(art_list_key,  sizeof(art_list_key),
        "chat:artifacts:%s:%s", USER_ID, chat_id);
    snprintf(meta_key,      sizeof(meta_key),
        "chat:meta:%s:%s",      USER_ID, chat_id);
    snprintf(cnt_admin_key, sizeof(cnt_admin_key),
        "chat:counter:%s:%s:admin", USER_ID, chat_id);
    snprintf(cnt_jai_key,   sizeof(cnt_jai_key),
        "chat:counter:%s:%s:jai",   USER_ID, chat_id);

    /* Delete individual messages */
    ids = redisCommand(c, "LRANGE %s 0 -1", list_key);
    if (ids && ids->type == REDIS_REPLY_ARRAY) {
        for (i = 0; i < ids->elements; i++) {
            char mkey[256];
            redisReply *dr;
            snprintf(mkey, sizeof(mkey),
                "message:%s:%s:%s", USER_ID, chat_id, ids->element[i]->str);
            dr = redisCommand(c, "DEL %s", mkey);
            if (dr) { deleted += dr->integer; freeReplyObject(dr); }
        }
        freeReplyObject(ids);
    }

    /* Delete individual artifacts */
    ids = redisCommand(c, "LRANGE %s 0 -1", art_list_key);
    if (ids && ids->type == REDIS_REPLY_ARRAY) {
        for (i = 0; i < ids->elements; i++) {
            char akey[256];
            redisReply *dr;
            snprintf(akey, sizeof(akey),
                "artifact:%s:%s:%s", USER_ID, chat_id, ids->element[i]->str);
            dr = redisCommand(c, "DEL %s", akey);
            if (dr) { deleted += dr->integer; freeReplyObject(dr); }
        }
        freeReplyObject(ids);
    }

    /* Delete list and meta keys */
    {
        const char *keys[] = {list_key, art_list_key, meta_key,
                               cnt_admin_key, cnt_jai_key};
        for (i = 0; i < 5; i++) {
            redisReply *dr = redisCommand(c, "DEL %s", keys[i]);
            if (dr) { deleted += dr->integer; freeReplyObject(dr); }
        }
    }

    return deleted;
}

long long
chat_redis_delete_all(redisContext *c, ngx_log_t *log)
{
    static const char *patterns[] = {
        "message:%s:*", "artifact:%s:*",
        "chat:messages:%s:*", "chat:artifacts:%s:*",
        "chat:meta:%s:*", "chat:counter:%s:*",
        NULL
    };
    long long    deleted = 0;
    int          pi;

    for (pi = 0; patterns[pi]; pi++) {
        char         pattern[128];
        redisReply  *keys;
        size_t       i;

        snprintf(pattern, sizeof(pattern), patterns[pi], USER_ID);
        keys = redisCommand(c, "KEYS %s", pattern);
        if (!keys || keys->type != REDIS_REPLY_ARRAY) {
            if (keys) freeReplyObject(keys);
            continue;
        }
        for (i = 0; i < keys->elements; i++) {
            redisReply *dr = redisCommand(c, "DEL %s", keys->element[i]->str);
            if (dr) { deleted += dr->integer; freeReplyObject(dr); }
        }
        freeReplyObject(keys);
    }
    return deleted;
}

int
chat_redis_ping(redisContext *c, ngx_log_t *log)
{
    redisReply *r = redisCommand(c, "PING");
    if (!r) return 0;
    int ok = (r->type == REDIS_REPLY_STATUS &&
              strcasecmp(r->str, "PONG") == 0);
    freeReplyObject(r);
    return ok;
}

/* ── Runtime config ──────────────────────────────────────────── */

void
chat_redis_get_config(redisContext *c, ngx_log_t *log,
    chat_redis_config_t *out)
{
    redisReply *r;
    size_t      i;

    memset(out, 0, sizeof(*out));

    r = redisCommand(c, "HGETALL %s", CHAT_CONFIG_KEY);
    if (!r || r->type != REDIS_REPLY_ARRAY) {
        if (r) freeReplyObject(r);
        return;
    }

    out->found = (r->elements > 0) ? 1 : 0;

    for (i = 0; i + 1 < r->elements; i += 2) {
        const char *field = r->element[i]->str;
        const char *value = r->element[i + 1]->str;
        if (!field || !value) continue;

        if      (strcmp(field, "model_name")     == 0)
            strncpy(out->model_name,     value, sizeof(out->model_name)     - 1);
        else if (strcmp(field, "model_url")      == 0)
            strncpy(out->model_url,      value, sizeof(out->model_url)      - 1);
        else if (strcmp(field, "temperature")    == 0)
            strncpy(out->temperature,    value, sizeof(out->temperature)    - 1);
        else if (strcmp(field, "top_p")          == 0)
            strncpy(out->top_p,          value, sizeof(out->top_p)          - 1);
        else if (strcmp(field, "top_k")          == 0)
            strncpy(out->top_k,          value, sizeof(out->top_k)          - 1);
        else if (strcmp(field, "num_ctx")        == 0)
            strncpy(out->num_ctx,        value, sizeof(out->num_ctx)        - 1);
        else if (strcmp(field, "num_predict")    == 0)
            strncpy(out->num_predict,    value, sizeof(out->num_predict)    - 1);
        else if (strcmp(field, "repeat_penalty") == 0)
            strncpy(out->repeat_penalty, value, sizeof(out->repeat_penalty) - 1);
        else if (strcmp(field, "hf_token")       == 0)
            strncpy(out->hf_token,       value, sizeof(out->hf_token)       - 1);
    }

    freeReplyObject(r);
}

int
chat_redis_set_config_field(redisContext *c, ngx_log_t *log,
    const char *field, const char *value)
{
    redisReply *r = redisCommand(c, "HSET %s %s %s",
        CHAT_CONFIG_KEY, field, value);
    if (!check_reply(r, log, "set_config_field")) return 0;
    freeReplyObject(r);
    return 1;
}

int
chat_redis_clear_config(redisContext *c, ngx_log_t *log)
{
    redisReply *r = redisCommand(c, "DEL %s", CHAT_CONFIG_KEY);
    if (!r) {
        chat_log_error(log, "redis", "clear_config", "null reply");
        return 0;
    }
    freeReplyObject(r);
    return 1;
}

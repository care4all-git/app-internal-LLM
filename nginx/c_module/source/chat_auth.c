#include "../headers/chat_auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Cookie read ─────────────────────────────────────────────── */

ngx_str_t
chat_auth_get_cookie(ngx_http_request_t *r, const char *name)
{
    ngx_str_t        result = ngx_null_string;
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t       i;
    size_t           name_len = strlen(name);

    part = &r->headers_in.headers.part;
    h    = part->elts;

    for (i = 0; ; i++) {
        if (i >= part->nelts) {
            if (!part->next) break;
            part = part->next;
            h    = part->elts;
            i    = 0;
        }
        if (h[i].key.len != 6 ||
            ngx_strncasecmp(h[i].key.data, (u_char *)"cookie", 6) != 0)
        {
            continue;
        }

        /* Parse "name1=val1; name2=val2" */
        u_char *p   = h[i].value.data;
        u_char *end = p + h[i].value.len;

        while (p < end) {
            while (p < end && *p == ' ') p++;
            u_char *ns = p;
            while (p < end && *p != '=' && *p != ';') p++;
            size_t nlen = (size_t)(p - ns);
            if (p < end && *p == '=') {
                u_char *vs = ++p;
                while (p < end && *p != ';') p++;
                if (nlen == name_len &&
                    ngx_strncmp(ns, (u_char *)name, name_len) == 0)
                {
                    result.data = vs;
                    result.len  = (size_t)(p - vs);
                    return result;
                }
            }
            if (p < end && *p == ';') p++;
        }
    }
    return result;
}

/* ── Cookie write ────────────────────────────────────────────── */

void
chat_auth_set_cookie(ngx_http_request_t *r,
    const char *name, const char *value, int max_age, int http_only)
{
    char            buf[640];
    ngx_table_elt_t *h;
    size_t           len;

    if (max_age > 0) {
        snprintf(buf, sizeof(buf),
            "%s=%s; Path=/; Max-Age=%d; SameSite=Strict%s",
            name, value, max_age, http_only ? "; HttpOnly" : "");
    } else if (max_age == 0) {
        snprintf(buf, sizeof(buf),
            "%s=; Path=/; Max-Age=0; SameSite=Strict%s",
            name, http_only ? "; HttpOnly" : "");
    } else {
        snprintf(buf, sizeof(buf),
            "%s=%s; Path=/; SameSite=Strict%s",
            name, value, http_only ? "; HttpOnly" : "");
    }

    h = ngx_list_push(&r->headers_out.headers);
    if (!h) return;

    len = strlen(buf);
    h->hash = 1;
    ngx_str_set(&h->key, "Set-Cookie");
    h->value.len  = len;
    h->value.data = ngx_pnalloc(r->pool, len + 1);
    if (h->value.data) {
        ngx_memcpy(h->value.data, buf, len + 1);
    }
}

/* ── Token generation ────────────────────────────────────────── */

void
chat_auth_gen_token(char *buf, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char     rnd[64];
    size_t            i;

    memset(rnd, 0, sizeof(rnd));

    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t need = (len + 1) / 2;
        if (need > sizeof(rnd)) need = sizeof(rnd);
        fread(rnd, 1, need, f);
        fclose(f);
    } else {
        /* Weak fallback — acceptable for internal tool */
        srand((unsigned)(time(NULL) ^ (uintptr_t)buf));
        for (i = 0; i < sizeof(rnd); i++)
            rnd[i] = (unsigned char)(rand() & 0xff);
    }

    for (i = 0; i < len; i++) {
        buf[i] = hex[(rnd[i / 2] >> ((i % 2 == 0) ? 4 : 0)) & 0xf];
    }
    buf[len] = '\0';
}

/* ── Cleanup stale user slots ────────────────────────────────── */

void
chat_auth_cleanup_users(redisContext *rc, ngx_log_t *log)
{
    redisReply *r;
    time_t      now = time(NULL);
    size_t      i;

    r = redisCommand(rc, "HGETALL %s", CHAT_AUTH_USERS_KEY);
    if (!r || r->type != REDIS_REPLY_ARRAY) {
        if (r) freeReplyObject(r);
        return;
    }

    for (i = 0; i + 1 < r->elements; i += 2) {
        const char *uid      = r->element[i]->str;
        const char *ts_str   = r->element[i + 1]->str;
        time_t      last_seen = ts_str ? (time_t)atol(ts_str) : 0;

        if (now - last_seen > CHAT_AUTH_USER_TTL) {
            redisReply *del = redisCommand(rc, "HDEL %s %s",
                CHAT_AUTH_USERS_KEY, uid);
            if (del) freeReplyObject(del);
        }
    }

    freeReplyObject(r);
}

/* ── Identify (main auth logic) ──────────────────────────────── */

ngx_int_t
chat_auth_identify(ngx_http_request_t *r,
    redisContext *rc, chat_auth_info_t *info)
{
    ngx_str_t  uid_cookie;
    redisReply *rep;
    time_t      now = time(NULL);
    char        ts_buf[32];
    char        new_uid[40];

    memset(info, 0, sizeof(*info));

    if (!rc) {
        /* No Redis — allow as fallback (single user mode) */
        info->type = CHAT_AUTH_USER;
        strncpy(info->uid, "local", sizeof(info->uid) - 1);
        return NGX_OK;
    }

    uid_cookie = chat_auth_get_cookie(r, "uid");

    /* ── Check existing uid cookie ── */
    if (uid_cookie.len > 0 && uid_cookie.len < sizeof(info->uid)) {
        char uid_str[72] = {0};
        ngx_memcpy(uid_str, uid_cookie.data, uid_cookie.len);

        rep = redisCommand(rc, "HEXISTS %s %s", CHAT_AUTH_USERS_KEY, uid_str);
        if (rep && rep->type == REDIS_REPLY_INTEGER && rep->integer == 1) {
            freeReplyObject(rep);
            /* Update last_seen */
            snprintf(ts_buf, sizeof(ts_buf), "%ld", (long)now);
            rep = redisCommand(rc, "HSET %s %s %s",
                CHAT_AUTH_USERS_KEY, uid_str, ts_buf);
            if (rep) freeReplyObject(rep);

            /* Refresh cookie TTL */
            chat_auth_set_cookie(r, "uid", uid_str,
                CHAT_AUTH_USER_TTL, 1);

            strncpy(info->uid, uid_str, sizeof(info->uid) - 1);
            info->type = CHAT_AUTH_USER;

            /* Count active users */
            rep = redisCommand(rc, "HLEN %s", CHAT_AUTH_USERS_KEY);
            info->total_users = (rep && rep->type == REDIS_REPLY_INTEGER)
                ? (int)rep->integer : 1;
            if (rep) freeReplyObject(rep);

            return NGX_OK;
        }
        if (rep) freeReplyObject(rep);
        /* Cookie was invalid/expired — fall through to create new */
    }

    /* ── Assign a new slot (no capacity limit) ── */
    chat_auth_cleanup_users(rc, r->connection->log);

    rep = redisCommand(rc, "HLEN %s", CHAT_AUTH_USERS_KEY);
    int active = (rep && rep->type == REDIS_REPLY_INTEGER) ? (int)rep->integer : 0;
    if (rep) freeReplyObject(rep);

    /* Generate new uid */
    chat_auth_gen_token(new_uid, 32);
    snprintf(ts_buf, sizeof(ts_buf), "%ld", (long)now);

    rep = redisCommand(rc, "HSET %s %s %s",
        CHAT_AUTH_USERS_KEY, new_uid, ts_buf);
    if (rep) freeReplyObject(rep);

    chat_auth_set_cookie(r, "uid", new_uid, CHAT_AUTH_USER_TTL, 1);

    strncpy(info->uid, new_uid, sizeof(info->uid) - 1);
    info->type        = CHAT_AUTH_NEW;
    info->total_users = active + 1;
    info->slot        = active + 1;

    return NGX_OK;
}

/* ── Admin check ─────────────────────────────────────────────── */

int
chat_auth_is_admin(ngx_http_request_t *r,
    redisContext *rc, ngx_log_t *log)
{
    ngx_str_t  sid_cookie;
    char       key[128];
    redisReply *rep;

    if (!rc) return 0;

    sid_cookie = chat_auth_get_cookie(r, "admin_sid");
    if (sid_cookie.len == 0 || sid_cookie.len >= 64) return 0;

    {
        char sid[64] = {0};
        ngx_memcpy(sid, sid_cookie.data, sid_cookie.len);
        snprintf(key, sizeof(key), "%s%s", CHAT_AUTH_ADMIN_PFX, sid);
    }

    rep = redisCommand(rc, "EXISTS %s", key);
    if (!rep) return 0;
    int ok = (rep->type == REDIS_REPLY_INTEGER && rep->integer == 1);
    freeReplyObject(rep);

    if (ok) {
        /* Refresh TTL on activity */
        rep = redisCommand(rc, "EXPIRE %s %d", key, CHAT_AUTH_ADMIN_TTL);
        if (rep) freeReplyObject(rep);
    }

    return ok;
}

/* ── Admin login ─────────────────────────────────────────────── */

int
chat_auth_admin_login(ngx_http_request_t *r,
    const char *provided, const char *expected,
    redisContext *rc, ngx_log_t *log)
{
    char        token[48];
    char        key[128];
    redisReply *rep;

    if (!provided || !expected) return 0;
    if (strcmp(provided, expected) != 0) return 0;
    if (!rc) return 0;

    chat_auth_gen_token(token, 40);
    snprintf(key, sizeof(key), "%s%s", CHAT_AUTH_ADMIN_PFX, token);

    rep = redisCommand(rc, "SETEX %s %d 1", key, CHAT_AUTH_ADMIN_TTL);
    if (!rep) return 0;
    freeReplyObject(rep);

    chat_auth_set_cookie(r, "admin_sid", token, CHAT_AUTH_ADMIN_TTL, 1);
    return 1;
}

/* ── Admin logout ────────────────────────────────────────────── */

void
chat_auth_admin_logout(ngx_http_request_t *r,
    redisContext *rc, ngx_log_t *log)
{
    ngx_str_t sid_cookie = chat_auth_get_cookie(r, "admin_sid");

    if (rc && sid_cookie.len > 0 && sid_cookie.len < 64) {
        char key[128];
        char sid[64] = {0};
        ngx_memcpy(sid, sid_cookie.data, sid_cookie.len);
        snprintf(key, sizeof(key), "%s%s", CHAT_AUTH_ADMIN_PFX, sid);
        redisReply *rep = redisCommand(rc, "DEL %s", key);
        if (rep) freeReplyObject(rep);
    }

    /* Clear both cookies */
    chat_auth_set_cookie(r, "admin_sid", "", 0, 1);
    chat_auth_set_cookie(r, "uid",       "", 0, 1);
}

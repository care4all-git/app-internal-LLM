#ifndef CHAT_AUTH_H
#define CHAT_AUTH_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <hiredis/hiredis.h>

#define CHAT_AUTH_MAX_USERS  3
#define CHAT_AUTH_USER_TTL   3600    /* seconds before slot freed */
#define CHAT_AUTH_ADMIN_TTL  86400   /* admin session TTL */
#define CHAT_AUTH_USERS_KEY  "auth:users"
#define CHAT_AUTH_ADMIN_PFX  "auth:admin:sid:"

typedef enum {
    CHAT_AUTH_NONE  = 0,
    CHAT_AUTH_NEW   = 1,  /* new slot assigned */
    CHAT_AUTH_USER  = 2,  /* existing anonymous user */
    CHAT_AUTH_ADMIN = 3,  /* valid admin session */
    CHAT_AUTH_FULL  = 4,  /* server at capacity */
} chat_auth_type_t;

typedef struct {
    chat_auth_type_t type;
    char             uid[72];     /* user id */
    int              slot;        /* 1-3 */
    int              total_users; /* active count */
} chat_auth_info_t;

ngx_str_t  chat_auth_get_cookie(ngx_http_request_t *r, const char *name);
void       chat_auth_set_cookie(ngx_http_request_t *r,
               const char *name, const char *value, int max_age, int http_only);
void       chat_auth_gen_token(char *buf, size_t len);
ngx_int_t  chat_auth_identify(ngx_http_request_t *r,
               redisContext *rc, chat_auth_info_t *info);
int        chat_auth_is_admin(ngx_http_request_t *r,
               redisContext *rc, ngx_log_t *log);
int        chat_auth_admin_login(ngx_http_request_t *r,
               const char *provided, const char *expected,
               redisContext *rc, ngx_log_t *log);
void       chat_auth_admin_logout(ngx_http_request_t *r,
               redisContext *rc, ngx_log_t *log);
void       chat_auth_cleanup_users(redisContext *rc, ngx_log_t *log);

#endif

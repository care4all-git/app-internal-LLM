#ifndef CHAT_UTILS_H
#define CHAT_UTILS_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "cJSON.h"

/* ── ID generation ──────────────────────────────────────── */
/* Writes "chat(<ms_timestamp>)" into buf (caller must supply ≥40 bytes).
   Returns pointer to buf. */
char *chat_generate_chat_id(char *buf, size_t bufsz);

/* Writes "admin(<n>)" or "jai(<n>)" depending on is_user flag. */
char *chat_generate_message_id(char *buf, size_t bufsz,
    const char *role,    /* "user" → admin, else → jai */
    long counter_value);

/* Writes "<parent_id>_code(<index>)". */
char *chat_generate_artifact_id(char *buf, size_t bufsz,
    const char *parent_id, int block_index);

/* ── Validation ─────────────────────────────────────────── */
int chat_is_valid_chat_id(const char *id);    /* chat(\d+) */
int chat_is_valid_message_id(const char *id); /* admin|jai(\d+) or _code variant */

/* ── HTTP helpers ───────────────────────────────────────── */
void chat_set_cors_headers(ngx_http_request_t *r);

/* Add a custom response header (key/value must outlive the request). */
void chat_add_header(ngx_http_request_t *r,
    const char *key, const char *value);

/* ── String / text helpers ──────────────────────────────── */
/* HTML-escape src into dst (dst must be ≥ 6*srclen+1 bytes). */
void chat_escape_html(const char *src, char *dst, size_t dstsz);

/* Simple snprintf-style nul-terminated strcat into a fixed buffer.
   Returns bytes written (not counting NUL). */
int chat_strbuf_append(char *buf, size_t bufsz, size_t *pos,
    const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Format bytes to human-readable "1.2 MB" style. */
void chat_format_file_size(long bytes, char *out, size_t outsz);

/* ── Logging ────────────────────────────────────────────── */
void chat_log_info(ngx_log_t *log, const char *module,
    const char *action, const char *detail);
void chat_log_error(ngx_log_t *log, const char *module,
    const char *action, const char *detail);

/* ── MIME helpers ───────────────────────────────────────── */
const char *chat_mime_for_path(const char *path);
int chat_is_text_mime(const char *mime);
int chat_is_text_extension(const char *filename);

#endif /* CHAT_UTILS_H */

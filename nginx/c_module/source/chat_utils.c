#include "../headers/chat_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

/* ── ID generation ──────────────────────────────────────── */

char *
chat_generate_chat_id(char *buf, size_t bufsz)
{
    struct timespec ts;
    long long ms;

    clock_gettime(CLOCK_REALTIME, &ts);
    ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    snprintf(buf, bufsz, "chat(%lld)", ms);
    return buf;
}

char *
chat_generate_message_id(char *buf, size_t bufsz,
    const char *role, long counter_value)
{
    const char *prefix = (strcmp(role, "user") == 0) ? "admin" : "jai";
    snprintf(buf, bufsz, "%s(%ld)", prefix, counter_value);
    return buf;
}

char *
chat_generate_artifact_id(char *buf, size_t bufsz,
    const char *parent_id, int block_index)
{
    snprintf(buf, bufsz, "%s_code(%d)", parent_id, block_index);
    return buf;
}

/* ── Validation ─────────────────────────────────────────── */

int
chat_is_valid_chat_id(const char *id)
{
    /* matches chat(\d+) */
    const char *p;
    if (!id) return 0;
    if (strncmp(id, "chat(", 5) != 0) return 0;
    p = id + 5;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return (*p == ')' && *(p+1) == '\0');
}

int
chat_is_valid_message_id(const char *id)
{
    /* matches admin(\d+), jai(\d+), admin(\d+)_code(\d+), jai(\d+)_code(\d+) */
    const char *p = id;
    if (!id) return 0;
    if (strncmp(p, "admin(", 6) == 0) p += 6;
    else if (strncmp(p, "jai(", 4) == 0)   p += 4;
    else return 0;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != ')') return 0;
    p++;
    if (*p == '\0') return 1;
    /* optional _code(\d+) suffix */
    if (strncmp(p, "_code(", 6) != 0) return 0;
    p += 6;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    return (*p == ')' && *(p+1) == '\0');
}

/* ── HTTP helpers ───────────────────────────────────────── */

void
chat_set_cors_headers(ngx_http_request_t *r)
{
    chat_add_header(r, "Access-Control-Allow-Origin", "*");
    chat_add_header(r, "Access-Control-Allow-Methods",
        "GET, POST, OPTIONS, DELETE");
    chat_add_header(r, "Access-Control-Allow-Headers",
        "Content-Type, Accept, Content-Length");
    chat_add_header(r, "Access-Control-Expose-Headers", "Content-Length");
}

void
chat_add_header(ngx_http_request_t *r, const char *key, const char *value)
{
    ngx_table_elt_t *h;
    ngx_str_t        k, v;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) return;

    k.len  = strlen(key);
    k.data = ngx_pnalloc(r->pool, k.len);
    if (!k.data) return;
    ngx_memcpy(k.data, key, k.len);

    v.len  = strlen(value);
    v.data = ngx_pnalloc(r->pool, v.len);
    if (!v.data) return;
    ngx_memcpy(v.data, value, v.len);

    h->key   = k;
    h->value = v;
    h->hash  = 1;
    h->lowcase_key = ngx_pnalloc(r->pool, k.len);
    if (h->lowcase_key) {
        ngx_strlow(h->lowcase_key, k.data, k.len);
    }
}

/* ── String helpers ─────────────────────────────────────── */

void
chat_escape_html(const char *src, char *dst, size_t dstsz)
{
    size_t i = 0;
    if (!src || !dst || dstsz == 0) return;
    while (*src && i + 7 < dstsz) {
        switch (*src) {
        case '&':  memcpy(dst+i, "&amp;",  5); i += 5; break;
        case '<':  memcpy(dst+i, "&lt;",   4); i += 4; break;
        case '>':  memcpy(dst+i, "&gt;",   4); i += 4; break;
        case '"':  memcpy(dst+i, "&quot;", 6); i += 6; break;
        case '\'': memcpy(dst+i, "&#39;",  5); i += 5; break;
        default:   dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
}

int
chat_strbuf_append(char *buf, size_t bufsz, size_t *pos,
    const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (!buf || *pos >= bufsz) return 0;
    va_start(ap, fmt);
    n = vsnprintf(buf + *pos, bufsz - *pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *pos += (size_t)n;
        if (*pos > bufsz) *pos = bufsz;
    }
    return n;
}

void
chat_format_file_size(long bytes, char *out, size_t outsz)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double      size    = (double)bytes;
    int         idx     = 0;

    if (bytes <= 0) { snprintf(out, outsz, "0 B"); return; }
    while (size >= 1024.0 && idx < 4) { size /= 1024.0; idx++; }
    if (idx == 0) snprintf(out, outsz, "%ld B", bytes);
    else          snprintf(out, outsz, "%.1f %s", size, units[idx]);
}

/* ── Logging ────────────────────────────────────────────── */

void
chat_log_info(ngx_log_t *log, const char *module,
    const char *action, const char *detail)
{
    ngx_log_error(NGX_LOG_INFO, log, 0,
        "chat [%s] %s: %s", module, action, detail ? detail : "");
}

void
chat_log_error(ngx_log_t *log, const char *module,
    const char *action, const char *detail)
{
    ngx_log_error(NGX_LOG_ERR, log, 0,
        "chat [%s] %s: %s", module, action, detail ? detail : "");
}

/* ── MIME helpers ───────────────────────────────────────── */

static const struct { const char *ext; const char *mime; } s_mime_map[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm",  "text/html; charset=utf-8"},
    {"css",  "text/css; charset=utf-8"},
    {"js",   "application/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"png",  "image/png"},
    {"jpg",  "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif",  "image/gif"},
    {"svg",  "image/svg+xml"},
    {"ico",  "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2","font/woff2"},
    {"ttf",  "font/ttf"},
    {"xml",  "application/xml; charset=utf-8"},
    {"txt",  "text/plain; charset=utf-8"},
    {"md",   "text/markdown; charset=utf-8"},
    {NULL, NULL}
};

const char *
chat_mime_for_path(const char *path)
{
    const char *dot;
    int i;

    if (!path) return "application/octet-stream";
    dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    for (i = 0; s_mime_map[i].ext; i++) {
        if (strcasecmp(dot, s_mime_map[i].ext) == 0)
            return s_mime_map[i].mime;
    }
    return "application/octet-stream";
}

int
chat_is_text_mime(const char *mime)
{
    static const char *text_prefixes[] = {
        "text/", "application/json", "application/xml",
        "application/javascript", "application/csv",
        "application/sql", "application/yaml", NULL
    };
    int i;
    if (!mime) return 0;
    for (i = 0; text_prefixes[i]; i++) {
        if (strncmp(mime, text_prefixes[i], strlen(text_prefixes[i])) == 0)
            return 1;
    }
    return 0;
}

int
chat_is_text_extension(const char *filename)
{
    static const char *text_exts[] = {
        ".txt", ".md", ".json", ".xml", ".csv", ".sql",
        ".js", ".ts", ".py", ".java", ".cpp", ".c", ".h",
        ".css", ".html", ".yml", ".yaml", ".toml", ".ini",
        ".cfg", ".conf", ".log", NULL
    };
    const char *dot;
    int i;
    if (!filename) return 0;
    dot = strrchr(filename, '.');
    if (!dot) return 0;
    for (i = 0; text_exts[i]; i++) {
        if (strcasecmp(dot, text_exts[i]) == 0) return 1;
    }
    return 0;
}

#include "../headers/chat_sse.h"
#include "../headers/chat_utils.h"
#include "../headers/cJSON.h"
#include <stdio.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────── */

/*
 * Write a single nginx output buffer chain containing `data` of length `len`.
 * If `last` is set the buffer is marked last_buf which tells nginx to flush
 * and, for HTTP/1.1, to close the chunked stream.
 */
static ngx_int_t
sse_write_buf(ngx_http_request_t *r,
    const char *data, size_t len, int last)
{
    ngx_buf_t   *b;
    ngx_chain_t  out;
    u_char      *p;

    if (!data || len == 0) return NGX_OK;

    p = ngx_pnalloc(r->pool, len);
    if (!p) return NGX_ERROR;
    ngx_memcpy(p, data, len);

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (!b) return NGX_ERROR;

    b->pos      = p;
    b->last     = p + len;
    b->memory   = 1;
    b->flush    = 1;   /* ask filter chain to flush immediately */
    b->last_buf = last ? 1 : 0;

    out.buf  = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}

/* ── Setup ──────────────────────────────────────────────── */

void
chat_sse_setup_headers(ngx_http_request_t *r)
{
    static ngx_str_t content_type =
        ngx_string("text/event-stream; charset=utf-8");

    r->headers_out.content_type     = content_type;
    r->headers_out.content_type_len = content_type.len;
    r->headers_out.content_length_n = -1;  /* unknown length = chunked */

    chat_add_header(r, "Cache-Control",    "no-cache");
    chat_add_header(r, "Connection",       "keep-alive");
    chat_add_header(r, "X-Accel-Buffering","no");
    chat_set_cors_headers(r);
}

int
chat_sse_validate_client(ngx_http_request_t *r)
{
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;
    ngx_uint_t        i;

    /* Walk the incoming headers list looking for Accept. */
    part = &r->headers_in.headers.part;
    h    = part->elts;

    for (i = 0; ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) break;
            part = part->next;
            h    = part->elts;
            i    = 0;
        }
        if (h[i].key.len == 6 &&
            ngx_strncasecmp(h[i].key.data, (u_char *)"accept", 6) == 0)
        {
            /* Absent value, wildcard, or explicit SSE type all pass. */
            if (h[i].value.len == 0 ||
                ngx_strnstr(h[i].value.data, "*/*",
                            h[i].value.len) ||
                ngx_strnstr(h[i].value.data, "text/event-stream",
                            h[i].value.len))
            {
                return 1;
            }
            return 0;
        }
    }

    /* No Accept header present — allow. */
    return 1;
}

/* ── Low-level output ────────────────────────────────────── */

ngx_int_t
chat_sse_send_raw(ngx_http_request_t *r,
    const char *event_type, const char *data_str, int last)
{
    char   frame[8192];
    size_t pos = 0;

    if (event_type) {
        pos += snprintf(frame + pos, sizeof(frame) - pos,
            "event: %s\n", event_type);
    }
    pos += snprintf(frame + pos, sizeof(frame) - pos,
        "data: %s\n\n", data_str ? data_str : "");

    return sse_write_buf(r, frame, pos, last);
}

/* ── High-level helpers ──────────────────────────────────── */

ngx_int_t
chat_sse_send_chat_id(ngx_http_request_t *r, const char *chat_id)
{
    char json[128];
    snprintf(json, sizeof(json),
        "{\"chat_id\":\"%s\",\"type\":\"chat_id\"}", chat_id);
    return chat_sse_send_raw(r, "chat_id", json, 0);
}

ngx_int_t
chat_sse_send_content(ngx_http_request_t *r, const char *content)
{
    /* Escape content for embedding in JSON value */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "content", content ? content : "");
    cJSON_AddStringToObject(obj, "type",    "content");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return NGX_ERROR;
    ngx_int_t rc = chat_sse_send_raw(r, "content", json, 0);
    free(json);
    return rc;
}

ngx_int_t
chat_sse_send_completion_status(ngx_http_request_t *r,
    int is_complete, int needs_continuation, const char *reason)
{
    char json[256];
    snprintf(json, sizeof(json),
        "{\"is_complete\":%s,\"needs_continuation\":%s,"
        "\"completion_reason\":\"%s\",\"type\":\"completion_status\"}",
        is_complete        ? "true"  : "false",
        needs_continuation ? "true"  : "false",
        reason             ? reason  : "finished");
    return chat_sse_send_raw(r, "completion_status", json, 0);
}

ngx_int_t
chat_sse_send_error(ngx_http_request_t *r,
    const char *msg, const char *details)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error",  msg     ? msg     : "Error");
    cJSON_AddStringToObject(obj, "type",   "error");
    if (details)
        cJSON_AddStringToObject(obj, "details", details);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return NGX_ERROR;
    ngx_int_t rc = chat_sse_send_raw(r, "error", json, 0);
    free(json);
    return rc;
}

ngx_int_t
chat_sse_send_status(ngx_http_request_t *r,
    const char *status, const char *message)
{
    char json[256];
    snprintf(json, sizeof(json),
        "{\"status\":\"%s\",\"message\":\"%s\",\"type\":\"status\"}",
        status  ? status  : "",
        message ? message : "");
    return chat_sse_send_raw(r, "status", json, 0);
}

ngx_int_t
chat_sse_send_done(ngx_http_request_t *r,
    int is_complete, const char *completion_reason)
{
    ngx_int_t rc;

    if (!is_complete) {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"type\":\"continuation_needed\","
            "\"reason\":\"%s\","
            "\"message\":\"Response may be incomplete. "
                "Would you like me to continue?\"}",
            completion_reason ? completion_reason : "truncated");
        rc = chat_sse_send_raw(r, "continuation_needed", json, 0);
        if (rc != NGX_OK) return rc;
    }

    return chat_sse_send_raw(r, "done", "[DONE]", 1);
}

/* ── Stream result ───────────────────────────────────────── */

int
chat_sse_stream_result(ngx_http_request_t *r,
    const chat_ollama_result_t *result)
{
    int   n, i;
    cJSON *chunk;

    if (!result || !result->chunks) {
        chat_sse_send_error(r, "No response from AI service", NULL);
        return 0;
    }

    n = cJSON_GetArraySize(result->chunks);
    for (i = 0; i < n; i++) {
        cJSON *content_item;
        chunk        = cJSON_GetArrayItem(result->chunks, i);
        content_item = cJSON_GetObjectItem(chunk, "content");
        if (content_item && cJSON_IsString(content_item) &&
            content_item->valuestring[0] != '\0')
        {
            chat_sse_send_content(r, content_item->valuestring);
        }
    }

    chat_sse_send_completion_status(r,
        result->completion.is_complete,
        !result->completion.is_complete,
        result->completion.completion_reason);

    return 1;
}

/* Helper for cJSON string type check (handles older cJSON versions) */
#ifndef cJSON_IsString
#define cJSON_IsString(x) ((x) && (x)->type == cJSON_String)
#endif

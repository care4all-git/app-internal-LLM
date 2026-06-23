#ifndef CHAT_SSE_H
#define CHAT_SSE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "chat_ollama.h"

/* ── Setup ──────────────────────────────────────────────────── */

/* Set response headers for an SSE stream (Content-Type, Cache-Control, etc.).
   Must be called before ngx_http_send_header(). */
void chat_sse_setup_headers(ngx_http_request_t *r);

/* Return 1 if the client Accept header allows text/event-stream or any type.
   Return 0 if the client explicitly rejects SSE. */
int chat_sse_validate_client(ngx_http_request_t *r);

/* ── Low-level frame sender ─────────────────────────────────── */

/* Write one SSE frame: "event: <event_type>\ndata: <data_str>\n\n".
   If last is non-zero the buffer is marked last_buf to flush and close. */
ngx_int_t chat_sse_send_raw(ngx_http_request_t *r,
    const char *event_type, const char *data_str, int last);

/* ── High-level event senders ───────────────────────────────── */

/* Send a chat_id event: {"chat_id":"…","type":"chat_id"} */
ngx_int_t chat_sse_send_chat_id(ngx_http_request_t *r, const char *chat_id);

/* Send a content chunk: {"content":"…","type":"content"} */
ngx_int_t chat_sse_send_content(ngx_http_request_t *r, const char *content);

/* Send completion metadata: is_complete, needs_continuation, reason. */
ngx_int_t chat_sse_send_completion_status(ngx_http_request_t *r,
    int is_complete, int needs_continuation, const char *reason);

/* Send an error event: {"error":"…","type":"error"[,"details":"…"]} */
ngx_int_t chat_sse_send_error(ngx_http_request_t *r,
    const char *msg, const char *details);

/* Send a status event: {"status":"…","message":"…","type":"status"} */
ngx_int_t chat_sse_send_status(ngx_http_request_t *r,
    const char *status, const char *message);

/* Close the SSE stream. If is_complete is false, emits a continuation_needed
   event first, then the terminal "done" frame. */
ngx_int_t chat_sse_send_done(ngx_http_request_t *r,
    int is_complete, const char *completion_reason);

/* ── Streaming helper ───────────────────────────────────────── */

/* Iterate over result->chunks and stream each content piece to the client,
   then emit a completion_status event.
   Returns 1 on success, 0 if result has no chunks. */
int chat_sse_stream_result(ngx_http_request_t *r,
    const chat_ollama_result_t *result);

#endif /* CHAT_SSE_H */

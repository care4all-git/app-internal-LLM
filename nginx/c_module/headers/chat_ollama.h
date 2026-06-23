#ifndef CHAT_OLLAMA_H
#define CHAT_OLLAMA_H

#include <ngx_config.h>
#include <ngx_core.h>
#include "cJSON.h"

/* ── Result structures ───────────────────────────────────── */

typedef struct {
    int  is_complete;       /* 1 if Ollama sent done:true */
    char completion_reason[64];
    int  seems_truncated;
} chat_ollama_completion_t;

/* Caller must free response and chunks with cJSON_Delete / free */
typedef struct {
    char                    *full_response;  /* heap-allocated, caller frees */
    cJSON                   *chunks;         /* cJSON array of {content} objects */
    chat_ollama_completion_t completion;
    char                     error[512];     /* non-empty on failure */
} chat_ollama_result_t;

/* SSE write-buffer accumulator used by curl write callback */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} chat_ollama_buf_t;

/* ── API ─────────────────────────────────────────────────── */

/* Send context_array (cJSON array of {role,content}) to Ollama.
   model_url e.g. "http://ollama:11434"
   model_name e.g. "devstral"
   Populates result; returns 1 on success, 0 on failure.
   Caller must free result.full_response and cJSON_Delete result.chunks. */
int chat_ollama_stream_chat(ngx_log_t *log,
    const char *model_url, const char *model_name,
    double temperature, double top_p, int top_k,
    int num_ctx, double repeat_penalty,
    cJSON *context_array,
    chat_ollama_result_t *result);

/* Check if Ollama is healthy and the model is available.
   Returns 1 on success. */
int chat_ollama_health_check(ngx_log_t *log,
    const char *model_url, const char *model_name,
    char *error_out, size_t error_outsz);

/* Map a raw error string to a user-friendly message.
   Writes into out (at most outsz bytes). */
void chat_ollama_friendly_error(const char *err,
    char *out, size_t outsz);

#endif /* CHAT_OLLAMA_H */

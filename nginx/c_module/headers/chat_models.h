#ifndef CHAT_MODELS_H
#define CHAT_MODELS_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "cJSON.h"

/* ── Ollama model management ─────────────────────────────────── */

/* Return JSON string of installed Ollama models (from /api/tags).
   Caller must free(). Returns NULL on error. */
char *chat_models_list_json(const char *ollama_url, ngx_log_t *log);

/* Delete a model from Ollama. Returns 1 on success.
   err_out receives an error message on failure (may be NULL). */
int chat_models_delete_model(const char *ollama_url, const char *model_name,
    char *err_out, size_t err_outsz, ngx_log_t *log);

/* Pull a model via Ollama /api/pull, streaming NDJSON progress back to
   the client as SSE events on the already-opened request r.
   Caller must have set SSE headers and sent the HTTP header before calling.
   Returns 1 on success (Ollama reported "success"), 0 on failure. */
int chat_models_pull_stream(ngx_http_request_t *r,
    const char *ollama_url, const char *model_name, ngx_log_t *log);

/* ── HuggingFace browser ─────────────────────────────────────── */

/* Search HuggingFace for GGUF models. Returns a JSON array string.
   Caller must free(). Returns NULL on error. */
char *chat_hf_search_json(const char *query, int limit,
    const char *hf_token, ngx_log_t *log);

/* List GGUF files in a HuggingFace model repo. Returns a JSON array string
   containing {filename, size, pull_name} objects. Caller must free().
   Returns NULL on error. */
char *chat_hf_files_json(const char *model_id,
    const char *hf_token, ngx_log_t *log);

#endif /* CHAT_MODELS_H */

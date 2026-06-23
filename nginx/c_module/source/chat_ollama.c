/*
 * chat_ollama.c — OpenAI-compatible inference client
 *
 * Works against vLLM, llama-server, OR Ollama's /v1 compatibility layer.
 * All three expose the same OpenAI /v1/chat/completions endpoint.
 */

#include "../headers/chat_ollama.h"
#include "../headers/chat_utils.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef cJSON_IsString
#define cJSON_IsString(item) ((item) && (item)->type == cJSON_String)
#endif

/* ── Dynamic buffer ──────────────────────────────────────────── */

static int
buf_append(chat_ollama_buf_t *b, const char *data, size_t len)
{
    if (b->len + len + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 65536;
        while (ncap < b->len + len + 1) ncap *= 2;
        char *p = realloc(b->buf, ncap);
        if (!p) return 0;
        b->buf = p;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return 1;
}

static size_t
curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    chat_ollama_buf_t *b = (chat_ollama_buf_t *)userdata;
    size_t total = size * nmemb;
    if (!buf_append(b, ptr, total)) return 0;
    return total;
}

/* ── Parse OpenAI SSE streaming response ─────────────────────── */
/*
 * vLLM / llama-server / Ollama /v1 all stream in OpenAI SSE format:
 *
 *   data: {"id":"...","choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]}
 *   data: {"id":"...","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}
 *   data: [DONE]
 *
 * We parse line-by-line, accumulate content chunks, and detect finish_reason.
 */
static int
parse_openai_sse(ngx_log_t *log,
    const char *body, size_t body_len,
    chat_ollama_result_t *res)
{
    char   *text = malloc(body_len + 1);
    char   *line, *saveptr;
    cJSON  *chunks;
    char   *full     = NULL;
    size_t  full_len = 0, full_cap = 0;

    if (!text) { snprintf(res->error, sizeof(res->error), "OOM"); return 0; }
    memcpy(text, body, body_len);
    text[body_len] = '\0';

    chunks = cJSON_CreateArray();
    res->completion.is_complete       = 0;
    res->completion.seems_truncated   = 0;
    res->completion.completion_reason[0] = '\0';

    for (line = strtok_r(text, "\r\n", &saveptr);
         line;
         line = strtok_r(NULL, "\r\n", &saveptr))
    {
        const char *data;
        cJSON      *obj, *choices, *choice, *delta, *content_item, *finish_item;
        const char *content;
        size_t      clen;

        if (*line == '\0') continue;

        /* Must start with "data: " */
        if (strncmp(line, "data: ", 6) != 0) continue;
        data = line + 6;

        /* Terminal marker */
        if (strcmp(data, "[DONE]") == 0) {
            if (!res->completion.is_complete) {
                res->completion.is_complete = 1;
                strncpy(res->completion.completion_reason, "stop",
                    sizeof(res->completion.completion_reason) - 1);
            }
            break;
        }

        obj = cJSON_Parse(data);
        if (!obj) continue;

        /* API-level error */
        {
            cJSON *err = cJSON_GetObjectItem(obj, "error");
            if (err) {
                cJSON *msg = cJSON_GetObjectItem(err, "message");
                snprintf(res->error, sizeof(res->error), "API error: %s",
                    (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
                cJSON_Delete(obj);
                goto fail;
            }
        }

        /* Extract from choices[0].delta.content */
        choices = cJSON_GetObjectItem(obj, "choices");
        if (!choices || !cJSON_IsArray(choices) ||
            cJSON_GetArraySize(choices) == 0)
        {
            cJSON_Delete(obj);
            continue;
        }

        choice       = cJSON_GetArrayItem(choices, 0);
        delta        = cJSON_GetObjectItem(choice, "delta");
        content_item = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
        finish_item  = cJSON_GetObjectItem(choice, "finish_reason");

        content = (content_item && cJSON_IsString(content_item) &&
                   content_item->valuestring)
                  ? content_item->valuestring : "";
        clen = strlen(content);

        if (clen > 0) {
            /* Grow full-response buffer */
            if (full_len + clen + 1 > full_cap) {
                size_t ncap = full_cap ? full_cap * 2 : 65536;
                while (ncap < full_len + clen + 1) ncap *= 2;
                char *nb = realloc(full, ncap);
                if (!nb) {
                    cJSON_Delete(obj);
                    snprintf(res->error, sizeof(res->error), "OOM");
                    goto fail;
                }
                full     = nb;
                full_cap = ncap;
            }
            memcpy(full + full_len, content, clen);
            full_len      += clen;
            full[full_len]  = '\0';

            /* Add to chunks array */
            {
                cJSON *chunk = cJSON_CreateObject();
                cJSON_AddStringToObject(chunk, "content", content);
                cJSON_AddItemToArray(chunks, chunk);
            }
        }

        if (finish_item && cJSON_IsString(finish_item) &&
            finish_item->valuestring && finish_item->valuestring[0])
        {
            res->completion.is_complete = 1;
            strncpy(res->completion.completion_reason,
                finish_item->valuestring,
                sizeof(res->completion.completion_reason) - 1);
        }

        cJSON_Delete(obj);
    }

    free(text);

    res->full_response = (full_len > 0) ? full : strdup("");
    res->chunks        = chunks;

    /* Heuristic truncation check */
    if (!res->completion.is_complete && res->full_response &&
        strlen(res->full_response) > 50)
    {
        const char *tail = res->full_response
            + strlen(res->full_response) - 50;
        if (strstr(tail, "...") || strstr(tail, "[continued]") ||
            strstr(tail, "[truncated]"))
        {
            res->completion.seems_truncated = 1;
        }
    }

    return 1;

fail:
    free(text);
    if (full) free(full);
    cJSON_Delete(chunks);
    res->full_response = NULL;
    res->chunks        = NULL;
    return 0;
}

/* ── stream_chat ──────────────────────────────────────────────── */

int
chat_ollama_stream_chat(ngx_log_t *log,
    const char *model_url, const char *model_name,
    double temperature, double top_p, int top_k,
    int num_ctx, double repeat_penalty,
    cJSON *context_array,
    chat_ollama_result_t *result)
{
    CURL               *curl;
    CURLcode            curlrc;
    struct curl_slist  *headers = NULL;
    char                url[512];
    cJSON              *payload;
    char               *payload_str;
    chat_ollama_buf_t   buf = {0};
    long                http_code = 0;
    int                 ok = 0;
    double              freq_penalty;

    memset(result, 0, sizeof(*result));

    /*
     * OpenAI /v1/chat/completions request.
     *
     * - temperature, top_p  → standard params
     * - frequency_penalty   → approximates Ollama's repeat_penalty:
     *                         freq_penalty = clamp(repeat_penalty - 1.0, 0, 2)
     * - max_tokens          → -1 means unlimited (vLLM/llama-server honour this)
     * - top_k               → non-standard; vLLM accepts it as an extra param
     */
    freq_penalty = repeat_penalty - 1.0;
    if (freq_penalty < 0.0) freq_penalty = 0.0;
    if (freq_penalty > 2.0) freq_penalty = 2.0;

    payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "model",             model_name);
    cJSON_AddItemToObject  (payload, "messages",
        cJSON_Duplicate(context_array, 1));
    cJSON_AddTrueToObject  (payload, "stream");
    cJSON_AddNumberToObject(payload, "temperature",       temperature);
    cJSON_AddNumberToObject(payload, "top_p",             top_p);
    cJSON_AddNumberToObject(payload, "frequency_penalty", freq_penalty);
    cJSON_AddNumberToObject(payload, "max_tokens",        -1.0);
    /* vLLM-specific extra params (ignored by other engines) */
    {
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddNumberToObject(extra, "top_k",   (double)top_k);
        cJSON_AddItemToObject(payload, "extra_body", extra);
    }

    payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_str) {
        snprintf(result->error, sizeof(result->error), "OOM building payload");
        return 0;
    }

    /* OpenAI-compatible endpoint — works with vLLM, llama-server, Ollama /v1 */
    snprintf(url, sizeof(url), "%s/v1/chat/completions", model_url);

    curl = curl_easy_init();
    if (!curl) {
        free(payload_str);
        snprintf(result->error, sizeof(result->error), "curl_easy_init failed");
        return 0;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(payload_str));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        0L);   /* no timeout */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    curlrc = curl_easy_perform(curl);
    free(payload_str);
    curl_slist_free_all(headers);

    if (curlrc != CURLE_OK) {
        snprintf(result->error, sizeof(result->error),
            "curl error: %s", curl_easy_strerror(curlrc));
        curl_easy_cleanup(curl);
        free(buf.buf);
        return 0;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
        snprintf(result->error, sizeof(result->error),
            "API returned HTTP %ld: %.300s",
            http_code, buf.buf ? buf.buf : "");
        free(buf.buf);
        return 0;
    }

    ok = parse_openai_sse(log, buf.buf, buf.len, result);
    free(buf.buf);
    return ok;
}

/* ── health_check ─────────────────────────────────────────────── */

int
chat_ollama_health_check(ngx_log_t *log,
    const char *model_url, const char *model_name,
    char *error_out, size_t error_outsz)
{
    CURL               *curl;
    CURLcode            curlrc;
    chat_ollama_buf_t   buf = {0};
    char                url[512];
    long                http_code = 0;

    /* vLLM / llama-server / Ollama-v1 all expose GET /v1/models */
    snprintf(url, sizeof(url), "%s/v1/models", model_url);

    curl = curl_easy_init();
    if (!curl) {
        snprintf(error_out, error_outsz, "curl_easy_init failed");
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    curlrc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    free(buf.buf);

    if (curlrc != CURLE_OK) {
        snprintf(error_out, error_outsz,
            "Cannot connect to inference server: %s",
            curl_easy_strerror(curlrc));
        return 0;
    }

    if (http_code != 200) {
        snprintf(error_out, error_outsz,
            "Inference server returned HTTP %ld", http_code);
        return 0;
    }

    return 1;

    (void)model_name; /* model checking done dynamically at request time */
}

/* ── friendly error ───────────────────────────────────────────── */

void
chat_ollama_friendly_error(const char *err, char *out, size_t outsz)
{
    if (!err) { snprintf(out, outsz, "Unknown error"); return; }

    if (strstr(err, "connection refused") ||
        strstr(err, "timeout") ||
        strstr(err, "connect"))
    {
        snprintf(out, outsz,
            "Cannot connect to inference server. "
            "Is vLLM running?");
    } else if (strstr(err, "model") && strstr(err, "not found")) {
        snprintf(out, outsz,
            "Model not loaded. Check vLLM model configuration.");
    } else if (strstr(err, "HTTP 4")) {
        snprintf(out, outsz,
            "Invalid request to inference server. Please try again.");
    } else if (strstr(err, "HTTP 5")) {
        snprintf(out, outsz,
            "Inference server error. Please try again in a moment.");
    } else {
        snprintf(out, outsz, "Inference error: %.400s", err);
    }
}

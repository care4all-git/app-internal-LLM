#include "../headers/chat_models.h"
#include "../headers/chat_sse.h"
#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Shared curl write buffer ────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} models_buf_t;

static int
mbuf_append(models_buf_t *b, const char *data, size_t len)
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
mbuf_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    models_buf_t *b = (models_buf_t *)userdata;
    size_t total = size * nmemb;
    if (!mbuf_append(b, ptr, total)) return 0;
    return total;
}

/* ── Simple URL percent-encoder ──────────────────────────────── */

static void
url_encode(const char *src, char *dst, size_t dstsz)
{
    size_t di = 0;
    while (*src && di + 4 < dstsz) {
        unsigned char c = (unsigned char)*src++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            dst[di++] = '%';
            dst[di++] = "0123456789ABCDEF"[c >> 4];
            dst[di++] = "0123456789ABCDEF"[c & 0xf];
        }
    }
    dst[di] = '\0';
}

/* ══════════════════════════════════════════════════════════════
 * Ollama: list installed models
 * ══════════════════════════════════════════════════════════════ */

char *
chat_models_list_json(const char *ollama_url, ngx_log_t *log)
{
    CURL         *curl;
    CURLcode      curlrc;
    models_buf_t  buf = {0};
    char          url[512];
    long          http_code = 0;

    /* Ollama native tags endpoint */
    snprintf(url, sizeof(url), "%s/api/tags", ollama_url);

    curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  mbuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    curlrc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (curlrc != CURLE_OK || http_code != 200) {
        free(buf.buf);
        return NULL;
    }

    return buf.buf; /* caller must free */
}

/* ══════════════════════════════════════════════════════════════
 * Ollama: delete a model
 * ══════════════════════════════════════════════════════════════ */

int
chat_models_delete_model(const char *ollama_url, const char *model_name,
    char *err_out, size_t err_outsz, ngx_log_t *log)
{
    CURL               *curl;
    CURLcode            curlrc;
    struct curl_slist  *headers = NULL;
    models_buf_t        buf = {0};
    char                url[512];
    cJSON              *payload;
    char               *payload_str;
    long                http_code = 0;

    snprintf(url, sizeof(url), "%s/api/delete", ollama_url);

    payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "name", model_name);
    payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_str) return 0;

    curl = curl_easy_init();
    if (!curl) { free(payload_str); return 0; }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,  "DELETE");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(payload_str));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  mbuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    curlrc = curl_easy_perform(curl);
    free(payload_str);
    curl_slist_free_all(headers);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    free(buf.buf);

    if (curlrc != CURLE_OK) {
        if (err_out) snprintf(err_out, err_outsz,
            "curl error: %s", curl_easy_strerror(curlrc));
        return 0;
    }
    if (http_code == 200) return 1;
    if (err_out) snprintf(err_out, err_outsz,
        "Ollama returned HTTP %ld", http_code);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * Ollama: pull a model with SSE progress streaming
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    ngx_http_request_t *r;
    ngx_log_t          *log;
    char                partial[8192]; /* incomplete line accumulator */
    size_t              partial_len;
    int                 done;          /* set when Ollama reports "success" */
} pull_ctx_t;

static size_t
pull_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    pull_ctx_t *ctx   = (pull_ctx_t *)userdata;
    size_t      total = size * nmemb;
    const char *p     = ptr;
    const char *end   = ptr + total;

    while (p < end) {
        const char *nl    = memchr(p, '\n', (size_t)(end - p));
        size_t      chunk = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (ctx->partial_len + chunk < sizeof(ctx->partial) - 1) {
            memcpy(ctx->partial + ctx->partial_len, p, chunk);
            ctx->partial_len += chunk;
        }

        if (nl) {
            ctx->partial[ctx->partial_len] = '\0';
            if (ctx->partial_len > 0) {
                cJSON *obj = cJSON_Parse(ctx->partial);
                if (obj) {
                    cJSON *status    = cJSON_GetObjectItem(obj, "status");
                    cJSON *total_b   = cJSON_GetObjectItem(obj, "total");
                    cJSON *completed = cJSON_GetObjectItem(obj, "completed");
                    cJSON *digest    = cJSON_GetObjectItem(obj, "digest");

                    cJSON *evt = cJSON_CreateObject();
                    if (status && status->valuestring)
                        cJSON_AddStringToObject(evt, "status",
                            status->valuestring);
                    if (total_b && total_b->valuedouble > 0)
                        cJSON_AddNumberToObject(evt, "total",
                            total_b->valuedouble);
                    if (completed)
                        cJSON_AddNumberToObject(evt, "completed",
                            completed->valuedouble);
                    if (digest && digest->valuestring)
                        cJSON_AddStringToObject(evt, "digest",
                            digest->valuestring);

                    if (status && status->valuestring &&
                        strcmp(status->valuestring, "success") == 0)
                    {
                        ctx->done = 1;
                    }

                    char *js = cJSON_PrintUnformatted(evt);
                    if (js) {
                        chat_sse_send_raw(ctx->r, "pull_progress", js, 0);
                        free(js);
                    }
                    cJSON_Delete(evt);
                    cJSON_Delete(obj);
                }
            }
            ctx->partial_len = 0;
            p = nl + 1;
        } else {
            p = end;
        }
    }

    return total;
}

int
chat_models_pull_stream(ngx_http_request_t *r,
    const char *ollama_url, const char *model_name, ngx_log_t *log)
{
    CURL               *curl;
    CURLcode            curlrc;
    struct curl_slist  *headers = NULL;
    char                url[512];
    cJSON              *payload;
    char               *payload_str;
    pull_ctx_t          ctx;
    long                http_code = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.r   = r;
    ctx.log = log;

    snprintf(url, sizeof(url), "%s/api/pull", ollama_url);

    payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "name", model_name);
    cJSON_AddTrueToObject(payload, "stream");
    payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_str) return 0;

    curl = curl_easy_init();
    if (!curl) { free(payload_str); return 0; }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     payload_str);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  (long)strlen(payload_str));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  pull_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        0L);   /* no timeout — large models */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);

    curlrc = curl_easy_perform(curl);
    free(payload_str);
    curl_slist_free_all(headers);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (curlrc != CURLE_OK || http_code != 200) return 0;
    return ctx.done;
}

/* ══════════════════════════════════════════════════════════════
 * HuggingFace: search for GGUF models
 * ══════════════════════════════════════════════════════════════ */

char *
chat_hf_search_json(const char *query, int limit,
    const char *hf_token, ngx_log_t *log)
{
    CURL               *curl;
    CURLcode            curlrc;
    struct curl_slist  *headers = NULL;
    models_buf_t        buf = {0};
    char                url[1024];
    char                encoded[512];
    char                auth_header[320];
    long                http_code = 0;
    cJSON              *arr, *out;
    char               *result;
    int                 i, n;

    url_encode(query ? query : "", encoded, sizeof(encoded));

    /*
     * Search for text-generation models (compatible with vLLM / safetensors).
     * pipeline_tag=text-generation filters to chat/completion models.
     * sort=downloads gives the most popular first.
     */
    snprintf(url, sizeof(url),
        "https://huggingface.co/api/models"
        "?pipeline_tag=text-generation&search=%s&limit=%d&sort=downloads&direction=-1",
        encoded, limit > 0 ? limit : 20);

    curl = curl_easy_init();
    if (!curl) return NULL;

    if (hf_token && *hf_token) {
        snprintf(auth_header, sizeof(auth_header),
            "Authorization: Bearer %s", hf_token);
        headers = curl_slist_append(headers, auth_header);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  mbuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curlrc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curlrc != CURLE_OK || http_code != 200) {
        free(buf.buf);
        return strdup("{\"error\":\"HuggingFace API request failed\","
                      "\"models\":[]}");
    }

    arr = cJSON_Parse(buf.buf ? buf.buf : "[]");
    free(buf.buf);
    if (!arr) return strdup("[]");

    /* Reformat to include only what the frontend needs */
    out = cJSON_CreateArray();
    n   = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;

    for (i = 0; i < n; i++) {
        cJSON *m    = cJSON_GetArrayItem(arr, i);
        cJSON *mid  = cJSON_GetObjectItem(m, "modelId");
        cJSON *dl   = cJSON_GetObjectItem(m, "downloads");
        cJSON *lk   = cJSON_GetObjectItem(m, "likes");
        cJSON *lm   = cJSON_GetObjectItem(m, "lastModified");
        cJSON *tags = cJSON_GetObjectItem(m, "tags");

        /* Detect quantization hints from tags */
        int has_awq = 0, has_gptq = 0, has_fp8 = 0, has_gguf = 0;
        if (tags && cJSON_IsArray(tags)) {
            int j, nt = cJSON_GetArraySize(tags);
            for (j = 0; j < nt; j++) {
                cJSON *t = cJSON_GetArrayItem(tags, j);
                if (!t || !t->valuestring) continue;
                if (strstr(t->valuestring, "awq"))  has_awq  = 1;
                if (strstr(t->valuestring, "gptq")) has_gptq = 1;
                if (strstr(t->valuestring, "fp8"))  has_fp8  = 1;
                if (strstr(t->valuestring, "gguf")) has_gguf = 1;
            }
        }

        {
            cJSON *entry = cJSON_CreateObject();
            char   quant_hint[64] = "";
            if (has_fp8)  strcat(quant_hint, "FP8 ");
            if (has_awq)  strcat(quant_hint, "AWQ ");
            if (has_gptq) strcat(quant_hint, "GPTQ ");
            if (has_gguf) strcat(quant_hint, "GGUF ");

            cJSON_AddStringToObject(entry, "id",
                (mid && mid->valuestring) ? mid->valuestring : "");
            cJSON_AddNumberToObject(entry, "downloads",
                dl ? dl->valuedouble : 0);
            cJSON_AddNumberToObject(entry, "likes",
                lk ? lk->valuedouble : 0);
            cJSON_AddStringToObject(entry, "last_modified",
                (lm && lm->valuestring) ? lm->valuestring : "");
            cJSON_AddStringToObject(entry, "quant_hint",
                quant_hint[0] ? quant_hint : "safetensors");
            cJSON_AddItemToArray(out, entry);
        }
    }

    cJSON_Delete(arr);
    result = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return result;
}

/* ══════════════════════════════════════════════════════════════
 * HuggingFace: list GGUF files in a model repo
 * ══════════════════════════════════════════════════════════════ */

char *
chat_hf_files_json(const char *model_id,
    const char *hf_token, ngx_log_t *log)
{
    CURL               *curl;
    CURLcode            curlrc;
    struct curl_slist  *headers = NULL;
    models_buf_t        buf = {0};
    char                url[512];
    char                auth_header[320];
    long                http_code = 0;
    cJSON              *model_obj, *siblings, *out;
    char               *result;
    int                 i, n;

    snprintf(url, sizeof(url),
        "https://huggingface.co/api/models/%s", model_id);

    curl = curl_easy_init();
    if (!curl) return NULL;

    if (hf_token && *hf_token) {
        snprintf(auth_header, sizeof(auth_header),
            "Authorization: Bearer %s", hf_token);
        headers = curl_slist_append(headers, auth_header);
    }
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  mbuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curlrc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curlrc != CURLE_OK || http_code != 200) {
        free(buf.buf);
        return strdup("[]");
    }

    model_obj = cJSON_Parse(buf.buf ? buf.buf : "{}");
    free(buf.buf);
    if (!model_obj) return strdup("[]");

    siblings = cJSON_GetObjectItem(model_obj, "siblings");
    out      = cJSON_CreateArray();

    if (siblings && cJSON_IsArray(siblings)) {
        /* Ollama only supports GGUF — list each .gguf file individually */
        n = cJSON_GetArraySize(siblings);
        for (i = 0; i < n; i++) {
            cJSON      *s    = cJSON_GetArrayItem(siblings, i);
            cJSON      *fn   = cJSON_GetObjectItem(s, "rfilename");
            cJSON      *sz   = cJSON_GetObjectItem(s, "size");
            const char *name;
            size_t      flen;

            if (!fn || !fn->valuestring) continue;
            name = fn->valuestring;
            flen = strlen(name);

            if (flen > 5 && strcmp(name + flen - 5, ".gguf") == 0) {
                cJSON *entry = cJSON_CreateObject();
                char   pull_name[600];
                snprintf(pull_name, sizeof(pull_name),
                    "hf.co/%s:%s", model_id, name);
                cJSON_AddStringToObject(entry, "filename",  name);
                cJSON_AddNumberToObject(entry, "size",      sz ? sz->valuedouble : 0);
                cJSON_AddStringToObject(entry, "pull_name", pull_name);
                cJSON_AddStringToObject(entry, "format",    "gguf");
                cJSON_AddItemToArray(out, entry);
            }
        }
    }

    cJSON_Delete(model_obj);
    result = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return result;
}

//
//  dbmem-rembed.c
//  sqlitememory
//
//  Created by Marco Bambini on 09/02/26.
//

#include "dbmem-embed.h"
#include "sqlite-memory.h"
#ifndef DBMEM_OMIT_CURL
#include "curl/curl.h"
#else
#include "dbmem-http.h"
#endif
#include "jsmn.h"
#include <string.h>
#include <stdlib.h>

#if defined(__ANDROID__) && !defined(DBMEM_OMIT_CURL)
#include "cacert.h"
static size_t cacert_len = sizeof(cacert_pem) - 1;
#endif

#define API_URL                 "https://api.vectors.space/v1/embeddings"
#define DEFAULT_BUFFER_SIZE     (100*1024) //100KB, enough for 4096 embedding dimension without reallocation

#ifndef DBMEM_OMIT_CURL
static size_t dbmem_remote_receive_data(void *contents, size_t size, size_t nmemb, void *xdata);
static struct curl_slist *dbmem_remote_build_headers (const char *api_key);
#endif

struct dbmem_remote_engine_t {
    dbmem_context       *context;

#ifndef DBMEM_OMIT_CURL
    CURL                *curl;
    struct curl_slist   *headers;
#else
    char                *api_key;
#endif
    char                *provider;
    char                *model;

    // data buffer
    char                *data;
    size_t              data_capacity;
    size_t              data_size;

    // request buffer
    char                *request;
    size_t              request_capacity;

    // embedding buffer
    float               *embedding;
    size_t              embedding_capacity;

    // json tokens buffer
    jsmntok_t           *tokens;
    int                 tokens_capacity;

    // statistics
    int64_t             total_tokens_processed;     // Cumulative tokens processed across all calls
    int64_t             total_embeddings_generated; // Cumulative embeddings generated
};

// MARK: -

#include <stdbool.h>
#include <stddef.h>

#ifndef DBMEM_OMIT_CURL
static struct curl_slist *dbmem_remote_build_headers (const char *api_key) {
    char auth_header[512];
    struct curl_slist *headers = NULL;
    struct curl_slist *next = NULL;

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    if (!headers) return NULL;

    next = curl_slist_append(headers, "Content-Type: application/json");
    if (!next) {
        curl_slist_free_all(headers);
        return NULL;
    }
    headers = next;

    return headers;
}
#endif

static bool text_needs_json_escape (const char *text, size_t *len) {
    size_t original_len = *len;
    size_t required_len = 0;
    bool needs_escape = false;

    for (size_t i = 0; i < original_len; i++) {
        unsigned char c = (unsigned char)text[i];

        switch (c) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                required_len += 2;   // e.g. \" or \n
                needs_escape = true;
                break;

            default:
                if (c < 0x20) {
                    required_len += 6;  // \u00XX
                    needs_escape = true;
                } else {
                    required_len += 1;
                }
        }
    }

    *len = required_len;
    return needs_escape;
}

static size_t text_encode_json (char *buffer, size_t buffer_size, const char *text, size_t text_len) {
    // caller guarantees enough space
    UNUSED_PARAM(buffer_size);

    char *p = buffer;
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];

        switch (c) {
            case '"':
                *p++ = '\\';
                *p++ = '"';
                break;

            case '\\':
                *p++ = '\\';
                *p++ = '\\';
                break;

            case '\b':
                *p++ = '\\';
                *p++ = 'b';
                break;

            case '\f':
                *p++ = '\\';
                *p++ = 'f';
                break;

            case '\n':
                *p++ = '\\';
                *p++ = 'n';
                break;

            case '\r':
                *p++ = '\\';
                *p++ = 'r';
                break;

            case '\t':
                *p++ = '\\';
                *p++ = 't';
                break;

            default:
                if (c < 0x20) {
                    /* encode as \u00XX */
                    static const char hex[] = "0123456789abcdef";
                    *p++ = '\\';
                    *p++ = 'u';
                    *p++ = '0';
                    *p++ = '0';
                    *p++ = hex[(c >> 4) & 0xF];
                    *p++ = hex[c & 0xF];
                } else {
                    *p++ = c;
                }
        }
    }

    *p = '\0';

    return (size_t)(p - buffer);
}

static int set_json_error_message (dbmem_remote_engine_t *engine) {
    // try to extract "message" from {"error":{"message":"..."}}
    const char *errmsg = "Unknown API error";

    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);

    int ntokens = jsmn_parse(&parser, engine->data, engine->data_size, tokens, 16);
    for (int i = 0; i < ntokens - 1; i++) {
        if (tokens[i].type == JSMN_STRING && tokens[i].end - tokens[i].start == 7 && memcmp(engine->data + tokens[i].start, "message", 7) == 0) {
            jsmntok_t *val = &tokens[i + 1];
            engine->data[val->end] = '\0';
            errmsg = engine->data + val->start;
            break;
        }
    }

    dbmem_context_set_error(engine->context, errmsg);
    return -1;
}

static int dbmem_json_skip_token (const jsmntok_t *tokens, int index) {
    int next = index + 1;

    if (tokens[index].type == JSMN_ARRAY) {
        for (int i = 0; i < tokens[index].size; i++) {
            next = dbmem_json_skip_token(tokens, next);
        }
        return next;
    }

    if (tokens[index].type == JSMN_OBJECT) {
        for (int i = 0; i < tokens[index].size; i++) {
            next += 1; // skip key token
            next = dbmem_json_skip_token(tokens, next);
        }
        return next;
    }

    return next;
}

static bool dbmem_json_token_equals (const char *json, const jsmntok_t *token, const char *text) {
    size_t len = strlen(text);
    size_t token_len = (size_t)(token->end - token->start);
    return token_len == len && memcmp(json + token->start, text, len) == 0;
}

static int dbmem_json_object_find (const char *json, const jsmntok_t *tokens, int object_index, const char *key) {
    if (object_index < 0 || tokens[object_index].type != JSMN_OBJECT) return -1;

    int index = object_index + 1;
    for (int i = 0; i < tokens[object_index].size; i++) {
        int key_index = index;
        int value_index = key_index + 1;

        if (tokens[key_index].type != JSMN_STRING) return -1;
        if (dbmem_json_token_equals(json, &tokens[key_index], key)) return value_index;

        index = dbmem_json_skip_token(tokens, value_index);
    }

    return -1;
}

static bool dbmem_json_parse_bool (const char *json, const jsmntok_t *token) {
    size_t len = (size_t)(token->end - token->start);
    return token->type == JSMN_PRIMITIVE && len == 4 && memcmp(json + token->start, "true", 4) == 0;
}

#if ENABLE_DBMEM_DEBUG_EMBEDDING
static void dbmem_remote_debug_log_response(dbmem_remote_engine_t *engine, long http_code) {
    const char *response = engine->data ? engine->data : "";
    DEBUG_DBMEM_ALWAYS("[dbmem-rembed] vectors.space response (HTTP %ld): %s", http_code, response);
}
#endif

// MARK: -

dbmem_remote_engine_t *dbmem_remote_engine_init (void *ctx, const char *provider, const char *model, char err_msg[DBMEM_ERRBUF_SIZE]) {
    const char *api_key = dbmem_context_apikey((dbmem_context *)ctx);
    if (!api_key) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "memory_set_apikey must be called before requesting remote embedding");
        return NULL;
    }

    dbmem_remote_engine_t *engine = (dbmem_remote_engine_t *)dbmemory_zeroalloc(sizeof(dbmem_remote_engine_t));
    if (!engine) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the remote embedding engine");
        return NULL;
    }

    // init internal buffers (data and request)
    char *data = dbmemory_alloc(DEFAULT_BUFFER_SIZE);
    if (!data) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the default buffer (1)");
        dbmem_remote_engine_free(engine);
        return NULL;
    }

    char *request = dbmemory_alloc(DEFAULT_BUFFER_SIZE);
    if (!request) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the default buffer (2)");
        dbmem_remote_engine_free(engine);
        dbmemory_free(data);
        return NULL;
    }

    // duplicate provider and model
    char *_provider = dbmem_strdup(provider);
    if (!_provider) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate provider name (insufficient memory)");
        dbmem_remote_engine_free(engine);
        dbmemory_free(request);
        dbmemory_free(data);
        return NULL;
    }

    char *_model = dbmem_strdup(model);
    if (!_model) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate model name (insufficient memory)");
        dbmem_remote_engine_free(engine);
        dbmemory_free(request);
        dbmemory_free(data);
        dbmemory_free(_provider);
        return NULL;
    }

#ifndef DBMEM_OMIT_CURL
    // set up curl
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to initialize curl");
        dbmemory_free(data);
        dbmemory_free(request);
        dbmemory_free(_provider);
        dbmemory_free(_model);
        dbmem_remote_engine_free(engine);
        return NULL;
    }

    // set PEM
    #ifdef __ANDROID__
    struct curl_blob pem_blob = {
        .data = (void *)cacert_pem,
        .len = cacert_len,
        .flags = CURL_BLOB_NOCOPY
    };
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &pem_blob);
    #endif

    // set up headers
    struct curl_slist *headers = dbmem_remote_build_headers(api_key);
    if (!headers) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate HTTP headers");
        curl_easy_cleanup(curl);
        dbmemory_free(data);
        dbmemory_free(request);
        dbmemory_free(_provider);
        dbmemory_free(_model);
        dbmem_remote_engine_free(engine);
        return NULL;
    }

    engine->curl = curl;
    engine->headers = headers;
#else
    // NSURLSession path: just store the API key
    char *_api_key = dbmem_strdup(api_key);
    if (!_api_key) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate API key (insufficient memory)");
        dbmemory_free(data);
        dbmemory_free(request);
        dbmemory_free(_provider);
        dbmemory_free(_model);
        dbmem_remote_engine_free(engine);
        return NULL;
    }
    engine->api_key = _api_key;
#endif

    engine->context = (dbmem_context *)ctx;
    engine->provider = _provider;
    engine->model = _model;
    engine->data = data;
    engine->request = request;
    engine->data_capacity = DEFAULT_BUFFER_SIZE;
    engine->request_capacity = DEFAULT_BUFFER_SIZE;

#ifndef DBMEM_OMIT_CURL
    // set static curl options (only POSTFIELDS changes per call)
    curl_easy_setopt(curl, CURLOPT_URL, API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dbmem_remote_receive_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)engine);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
#endif

    return engine;
}

#ifndef DBMEM_OMIT_CURL
static size_t dbmem_remote_receive_data (void *contents, size_t size, size_t nmemb, void *xdata) {
    dbmem_remote_engine_t *engine = (dbmem_remote_engine_t *)xdata;
    size_t real_size = size * nmemb;

    // grow buffer if needed (+1 for null terminator)
    size_t required = engine->data_size + real_size + 1;
    if (required > engine->data_capacity) {
        size_t new_capacity = required * 2;
        char *new_data = dbmemory_alloc(new_capacity);
        if (!new_data) return 0;
        memcpy(new_data, engine->data, engine->data_size);
        dbmemory_free(engine->data);
        engine->data = new_data;
        engine->data_capacity = new_capacity;
    }

    memcpy(engine->data + engine->data_size, contents, real_size);
    engine->data_size += real_size;
    engine->data[engine->data_size] = '\0';

    return real_size;
}
#endif

int dbmem_remote_compute_embedding (dbmem_remote_engine_t *engine, const char *text, int text_len, embedding_result_t *result) {
    // reset data buffer
    engine->data_size = 0;

    // check if text needs to be encoded
    size_t len = (size_t)text_len;
    bool encoding_needed = text_needs_json_escape(text, &len);

    // check if request buffer is big enough
    size_t provider_len = strlen(engine->provider);
    size_t model_len = strlen(engine->model);
    if (engine->request_capacity < len + provider_len + model_len + 128) {
        size_t new_size = len + provider_len + model_len + 1024;
        char *new_request = dbmemory_alloc(new_size);
        if (!new_request) {
            dbmem_context_set_error(engine->context, "Unable to allocate request buffer");
            return -1;
        }

        dbmemory_free(engine->request);
        engine->request = new_request;
        engine->request_capacity = new_size;
    }

    // build request
    if (encoding_needed) {
        int seek = snprintf(engine->request, engine->request_capacity, "{\"provider\": \"%s\", \"model\": \"%s\", \"input\": \"", engine->provider, engine->model);
        if (seek < 0 || (size_t)seek >= engine->request_capacity) {
            dbmem_context_set_error(engine->context, "Request buffer too small for provider/model header");
            return -1;
        }
        size_t seek2 = text_encode_json(engine->request + seek, engine->request_capacity - (size_t)seek, text, text_len);
        snprintf(engine->request + seek + seek2, engine->request_capacity - (size_t)seek - seek2, "\",\"strategy\": {\"type\": \"truncate\"}}");
    } else {
        snprintf(engine->request, engine->request_capacity,
                 "{"
                 "\"provider\": \"%s\","
                 "\"model\": \"%s\","
                 "\"input\": \"%s\","
                 "\"strategy\": {\"type\": \"truncate\"}"
                 "}",
                 engine->provider, engine->model, text
        );
    }

    // perform REST API request
#ifndef DBMEM_OMIT_CURL
    curl_easy_setopt(engine->curl, CURLOPT_POSTFIELDS, engine->request);
    CURLcode res = curl_easy_perform(engine->curl);
    if (res != CURLE_OK) {
        dbmem_context_set_error(engine->context, curl_easy_strerror(res));
        return -1;
    }

    // check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(engine->curl, CURLINFO_RESPONSE_CODE, &http_code);
#else
    void *response_data = NULL;
    size_t response_size = 0;
    long http_code = 0;
    char http_err[DBMEM_ERRBUF_SIZE];

    int rc = dbmem_http_post(API_URL, engine->api_key, engine->request,
                             &response_data, &response_size, &http_code,
                             http_err, sizeof(http_err));
    if (rc != 0) {
        dbmem_context_set_error(engine->context, http_err);
        return -1;
    }

    // copy response into engine's data buffer
    if (response_size + 1 > engine->data_capacity) {
        size_t new_capacity = (response_size + 1) * 2;
        char *new_data = dbmemory_alloc(new_capacity);
        if (!new_data) {
            sqlite3_free(response_data);
            dbmem_context_set_error(engine->context, "Unable to allocate response buffer");
            return -1;
        }
        dbmemory_free(engine->data);
        engine->data = new_data;
        engine->data_capacity = new_capacity;
    }
    memcpy(engine->data, response_data, response_size);
    engine->data_size = response_size;
    engine->data[engine->data_size] = '\0';
    sqlite3_free(response_data);
#endif

#if ENABLE_DBMEM_DEBUG_EMBEDDING
    dbmem_remote_debug_log_response(engine, http_code);
#endif

    if (http_code != 200) {
        return set_json_error_message(engine);
    }

    // parse successful response (two-pass: count tokens, then parse)
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, engine->data, engine->data_size, NULL, 0);
    if (ntokens < 1) {
        dbmem_context_set_error(engine->context, "Failed to parse API response");
        return -1;
    }

    // grow tokens buffer if needed
    if (engine->tokens_capacity < ntokens) {
        if (engine->tokens) dbmemory_free(engine->tokens);
        engine->tokens = (jsmntok_t *)dbmemory_alloc(sizeof(jsmntok_t) * ntokens);
        if (!engine->tokens) {
            dbmem_context_set_error(engine->context, "Unable to allocate JSON tokens");
            return -1;
        }
        engine->tokens_capacity = ntokens;
    }

    jsmn_init(&parser);
    jsmn_parse(&parser, engine->data, engine->data_size, engine->tokens, ntokens);
    jsmntok_t *tokens = engine->tokens;

    // extract fields
    int n_embd = 0;
    int prompt_tokens = 0;
    int estimated_prompt_tokens = 0;
    int exact_prompt_tokens = 0;
    bool truncated = false;
    int emb_start = -1;
    size_t emb_count = 0;

    if (tokens[0].type != JSMN_OBJECT) {
        dbmem_context_set_error(engine->context, "Invalid API response shape");
        return -1;
    }

    int output_dimension_index = dbmem_json_object_find(engine->data, tokens, 0, "output_dimension");
    if (output_dimension_index >= 0 && tokens[output_dimension_index].type == JSMN_PRIMITIVE) {
        n_embd = atoi(engine->data + tokens[output_dimension_index].start);
    }

    int data_index = dbmem_json_object_find(engine->data, tokens, 0, "data");
    if (data_index < 0 || tokens[data_index].type != JSMN_ARRAY || tokens[data_index].size <= 0) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }

    int item_index = data_index + 1;
    if (tokens[item_index].type != JSMN_OBJECT) {
        dbmem_context_set_error(engine->context, "Invalid embedding item in API response");
        return -1;
    }

    int embedding_index = dbmem_json_object_find(engine->data, tokens, item_index, "embedding");
    if (embedding_index < 0 || tokens[embedding_index].type != JSMN_ARRAY) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }
    if (tokens[embedding_index].size <= 0) {
        dbmem_context_set_error(engine->context, "Invalid embedding array size in API response");
        return -1;
    }
    emb_count = (size_t)tokens[embedding_index].size;
    emb_start = embedding_index + 1;

    int truncated_index = dbmem_json_object_find(engine->data, tokens, item_index, "truncated");
    if (truncated_index >= 0) {
        truncated = dbmem_json_parse_bool(engine->data, &tokens[truncated_index]);
    }

    int usage_index = dbmem_json_object_find(engine->data, tokens, 0, "usage");
    if (usage_index >= 0 && tokens[usage_index].type == JSMN_OBJECT) {
        int prompt_tokens_index = dbmem_json_object_find(engine->data, tokens, usage_index, "prompt_tokens");
        if (prompt_tokens_index >= 0 && tokens[prompt_tokens_index].type == JSMN_PRIMITIVE) {
            prompt_tokens = atoi(engine->data + tokens[prompt_tokens_index].start);
        }

        int exact_prompt_tokens_index = dbmem_json_object_find(engine->data, tokens, usage_index, "exact_prompt_tokens");
        if (exact_prompt_tokens_index >= 0 && tokens[exact_prompt_tokens_index].type == JSMN_PRIMITIVE) {
            exact_prompt_tokens = atoi(engine->data + tokens[exact_prompt_tokens_index].start);
        }

        int estimated_prompt_tokens_index = dbmem_json_object_find(engine->data, tokens, usage_index, "estimated_prompt_tokens");
        if (estimated_prompt_tokens_index >= 0 && tokens[estimated_prompt_tokens_index].type == JSMN_PRIMITIVE) {
            estimated_prompt_tokens = atoi(engine->data + tokens[estimated_prompt_tokens_index].start);
        }
    }

    // Some providers do not return output_dimension; fallback to embedding array length.
    if (n_embd == 0 && emb_count > 0) {
        n_embd = (int)emb_count;
    }

    if (emb_start < 0 || emb_count == 0 || n_embd == 0) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }

    // allocate/grow embedding buffer
    if (engine->embedding_capacity < emb_count) {
        if (engine->embedding) dbmemory_free(engine->embedding);
        engine->embedding = (float *)dbmemory_alloc(sizeof(float) * emb_count);
        if (!engine->embedding) {
            dbmem_context_set_error(engine->context, "Unable to allocate embedding buffer");
            return -1;
        }
        engine->embedding_capacity = emb_count;
    }

    // parse embedding floats
    for (size_t i = 0; i < emb_count; i++) {
        engine->embedding[i] = strtof(engine->data + tokens[emb_start + i].start, NULL);
    }

    // Fill result
    result->n_embd = n_embd;
    result->n_tokens = exact_prompt_tokens > 0 ? exact_prompt_tokens : (estimated_prompt_tokens > 0 ? estimated_prompt_tokens : prompt_tokens);
    result->truncated = truncated;
    result->embedding = engine->embedding;

    // Update statistics
    engine->total_tokens_processed += result->n_tokens;
    engine->total_embeddings_generated++;

    return 0;
}

int dbmem_remote_engine_set_apikey (dbmem_remote_engine_t *engine, const char *api_key, char err_msg[DBMEM_ERRBUF_SIZE]) {
    if (!engine || !api_key) {
        if (err_msg) snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Invalid remote engine or API key");
        return SQLITE_MISUSE;
    }

#ifndef DBMEM_OMIT_CURL
    struct curl_slist *headers = dbmem_remote_build_headers(api_key);
    if (!headers) {
        if (err_msg) snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate HTTP headers");
        return SQLITE_NOMEM;
    }

    curl_easy_setopt(engine->curl, CURLOPT_HTTPHEADER, headers);
    if (engine->headers) curl_slist_free_all(engine->headers);
    engine->headers = headers;
#else
    char *copy = dbmem_strdup(api_key);
    if (!copy) {
        if (err_msg) snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate API key (insufficient memory)");
        return SQLITE_NOMEM;
    }

    if (engine->api_key) dbmemory_free(engine->api_key);
    engine->api_key = copy;
#endif

    return SQLITE_OK;
}

void dbmem_remote_engine_free (dbmem_remote_engine_t *engine) {
    if (!engine) return;

#ifndef DBMEM_OMIT_CURL
    if (engine->headers) curl_slist_free_all(engine->headers);
    if (engine->curl) curl_easy_cleanup(engine->curl);
#else
    if (engine->api_key) dbmemory_free(engine->api_key);
#endif
    if (engine->provider) dbmemory_free(engine->provider);
    if (engine->model) dbmemory_free(engine->model);
    if (engine->data) dbmemory_free(engine->data);
    if (engine->request) dbmemory_free(engine->request);
    if (engine->embedding) dbmemory_free(engine->embedding);
    if (engine->tokens) dbmemory_free(engine->tokens);
    dbmemory_free(engine);
}

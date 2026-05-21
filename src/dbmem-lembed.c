//
//  dbmem-lembed.c
//  sqlitememory
//
//  Created by Marco Bambini on 04/02/26.
//

#include "dbmem-embed.h"
#include "sqlite-memory.h"
#include "llama.h"
#include "ggml.h"

#include <math.h>
#include <string.h>

#define DBMEM_LOCAL_MIN_CONTEXT_TOKENS 128
#define DBMEM_LOCAL_MAX_CONTEXT_TOKENS 8192

#if defined(_MSC_VER)
#define DBMEM_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define DBMEM_THREAD_LOCAL _Thread_local
#else
#define DBMEM_THREAD_LOCAL __thread
#endif

static DBMEM_THREAD_LOCAL bool dbmem_llama_diag_enabled = false;
static DBMEM_THREAD_LOCAL char dbmem_llama_diag[DBMEM_ERRBUF_SIZE];
static DBMEM_THREAD_LOCAL size_t dbmem_llama_diag_len = 0;

struct dbmem_local_engine_t {
    dbmem_context               *context;
    
    // Model and context
    struct llama_model          *model;                     // Loaded GGUF model weights and architecture
    struct llama_context        *ctx;                       // Inference context with KV cache and compute buffers
    const struct llama_vocab    *vocab;                     // Tokenizer vocabulary for text-to-token conversion
    enum llama_pooling_type     pooling;                    // Pooling strategy (NONE, MEAN, CLS, LAST, RANK)
    llama_memory_t              mem;                        // KV cache memory handle for clearing between batches

    // Model info
    int                         n_embd;                     // Embedding dimension (e.g., 768 for nomic-embed)
    int                         n_ctx;                      // Maximum context length in tokens
    int                         n_ubatch;                   // Maximum physical batch size for encoder input
    bool                        is_encoder_only;            // True for BERT-style models, false for GPT-style

    // Settings
    bool                        normalize;                  // Whether to L2 normalize output embeddings

    // Reusable buffers (avoid repeated allocations)
    llama_token                 *tokens;                    // Pre-allocated buffer for tokenized input
    int                         tokens_capacity;            // Size of tokens buffer, capped by n_ubatch
    struct llama_batch          batch;                      // Pre-allocated llama.cpp batch with sequence metadata
    bool                        batch_initialized;          // True when batch must be freed
    float                       *embedding;                 // Pre-allocated buffer for output embedding (n_embd floats)

    // Statistics
    int64_t                     total_tokens_processed;     // Cumulative tokens processed across all calls
    int64_t                     total_embeddings_generated; // Cumulative embeddings generated
};

// MARK: -

// L2 normalize an embedding vector (with loop unrolling for better pipelining)
static void dbmem_embedding_normalize (float *vec, int n) {
    float sum = 0.0f;
    int i = 0;

    // Process 4 elements at a time for better CPU pipelining
    for (; i + 3 < n; i += 4) {
        sum += vec[i] * vec[i] + vec[i+1] * vec[i+1] +
               vec[i+2] * vec[i+2] + vec[i+3] * vec[i+3];
    }
    // Handle remainder
    for (; i < n; i++) {
        sum += vec[i] * vec[i];
    }

    float norm = sqrtf(sum);
    if (norm > 1e-12f) {
        float inv_norm = 1.0f / norm;
        i = 0;
        for (; i + 3 < n; i += 4) {
            vec[i] *= inv_norm;
            vec[i+1] *= inv_norm;
            vec[i+2] *= inv_norm;
            vec[i+3] *= inv_norm;
        }
        for (; i < n; i++) {
            vec[i] *= inv_norm;
        }
    }
}

void dbmem_logger (enum ggml_log_level level, const char *text, void *user_data) {
    UNUSED_PARAM(user_data);
    if (!dbmem_llama_diag_enabled || !text) return;

    if (level == GGML_LOG_LEVEL_WARN || level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_CONT) {
        size_t remaining = sizeof(dbmem_llama_diag) - dbmem_llama_diag_len;
        if (remaining > 1) {
            int written = snprintf(dbmem_llama_diag + dbmem_llama_diag_len, remaining, "%s", text);
            if (written > 0) {
                size_t used = (size_t)written;
                if (used >= remaining) {
                    dbmem_llama_diag_len = sizeof(dbmem_llama_diag) - 1;
                } else {
                    dbmem_llama_diag_len += used;
                }
            }
        }
    }
}

// MARK: -

static void dbmem_llama_diag_begin(void) {
    dbmem_llama_diag[0] = 0;
    dbmem_llama_diag_len = 0;
    dbmem_llama_diag_enabled = true;
}

static void dbmem_llama_diag_end(void) {
    dbmem_llama_diag_enabled = false;
}

static const char *dbmem_llama_diag_message(void) {
    return dbmem_llama_diag[0] ? dbmem_llama_diag : NULL;
}

static void dbmem_local_set_error(dbmem_local_engine_t *engine, const char *message) {
    if (!engine || !engine->context) return;
    dbmem_context_set_error(engine->context, message);
}

static bool dbmem_local_batch_prepare(dbmem_local_engine_t *engine, int n_tokens) {
    if (!engine || !engine->batch_initialized) return false;
    if (n_tokens <= 0 || n_tokens > engine->tokens_capacity) return false;

    engine->batch.n_tokens = 0;
    for (int i = 0; i < n_tokens; i++) {
        engine->batch.token[i] = engine->tokens[i];
        engine->batch.pos[i] = i;
        engine->batch.n_seq_id[i] = 1;
        engine->batch.seq_id[i][0] = 0;
        engine->batch.logits[i] = 1;
        engine->batch.n_tokens++;
    }

    return true;
}

static int dbmem_local_process_batch(dbmem_local_engine_t *engine) {
    if (engine->is_encoder_only) {
        return llama_encode(engine->ctx, engine->batch);
    }
    return llama_decode(engine->ctx, engine->batch);
}

dbmem_local_engine_t *dbmem_local_engine_init (void *ctx, const char *model_path, int max_context_tokens, char err_msg[DBMEM_ERRBUF_SIZE]) {
    dbmem_local_engine_t *engine = (dbmem_local_engine_t *)dbmemory_zeroalloc(sizeof(dbmem_local_engine_t));
    if (!engine) return NULL;
    engine->context = (dbmem_context *)ctx;
    
    // set logger
    llama_log_set(dbmem_logger, NULL);
    dbmem_llama_diag_begin();

    // Initialize backend
    llama_backend_init();

    // Load model
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;
    model_params.split_mode = LLAMA_SPLIT_MODE_NONE;
    model_params.main_gpu = -1;
    engine->model = llama_model_load_from_file(model_path, model_params);
    if (!engine->model) {
        const char *diag = dbmem_llama_diag_message();
        if (diag) {
            snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to load model: %s: %s", model_path, diag);
        } else {
            snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to load model: %s", model_path);
        }
        goto cleanup;
    }

    // Get model's native context length
    int n_ctx_train = llama_model_n_ctx_train(engine->model);
    int n_ctx = max_context_tokens * 4;
    if (n_ctx < DBMEM_LOCAL_MIN_CONTEXT_TOKENS) n_ctx = DBMEM_LOCAL_MIN_CONTEXT_TOKENS;
    if (n_ctx > DBMEM_LOCAL_MAX_CONTEXT_TOKENS) n_ctx = DBMEM_LOCAL_MAX_CONTEXT_TOKENS;
    if (n_ctx_train > 0 && n_ctx > n_ctx_train) n_ctx = n_ctx_train;

    // Create context
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.embeddings = true;
    ctx_params.n_ctx = n_ctx;
    ctx_params.n_batch = n_ctx;
    ctx_params.n_ubatch = n_ctx;
    ctx_params.offload_kqv = false;
    ctx_params.op_offload = false;

    engine->ctx = llama_init_from_model(engine->model, ctx_params);
    if (!engine->ctx) {
        const char *diag = dbmem_llama_diag_message();
        if (diag) {
            snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to create context: %s", diag);
        } else {
            snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to create context");
        }
        goto cleanup;
    }

    // Get model info
    engine->vocab = llama_model_get_vocab(engine->model);
    engine->n_embd = llama_model_n_embd_out(engine->model);
    engine->n_ctx = llama_n_ctx(engine->ctx);
    engine->n_ubatch = llama_n_ubatch(engine->ctx);
    engine->pooling = llama_pooling_type(engine->ctx);
    engine->mem = llama_get_memory(engine->ctx);

    // Show model architecture info
    bool has_encoder = llama_model_has_encoder(engine->model);
    bool has_decoder = llama_model_has_decoder(engine->model);
    engine->is_encoder_only = has_encoder && !has_decoder;

    // Debug
    // printf("[INFO] Architecture: %s\n", engine->is_encoder_only ? "encoder-only (BERT-style)" : (has_encoder && has_decoder ? "encoder-decoder" : "decoder-only (GPT-style)"));
    // printf("[INFO] Embedding dimension: %d\n", engine->n_embd);
    // printf("[INFO] Max context: %d tokens\n", engine->n_ctx);

    // Allocate token buffer
    engine->tokens_capacity = engine->n_ctx;
    if (engine->n_ubatch > 0 && engine->tokens_capacity > engine->n_ubatch) {
        engine->tokens_capacity = engine->n_ubatch;
    }
    engine->tokens = (llama_token *)dbmemory_alloc(sizeof(llama_token) * engine->tokens_capacity);
    if (!engine->tokens) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate token buffer");
        goto cleanup;
    }

    engine->batch = llama_batch_init(engine->tokens_capacity, 0, 1);
    engine->batch_initialized = true;
    if (!engine->batch.token || !engine->batch.pos || !engine->batch.n_seq_id || !engine->batch.seq_id || !engine->batch.logits) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate llama batch");
        goto cleanup;
    }

    // Allocate single embedding buffer
    engine->embedding = (float *)dbmemory_alloc(sizeof(float) * engine->n_embd);
    if (!engine->embedding) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate embedding buffer");
        goto cleanup;
    }

    // Default settings
    engine->normalize = true;  // L2 normalize by default
    engine->total_tokens_processed = 0;
    engine->total_embeddings_generated = 0;

    dbmem_llama_diag_end();
    return engine;

cleanup:
    dbmem_llama_diag_end();
    dbmem_local_engine_free(engine);
    return NULL;
}

bool dbmem_local_engine_warmup (dbmem_local_engine_t *engine) {
    // Pre-warm the model by running a dummy inference
    // This ensures all Metal/GPU shaders are compiled before actual use

    const char *warmup_text = "Warmup";
    int warmup_tokens = llama_tokenize(engine->vocab, warmup_text, (int32_t)strlen(warmup_text), engine->tokens, engine->tokens_capacity, true, true);
    if (warmup_tokens > 0 && dbmem_local_batch_prepare(engine, warmup_tokens)) {
        dbmem_local_process_batch(engine);

        if (engine->mem != NULL) {
            llama_memory_clear(engine->mem, true);
        }
    }

    return true;
}

int dbmem_local_compute_embedding (dbmem_local_engine_t *engine, const char *text, int text_len, embedding_result_t *result) {
    memset(result, 0, sizeof(embedding_result_t));
    if (text_len == -1) text_len = (int)strlen(text);
    if (text_len == 0) return 0;

    bool truncated = false;

    // Tokenize
    int n_tokens = llama_tokenize(engine->vocab, text, text_len, engine->tokens, engine->tokens_capacity, true, true);
    if (n_tokens < 0) {
        int needed = -n_tokens;
        if (needed <= 0) {
            dbmem_local_set_error(engine, "Tokenization failed");
            return -1;
        }

        llama_token *all_tokens = (llama_token *)dbmemory_alloc(sizeof(llama_token) * needed);
        if (!all_tokens) {
            dbmem_local_set_error(engine, "Failed to allocate token overflow buffer");
            return -1;
        }

        int full_tokens = llama_tokenize(engine->vocab, text, text_len, all_tokens, needed, true, true);
        if (full_tokens < 0) {
            dbmemory_free(all_tokens);
            dbmem_local_set_error(engine, "Tokenization failed");
            return -1;
        }

        n_tokens = engine->tokens_capacity;
        memcpy(engine->tokens, all_tokens, sizeof(llama_token) * n_tokens);
        dbmemory_free(all_tokens);
        truncated = true;
    }

    // Handle token overflow: truncate to max context size
    if (n_tokens > engine->tokens_capacity) {
        truncated = true;
        n_tokens = engine->tokens_capacity;
    }

    if (!dbmem_local_batch_prepare(engine, n_tokens)) {
        dbmem_local_set_error(engine, "Failed to prepare llama batch");
        return -1;
    }

    // Clear memory
    if (engine->mem != NULL) {
        llama_memory_clear(engine->mem, true);
    }

    // Encode
    int ret = dbmem_local_process_batch(engine);
    if (ret != 0) {
        dbmem_local_set_error(engine, "llama batch processing failed");
        return -1;
    }

    // Get embeddings
    const float *emb_ptr = NULL;
    if (engine->pooling == LLAMA_POOLING_TYPE_NONE) {
        emb_ptr = llama_get_embeddings_ith(engine->ctx, n_tokens - 1);
    } else {
        emb_ptr = llama_get_embeddings_seq(engine->ctx, 0);
    }

    if (!emb_ptr) {
        dbmem_local_set_error(engine, "Failed to get embeddings");
        return -1;
    }

    // Copy embedding to engine buffer
    memcpy(engine->embedding, emb_ptr, sizeof(float) * engine->n_embd);

    // Normalize if enabled
    if (engine->normalize) {
        dbmem_embedding_normalize(engine->embedding, engine->n_embd);
    }

    // Fill result
    result->n_tokens = n_tokens;
    result->truncated = truncated;
    result->n_embd = engine->n_embd;
    result->embedding = engine->embedding;

    // Update statistics
    engine->total_tokens_processed += n_tokens;
    engine->total_embeddings_generated++;

    return 0;
}

void dbmem_local_engine_free (dbmem_local_engine_t *engine) {
    if (!engine) return;

    if (engine->embedding) {
        dbmemory_free(engine->embedding);
        engine->embedding = NULL;
    }
    if (engine->tokens) {
        dbmemory_free(engine->tokens);
        engine->tokens = NULL;
    }
    if (engine->batch_initialized) {
        llama_batch_free(engine->batch);
        memset(&engine->batch, 0, sizeof(engine->batch));
        engine->batch_initialized = false;
    }
    if (engine->ctx) {
        llama_free(engine->ctx);
        engine->ctx = NULL;
    }
    if (engine->model) {
        llama_model_free(engine->model);
        engine->model = NULL;
    }
    
    llama_backend_free();
    dbmemory_free(engine);
}

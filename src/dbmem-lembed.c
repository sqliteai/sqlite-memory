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
    bool                        is_encoder_only;            // True for BERT-style models, false for GPT-style

    // Settings
    bool                        normalize;                  // Whether to L2 normalize output embeddings

    // Reusable buffers (avoid repeated allocations)
    llama_token                 *tokens;                    // Pre-allocated buffer for tokenized input
    int                         tokens_capacity;            // Size of tokens buffer (equals n_ctx)
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
    dbmem_local_engine_t *engine = (dbmem_local_engine_t *)user_data;
    //if (ai->db == NULL) return;
    //if ((level == GGML_LOG_LEVEL_INFO) && (ai->options.log_info == false)) return;
    
    const char *type = NULL;
    switch (level) {
        case GGML_LOG_LEVEL_NONE: type = "NONE"; break;
        case GGML_LOG_LEVEL_DEBUG: type = "DEBUG"; break;
        case GGML_LOG_LEVEL_INFO: type = "INFO"; break;
        case GGML_LOG_LEVEL_WARN: type = "WARNING"; break;
        case GGML_LOG_LEVEL_ERROR: type = "ERROR"; break;
        case GGML_LOG_LEVEL_CONT: type = NULL; break;
    }
    
    // DEBUG
    // printf("%s %s\n", type, text);
    
    //const char *values[] = {type, text};
    //int types[] = {(type == NULL) ? SQLITE_NULL : SQLITE_TEXT, SQLITE_TEXT};
    //int lens[] = {-1, -1};
    //sqlite_db_write(NULL, ai->db, LOG_TABLE_INSERT_STMT, values, types, lens, 2);
}

// MARK: -

dbmem_local_engine_t *dbmem_local_engine_init (void *ctx, const char *model_path, char err_msg[DBMEM_ERRBUF_SIZE]) {
    dbmem_local_engine_t *engine = (dbmem_local_engine_t *)dbmem_zeroalloc(sizeof(dbmem_local_engine_t));
    if (!engine) return NULL;
    
    // set logger
    llama_log_set(dbmem_logger, engine);

    // Initialize backend
    llama_backend_init();

    // Load model
    struct llama_model_params model_params = llama_model_default_params();
    engine->model = llama_model_load_from_file(model_path, model_params);
    if (!engine->model) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to load model: %s", model_path);
        goto cleanup;
    }

    // Get model's native context length
    int n_ctx_train = llama_model_n_ctx_train(engine->model);

    // Create context
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.embeddings = true;
    ctx_params.n_ctx = n_ctx_train;
    ctx_params.n_batch = n_ctx_train;
    ctx_params.n_ubatch = n_ctx_train;

    engine->ctx = llama_init_from_model(engine->model, ctx_params);
    if (!engine->ctx) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to create context");
        goto cleanup;
    }

    // Get model info
    engine->vocab = llama_model_get_vocab(engine->model);
    engine->n_embd = llama_model_n_embd(engine->model);
    engine->n_ctx = llama_n_ctx(engine->ctx);
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
    engine->tokens = (llama_token *)dbmem_alloc(sizeof(llama_token) * engine->tokens_capacity);
    if (!engine->tokens) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate token buffer");
        goto cleanup;
    }

    // Allocate single embedding buffer
    engine->embedding = (float *)dbmem_alloc(sizeof(float) * engine->n_embd);
    if (!engine->embedding) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Failed to allocate embedding buffer");
        goto cleanup;
    }

    // Default settings
    engine->normalize = true;  // L2 normalize by default
    engine->total_tokens_processed = 0;
    engine->total_embeddings_generated = 0;

    return engine;

cleanup:
    dbmem_local_engine_free(engine);
    return NULL;
}

bool dbmem_local_engine_warmup (dbmem_local_engine_t *engine) {
    // Pre-warm the model by running a dummy inference
    // This ensures all Metal/GPU shaders are compiled before actual use

    const char *warmup_text = "Warmup";
    int warmup_tokens = llama_tokenize(engine->vocab, warmup_text, (int32_t)strlen(warmup_text), engine->tokens, engine->tokens_capacity, true, true);
    if (warmup_tokens > 0) {
        struct llama_batch batch = {
            .n_tokens   = warmup_tokens,
            .token      = engine->tokens,
            .embd       = NULL,
            .pos        = NULL,
            .n_seq_id   = NULL,
            .seq_id     = NULL,
            .logits     = NULL,
        };
        llama_encode(engine->ctx, batch);

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

    // Tokenize
    int n_tokens = llama_tokenize(engine->vocab, text, text_len, engine->tokens, engine->tokens_capacity, true, true);
    if (n_tokens < 0) {
        dbmem_context_set_error(engine->context, "Tokenization failed (text too long?)");
        return -1;
    }

    // Handle token overflow: truncate to max context size
    int n_tokens_truncated = 0;
    if (n_tokens > engine->n_ctx) {
        n_tokens_truncated = n_tokens - engine->n_ctx;
        n_tokens = engine->n_ctx;
    }

    // Create batch
    struct llama_batch batch = {
        .n_tokens   = n_tokens,
        .token      = engine->tokens,
        .embd       = NULL,
        .pos        = NULL,
        .n_seq_id   = NULL,
        .seq_id     = NULL,
        .logits     = NULL,
    };

    // Clear memory
    if (engine->mem != NULL) {
        llama_memory_clear(engine->mem, true);
    }

    // Encode
    int ret = llama_encode(engine->ctx, batch);
    if (ret != 0) {
        dbmem_context_set_error(engine->context, "Llama_encode failed");
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
        dbmem_context_set_error(engine->context, "Failed to get embeddings");
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
    result->n_tokens_truncated = n_tokens_truncated;
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
        dbmem_free(engine->embedding);
        engine->embedding = NULL;
    }
    if (engine->tokens) {
        dbmem_free(engine->tokens);
        engine->tokens = NULL;
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
}


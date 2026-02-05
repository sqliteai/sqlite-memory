//
//  dbmem-lembed.c
//  sqlitememory
//
//  Created by Marco Bambini on 04/02/26.
//

#include "dbmem-lembed.h"
#include "llama.h"
#include "ggml.h"

#include <math.h>
#include <string.h>

#define DEFAULT_CHUNK_SIZE          256
#define DEFAULT_OVERLAP             32
#define MAX_CONTEXT_SIZE            8192
#define MAX_CHUNKS                  256             // Pre-allocate for up to this many chunks

struct dbmem_local_engine_t {
    // Model and context
    struct llama_model          *model;
    struct llama_context        *ctx;
    const struct llama_vocab    *vocab;
    enum llama_pooling_type     pooling;
    llama_memory_t              mem;
    
    // Model info
    int         n_embd;
    int         n_ctx;
    bool        is_encoder_only;
    
    // Settings
    int         chunk_size;
    int         overlap;
    bool        json_output;
    bool        normalize;              // Whether to L2 normalize embeddings
    
    // Reusable buffers (avoid repeated allocations)
    llama_token *tokens;
    int         tokens_capacity;
    
    // Pre-allocated embedding storage
    float       *embedding_pool;        // Pool for all chunk embeddings
    int         embedding_pool_size;    // Number of embeddings that fit
    
    // Statistics
    int64_t     total_tokens_processed;
    int64_t     total_embeddings_generated;
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

// Copy embedding data
static inline void dbmem_embedding_copy (float *dst, const float *src, int n) {
    memcpy(dst, src, sizeof(float) * n);
}

// Free embedding result structure
static void dbmem_embedding_free( embedding_result_t *result) {
    if (result == NULL) return;
    
    if (result->chunks != NULL) {
        // Only free individual embeddings if they weren't from the pool
        if (!result->used_pool) {
            for (int i = 0; i < result->n_chunks; i++) {
                if (result->chunks[i].embedding != NULL) {
                    dbmem_free(result->chunks[i].embedding);
                }
            }
        }
        dbmem_free(result->chunks);
        result->chunks = NULL;
    }
    result->n_chunks = 0;
    result->used_pool = false;
}

// MARK: -

dbmem_local_engine_t *dbmem_local_engine_init (const char *model_path, char err_msg[DBMEM_MAXERROR_SIZE]) {
    dbmem_local_engine_t *engine = (dbmem_local_engine_t *)dbmem_zeroalloc(sizeof(dbmem_local_engine_t));
    if (!engine) return NULL;
    
    engine->chunk_size = DEFAULT_CHUNK_SIZE;
    engine->overlap = DEFAULT_OVERLAP;
    engine->json_output = false;
    
    // Initialize backend
    llama_backend_init();
    
    // Load model
    struct llama_model_params model_params = llama_model_default_params();
    engine->model = llama_model_load_from_file(model_path, model_params);
    if (!engine->model) {
        snprintf(err_msg, DBMEM_MAXERROR_SIZE, "Failed to load model: %s", model_path);
        goto cleanup;
    }
    
    // Create context
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.embeddings = true;
    ctx_params.n_ctx = MAX_CONTEXT_SIZE;
    ctx_params.n_batch = MAX_CONTEXT_SIZE;
    ctx_params.n_ubatch = MAX_CONTEXT_SIZE;
    
    engine->ctx = llama_init_from_model(engine->model, ctx_params);
    if (!engine->ctx) {
        snprintf(err_msg, DBMEM_MAXERROR_SIZE, "Failed to create context");
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
    printf("[INFO] Architecture: %s\n", engine->is_encoder_only ? "encoder-only (BERT-style)" : (has_encoder && has_decoder ? "encoder-decoder" : "decoder-only (GPT-style)"));
    printf("[INFO] Embedding dimension: %d\n", engine->n_embd);
    printf("[INFO] Max context: %d tokens\n", engine->n_ctx);
    
    // Allocate token buffer
    engine->tokens_capacity = MAX_CONTEXT_SIZE;
    engine->tokens = (llama_token *)dbmem_alloc(sizeof(llama_token) * engine->tokens_capacity);
    if (!engine->tokens) {
        snprintf(err_msg, DBMEM_MAXERROR_SIZE, "Failed to allocate token buffer");
        goto cleanup;
    }
    
    // Pre-allocate embedding pool to avoid malloc/free per chunk
    engine->embedding_pool_size = MAX_CHUNKS;
    engine->embedding_pool = (float *)dbmem_alloc(sizeof(float) * engine->n_embd * MAX_CHUNKS);
    if (!engine->embedding_pool) {
        snprintf(err_msg, DBMEM_MAXERROR_SIZE, "Failed to allocate embedding pool");
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
        snprintf(result->err_msg, DBMEM_MAXERROR_SIZE, "Tokenization failed (text too long?)");
        return -1;
    }
    
    // Calculate chunks
    int chunk_size = engine->chunk_size;
    int overlap = engine->overlap;
    int step = chunk_size - overlap;
    if (step < 1) step = 1;
    int n_chunks = (n_tokens <= chunk_size) ? 1 : 1 + (n_tokens - chunk_size + step - 1) / step;
    
    // Check if we exceed pre-allocated pool
    bool use_pool = (n_chunks <= engine->embedding_pool_size);
    
    // Setup result
    result->total_tokens = n_tokens;
    result->total_chars = text_len;
    result->n_embd = engine->n_embd;
    result->chunk_size = chunk_size;
    result->overlap = overlap;
    result->n_chunks = n_chunks;
    result->used_pool = use_pool;
    result->chunks = (chunk_result_t *)dbmem_zeroalloc(n_chunks * sizeof(chunk_result_t));
    if (!result->chunks) {
        snprintf(result->err_msg, DBMEM_MAXERROR_SIZE, "Failed to allocate chunks");
        return -1;
    }
    
    // Calculate average chars per token for position estimation
    float avg_chars_per_token = (n_tokens > 0) ? (float)text_len / n_tokens : 1.0f;
    
    // Process each chunk
    for (int chunk_idx = 0; chunk_idx < n_chunks; chunk_idx++) {
        int token_start = chunk_idx * step;
        int token_length = chunk_size;
        
        if (token_start + token_length > n_tokens) {
            token_length = n_tokens - token_start;
        }
        
        // Create batch
        struct llama_batch batch = {
            .n_tokens   = token_length,
            .token      = &engine->tokens[token_start],
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
        
        // Always use llama_encode() for embedding generation
        int ret = llama_encode(engine->ctx, batch);
        
        if (ret != 0) {
            snprintf(result->err_msg, DBMEM_MAXERROR_SIZE, "llama_encode failed for chunk %d", chunk_idx);
            dbmem_embedding_free(result);
            return -1;
        }
        
        // Get embeddings
        const float *emb_ptr = NULL;
        if (engine->pooling == LLAMA_POOLING_TYPE_NONE) {
            emb_ptr = llama_get_embeddings_ith(engine->ctx, token_length - 1);
        } else {
            emb_ptr = llama_get_embeddings_seq(engine->ctx, 0);
        }
        
        if (!emb_ptr) {
            snprintf(result->err_msg, DBMEM_MAXERROR_SIZE, "Failed to get embeddings for chunk %d", chunk_idx);
            dbmem_embedding_free(result);
            return -1;
        }
        
        // Store result
        chunk_result_t *chunk = &result->chunks[chunk_idx];
        chunk->index = chunk_idx;
        chunk->token_start = token_start;
        chunk->token_length = token_length;
        chunk->char_start = (int)(token_start * avg_chars_per_token);
        chunk->char_length = (int)(token_length * avg_chars_per_token);
        
        if (chunk->char_start > text_len) chunk->char_start = text_len;
        if (chunk->char_start + chunk->char_length > text_len) {
            chunk->char_length = text_len - chunk->char_start;
        }
        
        // Use pre-allocated pool if possible, otherwise malloc
        if (use_pool) {
            chunk->embedding = &engine->embedding_pool[chunk_idx * engine->n_embd];
        } else {
            chunk->embedding = (float *)dbmem_alloc(sizeof(float) * engine->n_embd);
            if (!chunk->embedding) {
                snprintf(result->err_msg, DBMEM_MAXERROR_SIZE, "Failed to allocate embedding");
                dbmem_embedding_free(result);
                return -1;
            }
        }
        
        // Copy embedding (using SIMD-optimized copy)
        dbmem_embedding_copy(chunk->embedding, emb_ptr, engine->n_embd);
        
        // Normalize if enabled
        if (engine->normalize) {
            dbmem_embedding_normalize(chunk->embedding, engine->n_embd);
        }
        
        // Update statistics
        engine->total_tokens_processed += token_length;
        engine->total_embeddings_generated++;
    }
    
    return 0;
}

void dbmem_local_engine_free (dbmem_local_engine_t *engine) {
    if (!engine) return;
    
    if (engine->embedding_pool) {
        dbmem_free(engine->embedding_pool);
        engine->embedding_pool = NULL;
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

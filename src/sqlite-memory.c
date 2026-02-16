//
//  sqlite-memory.c
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "sqlite-memory.h"
#include "dbmem-utils.h"
#include "dbmem-embed.h"
#include "dbmem-parser.h"
#include "dbmem-search.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

// available compilation options
// DBMEM_OMIT_IO                                // to be used when compiled for WASM
// DBMEM_OMIT_LOCAL_ENGINE                      // to be used when compiled for WASM or when the local inference engine is not needed
// DBMEM_OMIT_REMOTE_ENGINE                     // to be used when remote provider should not be used

#define DBMEM_TYPE_VALUE                        500
#define DBMEM_LOCAL_PROVIDER                    "local"
#define DBMEM_SAVEPOINT_NAME                    "memory_transaction"

#define DBMEM_SETTINGS_KEY_PROVIDER             "provider"
#define DBMEM_SETTINGS_KEY_MODEL                "model"
#define DBMEM_SETTINGS_KEY_DIMENSION            "dimension"
#define DBMEM_SETTINGS_KEY_MAX_TOKENS           "max_tokens"
#define DBMEM_SETTINGS_KEY_OVERLAY_TOKENS       "overlay_tokens"
#define DBMEM_SETTINGS_KEY_CHARS_PER_TOKENS     "chars_per_tokens"
#define DBMEM_SETTINGS_KEY_SAVE_CONTEXT         "save_content"
#define DBMEM_SETTINGS_KEY_SKIP_SEMANTIC        "skip_semantic"
#define DBMEM_SETTINGS_KEY_SKIP_HTML            "skip_html"
#define DBMEM_SETTINGS_KEY_EXTENSIONS           "extensions"
#define DBMEM_SETTINGS_KEY_ENGINE_WARMUP        "engine_warmup"
#define DBMEM_SETTINGS_KEY_MAX_RESULTS          "max_results"
#define DBMEM_SETTINGS_KEY_FTS_ENABLED          "fts_enabled"
#define DBMEM_SETTINGS_KEY_VECTOR_WEIGHT        "vector_weight"
#define DBMEM_SETTINGS_KEY_TEXT_WEIGHT          "text_weight"
#define DBMEM_SETTINGS_KEY_MIN_SCORE            "min_score"
#define DBMEM_SETTINGS_KEY_UPDATE_ACCESS        "update_access"
#define DBMEM_SETTINGS_KEY_EMBEDDING_CACHE     "embedding_cache"
#define DBMEM_SETTINGS_KEY_CACHE_MAX_ENTRIES   "cache_max_entries"
#define DBMEM_SETTINGS_KEY_SEARCH_OVERSAMPLE  "search_oversample"

// default values from https://docs.openclaw.ai/concepts/memory
#define DEFAULT_CHARS_PER_TOKEN                 4       // Approximate number of characters per token (GPT ≈ 4, Claude ≈ 3.5)
#define DEFAULT_MAX_TOKENS                      400
#define DEFAULT_OVERLAY_TOKENS                  80
#define DEFAULT_MAX_SNIPPET_CHARS               700
#define DEFAULT_MAX_RESULTS                     20
#define DEFAULT_VECTOR_WEIGHT                   0.5
#define DEFAULT_TEXT_WEIGHT                     0.5
#define DEFAULT_MIN_SCORE                       0.7

struct dbmem_context {
    // Database and engine
    sqlite3                 *db;                // SQLite database connection
    bool                    is_local;           // Flag set based on memory_set_model
    dbmem_local_engine_t    *l_engine;          // Local embedding engine (llama.cpp based)
    dbmem_remote_engine_t   *r_engine;          // Remote embedding engine (vectors.space based)

    // Provider configuration
    char        *provider;                      // Embedding provider: "local" or remote service name
    char        *model;                         // Model path (local) or model identifier (remote)
    char        *api_key;                       // API key for remote embedding services
    char        *extensions;                    // Comma-separated file extensions to process (e.g., "md,txt")
    int         dimension;                      // Embedding dimension from provider/model (stored into the database)

    // Chunking parameters
    size_t      max_tokens;                     // Maximum tokens per chunk
    size_t      overlay_tokens;                 // Token overlap between consecutive chunks
    size_t      chars_per_tokens;               // Estimated characters per token (for size calculations)
    size_t      snippet_max_chars;              // Maximum characters for search result snippets

    // Processing flags
    bool        engine_warmup;                  // Whether engine has been warmed up (GPU shaders compiled)
    bool        save_content;                   // Whether to store original content in database
    bool        skip_semantic;                  // Skip markdown parsing, treat as raw text
    bool        skip_html;                      // Strip HTML tags when parsing markdown
    bool        perform_fts;                    // Enable/Disable FTS during search
    
    bool        vector_extension_available;     // SQLite-vector available and correctly loaded flag
    bool        sync_extension_available;       // SQLite-sync available and correctly loadedflag
    bool        dimension_saved;                // Embedding dimension needs to be automatically serialized

    // Settings
    int         max_results;                    // Maximum number of results to be returned from a search
    double      vector_weight;                  // Weight of the vector results during the merge of the result
    double      text_weight;                    // Weight of the FTS results during the merge of the result
    double      min_score;                      // Minimum score threshold to filter irrelevant results
    bool        update_access;                  // Whether to update last_accessed on search
    bool        embedding_cache;                // Enable/disable embedding cache (default: true)
    int         cache_max_entries;              // Max cache entries (0 = no limit)
    int         search_oversample;             // Search oversampling multiplier (0 = no oversampling)

    // Cache
    float       *cache_buffer;                  // Reusable buffer for cache hits
    int         cache_buffer_size;              // Allocated size in floats

    // Runtime state
    int64_t     counter;                        // Chunk counter during file processing
    uint64_t    hash;                           // Hash of the current text
    const char  *context;                       // Optional context string for current operation
    const char  *path;                          // Full path file (optional)
    char        error_msg[DBMEM_ERRBUF_SIZE];   // Error message buffer
};

static bool fts5_is_available = true;

// MARK: - Settings -

static int dbmem_settings_write (sqlite3 *db, const char *key, const char *text_value, sqlite3_int64 int_value, const sqlite3_value *sql_value, int bind_type) {
    static const char *sql = "REPLACE INTO dbmem_settings (key, value) VALUES (?1, ?2);";
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 1, key, -1, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    switch (bind_type) {
        case SQLITE_TEXT: rc = sqlite3_bind_text(vm, 2, text_value, -1, NULL); break;
        case SQLITE_INTEGER: rc = sqlite3_bind_int64(vm, 2, int_value); break;
        case DBMEM_TYPE_VALUE: rc = sqlite3_bind_value(vm, 2, sql_value); break;
        default: rc = SQLITE_MISUSE; goto cleanup;
    }
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM("Error in dbmem_settings_write: %s", sqlite3_errmsg(db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int dbmem_settings_write_text (sqlite3 *db, const char *key, const char *value) {
    return dbmem_settings_write(db, key, value, 0, NULL, SQLITE_TEXT);
}

static int dbmem_settings_write_int (sqlite3 *db, const char *key, sqlite3_int64 value) {
    return dbmem_settings_write(db, key, NULL, value, NULL, SQLITE_INTEGER);
}

static int dbmem_settings_write_value (sqlite3 *db, const char *key, sqlite3_value *value) {
    return dbmem_settings_write(db, key, NULL, 0, value, DBMEM_TYPE_VALUE);
}

static int dbmem_settings_sync (dbmem_context *ctx, const char *key, sqlite3_value *value) {
    if (!value) return 0;
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MAX_TOKENS) == 0) {
        int n = sqlite3_value_int(value);
        if (n > 0) ctx->max_tokens = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_OVERLAY_TOKENS) == 0) {
        int n = sqlite3_value_int(value);
        if (n > 0) ctx->overlay_tokens = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_CHARS_PER_TOKENS) == 0) {
        int n = sqlite3_value_int(value);
        if (n > 0) ctx->chars_per_tokens = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_DIMENSION) == 0) {
        int n = sqlite3_value_int(value);
        if (n > 0) {ctx->dimension = n; ctx->dimension_saved = true;}
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_SAVE_CONTEXT) == 0) {
        int n = sqlite3_value_int(value);
        ctx->save_content = (n > 0) ? 1 : 0;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_SKIP_SEMANTIC) == 0) {
        int n = sqlite3_value_int(value);
        ctx->skip_semantic = (n > 0) ? 1 : 0;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_SKIP_HTML) == 0) {
        int n = sqlite3_value_int(value);
        ctx->skip_html = (n > 0) ? 1 : 0;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_ENGINE_WARMUP) == 0) {
        int n = sqlite3_value_int(value);
        ctx->engine_warmup = (n > 0) ? 1 : 0;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_FTS_ENABLED) == 0) {
        int n = sqlite3_value_int(value);
        ctx->perform_fts = (n > 0) ? 1 : 0;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MAX_RESULTS) == 0) {
        int n = sqlite3_value_int(value);
        if (n > 0) ctx->max_results = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_VECTOR_WEIGHT) == 0) {
        double n = sqlite3_value_double(value);
        if (n > 0) ctx->vector_weight = n;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_TEXT_WEIGHT) == 0) {
        double n = sqlite3_value_double(value);
        if (n > 0) ctx->text_weight = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MIN_SCORE) == 0) {
        double n = sqlite3_value_double(value);
        if (n > 0) ctx->min_score = n;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_UPDATE_ACCESS) == 0) {
        int n = sqlite3_value_int(value);
        ctx->update_access = (n > 0) ? 1 : 0;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_EMBEDDING_CACHE) == 0) {
        int n = sqlite3_value_int(value);
        ctx->embedding_cache = (n > 0) ? 1 : 0;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_CACHE_MAX_ENTRIES) == 0) {
        int n = sqlite3_value_int(value);
        if (n >= 0) ctx->cache_max_entries = n;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_SEARCH_OVERSAMPLE) == 0) {
        int n = sqlite3_value_int(value);
        if (n >= 0) ctx->search_oversample = n;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_PROVIDER) == 0) {
        char *provider = dbmem_strdup((const char *)sqlite3_value_text(value));
        if (provider) {
            if (ctx->provider) dbmem_free(ctx->provider);
            ctx->provider = provider;
        }
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MODEL) == 0) {
        char *model = dbmem_strdup((const char *)sqlite3_value_text(value));
        if (model) {
            if (ctx->model) dbmem_free(ctx->model);
            ctx->model = model;
        }
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_EXTENSIONS) == 0) {
        char *extensions = dbmem_strdup((const char *)sqlite3_value_text(value));
        if (extensions) {
            if (ctx->extensions) dbmem_free(ctx->extensions);
            ctx->extensions = extensions;
        }
        return 0;
    }
    
    return 0;
}

void dbmem_settings_load (sqlite3 *db, dbmem_context *ctx) {
    const char *sql = "SELECT key, value FROM dbmem_settings;";
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    while (1) {
        // no error handling here
        rc = sqlite3_step(vm);
        if (rc != SQLITE_ROW) break;
        
        const char *key = (const char *)sqlite3_column_text(vm, 0);
        dbmem_settings_sync(ctx, key, sqlite3_column_value(vm, 1));
    }
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return;
}

// MARK: - Database -

static int dbmem_database_init (sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS dbmem_settings (key TEXT PRIMARY KEY, value TEXT);";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_content (hash INTEGER PRIMARY KEY NOT NULL, path TEXT NOT NULL UNIQUE, value TEXT DEFAULT NULL, length INTEGER NOT NULL, context TEXT DEFAULT NULL, created_at INTEGER DEFAULT 0, last_accessed INTEGER DEFAULT 0);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_vault (hash INTEGER NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, PRIMARY KEY (hash, seq));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_cache (text_hash INTEGER NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL, embedding BLOB NOT NULL, dimension INTEGER NOT NULL, PRIMARY KEY (text_hash, provider, model));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    sql = "CREATE VIRTUAL TABLE IF NOT EXISTS dbmem_vault_fts USING fts5 (content, hash UNINDEXED, seq UNINDEXED, context UNINDEXED);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fts5_is_available = false;
        rc = SQLITE_OK;
    }
    
    // explicitly allows extension loading (only when loaded dynamically)
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) return rc;

    return rc;
}

static bool dbmem_database_check_if_stored (sqlite3 *db, uint64_t hash, int64_t len) {
    static const char *sql = "SELECT length FROM dbmem_content WHERE hash=? LIMIT 1;";
    
    bool result = false;
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 1, (sqlite3_int64)hash);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    else if (rc != SQLITE_ROW) goto cleanup;
    
    // SQLITE_ROW case
    sqlite3_int64 saved_len = sqlite3_column_int64(vm, 0);
    result = (saved_len == len);
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return result;
}

static void dbmem_database_delete_hash (sqlite3 *db, sqlite3_int64 hash) {
    sqlite3_stmt *vm = NULL;
    if (fts5_is_available) {
        sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash=?1;", -1, &vm, NULL);
        sqlite3_bind_int64(vm, 1, hash);
        sqlite3_step(vm);
        sqlite3_finalize(vm);
    }
    sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash=?1;", -1, &vm, NULL);
    sqlite3_bind_int64(vm, 1, hash);
    sqlite3_step(vm);
    sqlite3_finalize(vm);

    sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash=?1;", -1, &vm, NULL);
    sqlite3_bind_int64(vm, 1, hash);
    sqlite3_step(vm);
    sqlite3_finalize(vm);
}

static void dbmem_database_delete_stale_path (sqlite3 *db, const char *path, uint64_t new_hash) {
    if (!path) return;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT hash FROM dbmem_content WHERE path=?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_bind_text(vm, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        sqlite3_int64 old_hash = sqlite3_column_int64(vm, 0);
        sqlite3_finalize(vm);
        if ((uint64_t)old_hash != new_hash) {
            dbmem_database_delete_hash(db, old_hash);
        }
    } else {
        sqlite3_finalize(vm);
    }
}

static int dbmem_database_add_entry (dbmem_context *ctx, sqlite3 *db, uint64_t hash, const char *buffer, int64_t len) {
    static const char *sql = "INSERT INTO dbmem_content (hash, path, value, length, context, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6);";

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_int64(vm, 1, (sqlite3_int64)hash);
    if (rc != SQLITE_OK) goto cleanup;

    const char *path = ctx->path;
    char uuid[DBMEM_UUID_STR_MAXLEN];
    if (path == NULL) path = dbmem_uuid_v7(uuid);
    rc = sqlite3_bind_text(vm, 2, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = (ctx->save_content) ? sqlite3_bind_text(vm, 3, buffer, (int)len, SQLITE_STATIC) : sqlite3_bind_null(vm, 3);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_int64(vm, 4, (sqlite3_int64)len);
    if (rc != SQLITE_OK) goto cleanup;

    rc = (ctx->context) ? sqlite3_bind_text(vm, 5, ctx->context, -1, SQLITE_STATIC) : sqlite3_bind_null(vm, 5);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_int64(vm, 6, (sqlite3_int64)time(NULL));
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM_ALWAYS("Error in dbmem_database_add_entry: %s", sqlite3_errmsg(ctx->db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int dbmem_database_add_chunk (dbmem_context *ctx, embedding_result_t *result, size_t offset, size_t length, size_t index) {
    static const char *sql = "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length) VALUES (?1, ?2, ?3, ?4, ?5);";
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 1, (sqlite3_int64)ctx->hash);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 2, (sqlite3_int64)index);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_blob(vm, 3, result->embedding, (int)(result->n_embd * sizeof(float)), SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 4, (sqlite3_int64)offset);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 5, (sqlite3_int64)length);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM_ALWAYS("Error in dbmem_database_add_chunk: %s", sqlite3_errmsg(ctx->db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int dbmem_database_add_fts5 (dbmem_context *ctx, const char *text, size_t text_len, size_t index) {
    static const char *sql = "INSERT INTO dbmem_vault_fts (content, hash, seq, context) VALUES (?1, ?2, ?3, ?4);";
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 1, text, (int)text_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 2, (sqlite3_int64)ctx->hash);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int64(vm, 3, (sqlite3_int64)index);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = (ctx->context) ? sqlite3_bind_text(vm, 4, ctx->context, -1, SQLITE_STATIC) : sqlite3_bind_null(vm, 4);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM_ALWAYS("Error in dbmem_database_add_fts5: %s", sqlite3_errmsg(ctx->db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int dbmem_database_begin_transaction (sqlite3 *db) {
    return sqlite3_exec(db, "SAVEPOINT " DBMEM_SAVEPOINT_NAME ";", NULL, NULL, NULL);
}

static int dbmem_database_commit_transaction (sqlite3 *db) {
    return sqlite3_exec(db, "RELEASE " DBMEM_SAVEPOINT_NAME ";", NULL, NULL, NULL);
}

static int dbmem_database_rollback_transaction (sqlite3 *db) {
    return sqlite3_exec(db, "ROLLBACK TO " DBMEM_SAVEPOINT_NAME "; RELEASE " DBMEM_SAVEPOINT_NAME ";", NULL, NULL, NULL);
}

// MARK: - Context -

static void *dbmem_context_create (sqlite3 *db) {
    dbmem_context *ctx = (dbmem_context *)dbmem_zeroalloc(sizeof(dbmem_context));
    if (!ctx) return NULL;
    
    ctx->db = db;
    ctx->chars_per_tokens = DEFAULT_CHARS_PER_TOKEN;
    ctx->max_tokens = DEFAULT_MAX_TOKENS;
    ctx->overlay_tokens = DEFAULT_OVERLAY_TOKENS;
    ctx->snippet_max_chars = DEFAULT_MAX_SNIPPET_CHARS;
    ctx->skip_html = true;
    ctx->save_content = true;
    ctx->engine_warmup = false;
    
    ctx->perform_fts = fts5_is_available;
    ctx->max_results = DEFAULT_MAX_RESULTS;
    ctx->vector_weight = DEFAULT_VECTOR_WEIGHT;
    ctx->text_weight = DEFAULT_TEXT_WEIGHT;
    ctx->min_score = DEFAULT_MIN_SCORE;
    ctx->update_access = true;
    ctx->embedding_cache = true;

    return (void *)ctx;
}

static void dbmem_context_free (void *ptr) {
    if (!ptr) return;
    dbmem_context *ctx = (dbmem_context *)ptr;

    if (ctx->provider) dbmem_free(ctx->provider);
    if (ctx->model) dbmem_free(ctx->model);
    if (ctx->api_key) dbmem_free(ctx->api_key);
    if (ctx->extensions) dbmem_free(ctx->extensions);
    if (ctx->cache_buffer) dbmem_free(ctx->cache_buffer);

    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (ctx->l_engine) dbmem_local_engine_free(ctx->l_engine);
    #endif
    
    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (ctx->r_engine) dbmem_remote_engine_free(ctx->r_engine);
    #endif

    dbmem_free(ctx);
}

static void dbmem_context_reset_temp_values (dbmem_context *ctx) {
    ctx->counter = 0;
    ctx->hash = 0;
    ctx->context = NULL;
    ctx->path = NULL;
    ctx->error_msg[0] = 0;
}

void *dbmem_context_engine (dbmem_context *ctx, bool *is_local) {
    if (is_local) *is_local = ctx->is_local;
    return (ctx->is_local) ? (void *)ctx->l_engine : (void *)ctx->r_engine;
}

bool dbmem_context_load_vector (dbmem_context *ctx) {
    if (ctx->vector_extension_available) return true;
    
    // check if sqlite-vector is loaded
    
    // there's no built-in way to verify if sqlite-vector has already been already loaded for this specific database connection
    // the workaround is to attempt to execute vector_version and check for an error
    // an error indicates that initialization has not been performed
    if (sqlite3_exec(ctx->db, "SELECT vector_version();", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", "SQLite-vector extension not found, make sure to load it before using the memory_search function");
        return false;
    }
    
    if (ctx->dimension == 0) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", "SQLite-vector extension cannot be loaded because embedding dimension is not specified");
        return false;
    }
    
    // In the future can check for quantization options and embedding type here
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT vector_init('dbmem_vault', 'embedding', 'type=FLOAT32,distance=COSINE,dimension=%d');", ctx->dimension);
    int rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", sqlite3_errmsg(ctx->db));
        return false;
    }
    
    ctx->vector_extension_available = true;
    return true;
}

bool dbmem_context_load_sync (dbmem_context *ctx) {
    return false;
#if 0
    if (ctx->sync_extension_available) return true;
    
    // there's no built-in way to verify if sqlite-sync has already been already loaded for this specific database connection
    // the workaround is to attempt to execute vector_version and check for an error (an error indicates that initialization has not been performed)
    if (sqlite3_exec(ctx->db, "SELECT cloudsync_version();", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", "SQLite-sync extension not found, make sure to load it before using the memory_sync function");
        return false;
    }
    
    // SELECT cloudsync_init
    
    ctx->sync_extension_available = true;
    return true;
#endif
}

bool dbmem_context_perform_fts (dbmem_context *ctx) {
    if (!fts5_is_available) return false;
    return ctx->perform_fts;
}

int dbmem_context_max_results (dbmem_context *ctx) {
    return ctx->max_results;
}

double dbmem_context_vector_weight (dbmem_context *ctx) {
    return ctx->vector_weight;
}

double dbmem_context_text_weight (dbmem_context *ctx) {
    return ctx->text_weight;
}

double dbmem_context_min_score (dbmem_context *ctx) {
    return ctx->min_score;
}

bool dbmem_context_update_access (dbmem_context *ctx) {
    return ctx->update_access;
}

int dbmem_context_search_oversample (dbmem_context *ctx) {
    return ctx->search_oversample;
}

const char *dbmem_context_errmsg (dbmem_context *ctx) {
    return ctx->error_msg;
}

// MARK: - Deletion -

static void dbmem_delete (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    if (sqlite3_value_type(argv[0]) != SQLITE_INTEGER) {
        sqlite3_result_error(context, "The function memory_delete expects one argument of type INTEGER (hash)", SQLITE_ERROR);
        return;
    }

    sqlite3_int64 hash = sqlite3_value_int64(argv[0]);
    sqlite3 *db = sqlite3_context_db_handle(context);

    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    // Delete from FTS first (if available)
    if (fts5_is_available) {
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash = ?1;", -1, &vm, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(vm, 1, hash);
            sqlite3_step(vm);
            sqlite3_finalize(vm);
        }
    }

    // Delete from vault
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_int64(vm, 1, hash);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    // Delete from content
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_int64(vm, 1, hash);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    int changes = sqlite3_changes(db);
    dbmem_database_commit_transaction(db);
    sqlite3_result_int(context, changes);
    return;

rollback:
    dbmem_database_rollback_transaction(db);
    sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

static void dbmem_delete_context (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_delete_context expects one argument of type TEXT (context)", SQLITE_ERROR);
        return;
    }

    const char *ctx_name = (const char *)sqlite3_value_text(argv[0]);
    sqlite3 *db = sqlite3_context_db_handle(context);

    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    // Delete from FTS first (if available)
    if (fts5_is_available) {
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash IN (SELECT hash FROM dbmem_content WHERE context = ?1);", -1, &vm, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(vm, 1, ctx_name, -1, SQLITE_STATIC);
            sqlite3_step(vm);
            sqlite3_finalize(vm);
        }
    }

    // Delete from vault
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash IN (SELECT hash FROM dbmem_content WHERE context = ?1);", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_text(vm, 1, ctx_name, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    // Delete from content
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE context = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    sqlite3_bind_text(vm, 1, ctx_name, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    int changes = sqlite3_changes(db);
    dbmem_database_commit_transaction(db);
    sqlite3_result_int(context, changes);
    return;

rollback:
    dbmem_database_rollback_transaction(db);
    sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

static void dbmem_clear (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);

    sqlite3 *db = sqlite3_context_db_handle(context);

    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    // Delete from FTS first (if available)
    if (fts5_is_available) {
        rc = sqlite3_exec(db, "DELETE FROM dbmem_vault_fts;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto rollback;
    }

    // Delete from vault
    rc = sqlite3_exec(db, "DELETE FROM dbmem_vault;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;

    // Delete from content
    rc = sqlite3_exec(db, "DELETE FROM dbmem_content;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;

    dbmem_database_commit_transaction(db);
    sqlite3_result_int(context, 1);
    return;

rollback:
    dbmem_database_rollback_transaction(db);
    sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

// MARK: - Cache Clear -

static void dbmem_cache_clear (sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc;

    if (argc == 0) {
        rc = sqlite3_exec(db, "DELETE FROM dbmem_cache;", NULL, NULL, NULL);
    } else if (argc == 2) {
        if (sqlite3_value_type(argv[0]) != SQLITE_TEXT || sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
            sqlite3_result_error(context, "The function memory_cache_clear expects two arguments of type TEXT (provider, model)", SQLITE_ERROR);
            return;
        }
        const char *provider = (const char *)sqlite3_value_text(argv[0]);
        const char *model = (const char *)sqlite3_value_text(argv[1]);

        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_cache WHERE provider=?1 AND model=?2;", -1, &vm, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(vm, 1, provider, -1, SQLITE_STATIC);
            sqlite3_bind_text(vm, 2, model, -1, SQLITE_STATIC);
            rc = sqlite3_step(vm);
            if (rc == SQLITE_DONE) rc = SQLITE_OK;
        }
        if (vm) sqlite3_finalize(vm);
    } else {
        sqlite3_result_error(context, "The function memory_cache_clear expects 0 or 2 arguments", SQLITE_ERROR);
        return;
    }

    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    sqlite3_result_int(context, sqlite3_changes(db));
}

// MARK: - General -

static void dbmem_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);
    sqlite3_result_text(context, SQLITE_DBMEMORY_VERSION, -1, NULL);
}

static int dbmem_reindex(dbmem_context *ctx);

static void dbmem_set_model (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // 2 TEXT arguments: provider and model
    
    // if provider is local then model is the full path to the model to use
    // options are saved into settings
    
    // sanity check type
    if ((sqlite3_value_type(argv[0]) != SQLITE_TEXT) || (sqlite3_value_type(argv[1]) != SQLITE_TEXT)) {
        sqlite3_result_error(context, "The function memory_set_model expects two arguments of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve arguments
    const char *provider = (const char *)sqlite3_value_text(argv[0]);
    const char *model = (const char *)sqlite3_value_text(argv[1]);
    
    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);

    // detect model change (only if a model was previously configured)
    bool model_changed = false;
    if (ctx->provider && ctx->model) {
        model_changed = (strcasecmp(ctx->provider, provider) != 0 ||
                         strcasecmp(ctx->model, model) != 0);
    }

    bool is_local_provider = (strcasecmp(provider, DBMEM_LOCAL_PROVIDER) == 0);
    #ifdef DBMEM_OMIT_LOCAL_ENGINE
    if (is_local_provider) {
        sqlite3_result_error(context, "Local provider cannot be set because SQLite-memory was compiled without local provider support", SQLITE_ERROR);
        return;
    }
    #endif
    #ifdef DBMEM_OMIT_REMOTE_ENGINE
    if (!is_local_provider) {
        sqlite3_result_error(context, "Remote provider cannot be set because SQLite-memory was compiled without remote provider support", SQLITE_ERROR);
        return;
    }
    #endif
    
    // if provider is local then make sure model file exists
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (is_local_provider) {
        if (dbmem_file_exists(model) == false) {
            sqlite3_result_error(context, "Local model not found in the specified path", SQLITE_ERROR);
            return;
        }
        
        if (ctx->l_engine) dbmem_local_engine_free(ctx->l_engine);
        ctx->l_engine = NULL;
        
        ctx->l_engine = dbmem_local_engine_init(model, ctx->error_msg);
        if (ctx->l_engine == NULL) {
            sqlite3_result_error(context, ctx->error_msg, -1);
            return;
        }
        
        if (ctx->engine_warmup) {
            dbmem_local_engine_warmup(ctx->l_engine);
        }
        
        ctx->is_local = true;
    }
    #endif
    
    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (!is_local_provider) {
        if (ctx->r_engine) dbmem_remote_engine_free(ctx->r_engine);
        ctx->r_engine = NULL;
        
        ctx->r_engine = dbmem_remote_engine_init(provider, model, ctx->error_msg);
        if (ctx->r_engine == NULL) {
            sqlite3_result_error(context, ctx->error_msg, -1);
            return;
        }
        
        ctx->is_local = false;
    }
    #endif
    
    // update settings
    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc = dbmem_settings_write_text (db, DBMEM_SETTINGS_KEY_PROVIDER, provider);
    if (rc == SQLITE_OK) rc = dbmem_settings_write_text (db, DBMEM_SETTINGS_KEY_MODEL, model);
    
    // sync settings
    if (rc == SQLITE_OK) {
        dbmem_settings_sync(ctx, DBMEM_SETTINGS_KEY_PROVIDER, argv[0]);
        dbmem_settings_sync(ctx, DBMEM_SETTINGS_KEY_MODEL, argv[1]);
    }

    // reindex all content if the model changed
    if (model_changed && rc == SQLITE_OK) {
        rc = dbmem_reindex(ctx);
    }

    (rc == SQLITE_OK) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

static void dbmem_set_apikey (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_set_apikey expects one argument of type TEXT", SQLITE_ERROR);
        return;
    }
    
    char *apikey = dbmem_strdup((const char *)sqlite3_value_text(argv[0]));
    if (!apikey) {
        sqlite3_result_error_nomem(context);
        return;
    }
    
    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    
    if (ctx->api_key) dbmem_free(ctx->api_key);
    ctx->api_key = apikey;
    
    sqlite3_result_int(context, 1);
}

// MARK: -

static void dbmem_set_option (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_set_option expects the key argument to be of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // update settings
    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *key = (const char *)sqlite3_value_text(argv[0]);
    int rc = dbmem_settings_write_value(db, key, argv[1]);
    
    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    
    if (rc == SQLITE_OK) {
        dbmem_settings_sync(ctx, key, argv[1]);
    }
    
    (rc == SQLITE_OK) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

static void dbmem_get_option (sqlite3_context *context, int argc, sqlite3_value **argv) {
    static const char *sql = "SELECT value FROM dbmem_settings WHERE key=?1 LIMIT 1;";
    
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_get_option expects the key argument to be of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve from settings
    sqlite3_stmt *vm = NULL;
    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    const char *key = (const char *)sqlite3_value_text(argv[0]);
    rc = sqlite3_bind_text(vm, 1, key, -1, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) {
        sqlite3_result_null(context);
        rc = SQLITE_OK;
    } else if (rc == SQLITE_ROW) {
        sqlite3_result_value(context, sqlite3_column_value(vm, 0));
        rc = SQLITE_OK;
    }
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    if (rc != SQLITE_OK) sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

// MARK: -

#if ENABLE_DBMEM_DEBUG
static void dbmem_dump_embeding (const embedding_result_t *result) {
    printf("{\n");
    printf("  \"n_tokens\": %d,\n", result->n_tokens);
    printf("  \"n_tokens_truncated\": %d,\n", result->n_tokens_truncated);
    printf("  \"n_embd\": %d,\n", result->n_embd);
    printf("  \"embedding\": [");

    for (int i = 0; i < result->n_embd; i++) {
        printf("%.8f", result->embedding[i]);
        if (i < result->n_embd - 1) printf(", ");
    }

    printf("]\n");
    printf("}\n");
    fflush(stdout);
}
#endif

// MARK: - Embedding Cache -

static bool dbmem_cache_lookup (dbmem_context *ctx, uint64_t text_hash, embedding_result_t *result) {
    static const char *sql = "SELECT embedding, dimension FROM dbmem_cache WHERE text_hash=?1 AND provider=?2 AND model=?3 LIMIT 1;";

    if (!ctx->provider || !ctx->model) return false;

    bool found = false;
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    sqlite3_bind_int64(vm, 1, (sqlite3_int64)text_hash);
    sqlite3_bind_text(vm, 2, ctx->provider, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 3, ctx->model, -1, SQLITE_STATIC);

    rc = sqlite3_step(vm);
    if (rc != SQLITE_ROW) goto cleanup;

    int dimension = sqlite3_column_int(vm, 1);
    int blob_bytes = sqlite3_column_bytes(vm, 0);
    const void *blob = sqlite3_column_blob(vm, 0);

    if (blob_bytes != dimension * (int)sizeof(float)) goto cleanup;

    // ensure cache_buffer is large enough
    if (ctx->cache_buffer_size < dimension) {
        float *new_buf = (float *)dbmem_realloc(ctx->cache_buffer, dimension * sizeof(float));
        if (!new_buf) goto cleanup;
        ctx->cache_buffer = new_buf;
        ctx->cache_buffer_size = dimension;
    }

    memcpy(ctx->cache_buffer, blob, blob_bytes);
    result->embedding = ctx->cache_buffer;
    result->n_embd = dimension;
    result->n_tokens = 0;
    result->n_tokens_truncated = 0;
    found = true;

cleanup:
    if (vm) sqlite3_finalize(vm);
    return found;
}

static void dbmem_cache_evict (dbmem_context *ctx) {
    static const char *sql = "DELETE FROM dbmem_cache WHERE rowid IN (SELECT rowid FROM dbmem_cache ORDER BY rowid ASC LIMIT ?1);";

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // count current entries
    sqlite3_stmt *count_vm = NULL;
    rc = sqlite3_prepare_v2(ctx->db, "SELECT COUNT(*) FROM dbmem_cache;", -1, &count_vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(count_vm);
    if (rc != SQLITE_ROW) { sqlite3_finalize(count_vm); goto cleanup; }
    int count = sqlite3_column_int(count_vm, 0);
    sqlite3_finalize(count_vm);

    int excess = count - ctx->cache_max_entries;
    if (excess <= 0) goto cleanup;

    sqlite3_bind_int(vm, 1, excess);
    sqlite3_step(vm);

cleanup:
    if (vm) sqlite3_finalize(vm);
}

static void dbmem_cache_store (dbmem_context *ctx, uint64_t text_hash, const embedding_result_t *result) {
    static const char *sql = "INSERT OR REPLACE INTO dbmem_cache (text_hash, provider, model, embedding, dimension) VALUES (?1, ?2, ?3, ?4, ?5);";

    if (!ctx->provider || !ctx->model) return;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    sqlite3_bind_int64(vm, 1, (sqlite3_int64)text_hash);
    sqlite3_bind_text(vm, 2, ctx->provider, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 3, ctx->model, -1, SQLITE_STATIC);
    sqlite3_bind_blob(vm, 4, result->embedding, result->n_embd * (int)sizeof(float), SQLITE_STATIC);
    sqlite3_bind_int(vm, 5, result->n_embd);

    sqlite3_step(vm);

cleanup:
    if (vm) sqlite3_finalize(vm);

    // evict oldest entries if limit is set and exceeded
    if (ctx->cache_max_entries > 0) {
        dbmem_cache_evict(ctx);
    }
}

// MARK: -

static int dbmem_process_callback (const char *text, size_t len, size_t offset, size_t length, void *xdata, size_t index) {
    dbmem_context *ctx = (dbmem_context *)xdata;
    embedding_result_t result = {0};
    int rc = SQLITE_OK;

    // check embedding cache
    uint64_t chunk_hash = 0;
    bool cache_hit = false;
    if (ctx->embedding_cache) {
        chunk_hash = dbmem_hash_compute(text, len);
        cache_hit = dbmem_cache_lookup(ctx, chunk_hash, &result);
    }

    if (!cache_hit) {
        // compute embedding
        if (ctx->is_local) {
        #ifndef DBMEM_OMIT_LOCAL_ENGINE
            rc = dbmem_local_compute_embedding(ctx->l_engine, text, (int)len, &result);
            if (rc != 0) {
                const char *err = dbmem_local_errmsg(ctx->l_engine);
                memcpy(ctx->error_msg, err, strlen(err) + 1);
                return rc;
            }
        #else
            const char *err = "Local embedding cannot be computed because extension was compiled without local engine support";
            memcpy(ctx->error_msg, err, strlen(err) + 1);
            return 1;
        #endif
        }

        if (!ctx->is_local) {
        #ifndef DBMEM_OMIT_REMOTE_ENGINE
            rc = dbmem_remote_compute_embedding(ctx->r_engine, text, (int)len, &result);
            if (rc != 0) {
                const char *err = dbmem_remote_errmsg(ctx->r_engine);
                memcpy(ctx->error_msg, err, strlen(err) + 1);
                return rc;
            }
        #else
            const char *err = "Remote embedding cannot be computed because extension was compiled without remote engine support";
            memcpy(ctx->error_msg, err, strlen(err) + 1);
            return 1;
        #endif
        }

        // store in cache on miss
        if (ctx->embedding_cache) {
            dbmem_cache_store(ctx, chunk_hash, &result);
        }
    }
    
    // make sure dimension is the same
    if (ctx->dimension == 0) ctx->dimension = result.n_embd;
    else if (ctx->dimension != result.n_embd) {
        const char *err = "Embedding dimension mismatch from the one stored in the database.";
        memcpy(ctx->error_msg, err, strlen(err) + 1);
        return SQLITE_MISMATCH;
    }
    
    // save embedding to database
    rc = dbmem_database_add_chunk(ctx, &result, offset, length, index);
    if (rc != 0) {
        const char *err = sqlite3_errmsg(ctx->db);
        memcpy(ctx->error_msg, err, strlen(err) + 1);
        goto cleanup;
    }
    DEBUG_EMBEDDING(&result);

    // save FTS5 (if available)
    if (!fts5_is_available) goto cleanup;
    rc = dbmem_database_add_fts5(ctx, text, len, index);
    if (rc != 0) {
        const char *err = sqlite3_errmsg(ctx->db);
        memcpy(ctx->error_msg, err, strlen(err) + 1);
        goto cleanup;
    }
    
cleanup:
    return rc;
}

static int dbmem_process_buffer (dbmem_context *ctx, const char *buffer, int64_t len) {
    // check if buffer was already processed
    uint64_t hash = dbmem_hash_compute (buffer, (size_t)len);
    if (dbmem_database_check_if_stored(ctx->db, hash, len)) return SQLITE_OK;
    
    // set up parse settings
    dbmem_parse_settings settings = {0};
    
    ctx->hash = hash;
    settings.xdata = (void *)ctx;
    settings.callback = dbmem_process_callback;
    settings.chars_per_token = ctx->chars_per_tokens;
    settings.max_tokens = ctx->max_tokens;
    settings.overlay_tokens = ctx->overlay_tokens;
    settings.skip_semantic = ctx->skip_semantic;
    settings.skip_html = ctx->skip_html;
    
    sqlite3 *db = ctx->db;
    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) goto cleanup;

    // delete old entry if this path was previously indexed with different content
    dbmem_database_delete_stale_path(db, ctx->path, hash);

    rc = dbmem_database_add_entry(ctx, db, hash, buffer, len);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = dbmem_parse(buffer, (size_t)len, &settings);
    
    if (rc == SQLITE_OK && !ctx->dimension_saved) {
        // make sure to serialize dimension
        dbmem_settings_write_int(db, DBMEM_SETTINGS_KEY_DIMENSION, ctx->dimension);
        ctx->dimension_saved = true;
    }
    
cleanup:
    (rc == SQLITE_OK) ? dbmem_database_commit_transaction(db) : dbmem_database_rollback_transaction(db);
    return rc;
}

static int dbmem_process_file (dbmem_context *ctx, const char *path) {
    if (!dbmem_file_exists(path)) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "Unable to find file at path %s", path);
        return -1;
    }
    
    // check if the file needs to be skipped based on its extension
    char *extensions = (ctx->extensions) ? ctx->extensions : "md,mdx";
    if (extensions && !dbmem_file_has_extension(path, extensions)) return 0;
    
    int64_t len = 0;
    char *buffer = dbmem_file_read(path, &len);
    if (!buffer) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "Unable to read file at path %s", path);
        return -1;
    }
    
    // do real processing
    ctx->path = path;
    int rc = dbmem_process_buffer(ctx, buffer, len);
    dbmem_free(buffer);
    
    DEBUG_DBMEM("%*d\t%s", 4, (int)ctx->counter, path);
    return rc;
}

static int dbmem_reindex (dbmem_context *ctx) {
    sqlite3 *db = ctx->db;
    int rc = SQLITE_OK;

    // copy all content to a temp table
    rc = sqlite3_exec(db, "CREATE TEMP TABLE dbmem_reindex AS SELECT path, value, context FROM dbmem_content;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    // clear all indexed data
    if (fts5_is_available) {
        sqlite3_exec(db, "DELETE FROM dbmem_vault_fts;", NULL, NULL, NULL);
    }
    sqlite3_exec(db, "DELETE FROM dbmem_vault;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM dbmem_content;", NULL, NULL, NULL);

    // reset dimension so the new model's dimension is auto-detected
    ctx->dimension = 0;
    ctx->dimension_saved = false;
    ctx->vector_extension_available = false;

    // iterate temp table one row at a time
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT path, value, context FROM dbmem_reindex;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    while (sqlite3_step(vm) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(vm, 0);
        const char *value = (const char *)sqlite3_column_text(vm, 1);
        int value_len = sqlite3_column_bytes(vm, 1);
        const char *context = (const char *)sqlite3_column_text(vm, 2);

        dbmem_context_reset_temp_values(ctx);
        ctx->context = context;

        if (path && dbmem_file_exists(path)) {
            dbmem_process_file(ctx, path);
        } else if (value && value_len > 0) {
            ctx->path = path;
            dbmem_process_buffer(ctx, value, value_len);
        }
        // else: skip entries that can't be rebuilt
    }

    rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    sqlite3_exec(db, "DROP TABLE IF EXISTS dbmem_reindex;", NULL, NULL, NULL);
    return rc;
}

static int dbmem_scan_callback (const char *path, void *data) {
    dbmem_context *ctx = (dbmem_context *)data;
    
    int rc = dbmem_process_file(ctx, path);
    if (rc == 0) ctx->counter++;
    
    return rc;
}

static void dbmem_sync_text (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_sync_text expects a parameter of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve dbmem_context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *content = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    
    // reset temp values
    dbmem_context_reset_temp_values(ctx);
    
    // check for optional memory context
    if ((argc == 2) && (sqlite3_value_type(argv[1]) == SQLITE_TEXT)) {
        ctx->context = (const char *)sqlite3_value_text(argv[1]);
    }
    
    int rc = dbmem_process_buffer(ctx, content, len);
    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

#ifndef DBMEM_OMIT_IO
static void dbmem_sync_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_sync_file expects the first parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve dbmem_context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    
    // reset temp values
    dbmem_context_reset_temp_values(ctx);
    
    // check for optional memory context
    if ((argc == 2) && (sqlite3_value_type(argv[1]) == SQLITE_TEXT)) {
        ctx->context = (const char *)sqlite3_value_text(argv[1]);
    }
    
    int rc = dbmem_process_file(ctx, path);
    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

static void dbmem_database_delete_missing_files (sqlite3 *db, const char *dir_path) {
    char *sql = sqlite3_mprintf("SELECT hash, path FROM dbmem_content WHERE path LIKE '%q%%';", dir_path);
    if (!sql) return;

    char **table = NULL;
    int nrow = 0, ncol = 0;
    int rc = sqlite3_get_table(db, sql, &table, &nrow, &ncol, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK || nrow == 0) {
        if (table) sqlite3_free_table(table);
        return;
    }

    dbmem_database_begin_transaction(db);
    for (int i = 0; i < nrow; i++) {
        const char *path = table[ncol + i * ncol + 1];
        if (dbmem_file_exists(path)) continue;
        sqlite3_int64 hash = strtoll(table[ncol + i * ncol], NULL, 10);
        dbmem_database_delete_hash(db, hash);
    }
    dbmem_database_commit_transaction(db);
    sqlite3_free_table(table);
}

static void dbmem_sync_directory (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_sync_directory expects the first parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }

    // retrieve dbmem_context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *path = (const char *)sqlite3_value_text(argv[0]);

    // reset temp values
    dbmem_context_reset_temp_values(ctx);

    // check for optional memory context
    if ((argc == 2) && (sqlite3_value_type(argv[1]) == SQLITE_TEXT)) {
        ctx->context = (const char *)sqlite3_value_text(argv[1]);
    }

    if (!dbmem_dir_exists(path)) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "Unable to find directory at path %s", path);
        sqlite3_result_error(context, ctx->error_msg, SQLITE_ERROR);
        return;
    }

    // Phase 1: remove entries whose files no longer exist on disk
    dbmem_database_delete_missing_files(ctx->db, path);

    // Phase 2: add new + reindex changed (hash-skip handles unchanged)
    int rc = dbmem_dir_scan(path, dbmem_scan_callback, ctx);
    (rc == 0) ? sqlite3_result_int64(context, ctx->counter) : sqlite3_result_error(context, ctx->error_msg, -1);
}
#endif
    
// MARK: -

SQLITE_DBMEMORY_API int sqlite3_memory_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    int rc = SQLITE_OK;
    
    rc = dbmem_database_init(db);
    if (rc != SQLITE_OK) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("An error occurred while creating internal tables (%s)", sqlite3_errmsg(db));
        return rc;
    }
    
    void *ctx = dbmem_context_create(db);
    if (!ctx) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Not enough memory to create a database context");
        return SQLITE_NOMEM;
    }
    
    dbmem_settings_load(db, (dbmem_context *)ctx);
    
    rc = sqlite3_create_function_v2(db, "memory_version", 0, SQLITE_UTF8, ctx, dbmem_version, NULL, NULL, dbmem_context_free);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_set_option", 2, SQLITE_UTF8, ctx, dbmem_set_option, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_get_option", 1, SQLITE_UTF8, ctx, dbmem_get_option, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_set_model", 2, SQLITE_UTF8, ctx, dbmem_set_model, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_set_apikey", 1, SQLITE_UTF8, ctx, dbmem_set_apikey, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    #ifndef DBMEM_OMIT_IO
    rc = sqlite3_create_function_v2(db, "memory_sync_file", 1, SQLITE_UTF8, ctx, dbmem_sync_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_sync_file", 2, SQLITE_UTF8, ctx, dbmem_sync_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_sync_directory", 1, SQLITE_UTF8, ctx, dbmem_sync_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_sync_directory", 2, SQLITE_UTF8, ctx, dbmem_sync_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    #endif
    
    rc = sqlite3_create_function_v2(db, "memory_sync_text", 1, SQLITE_UTF8, ctx, dbmem_sync_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_sync_text", 2, SQLITE_UTF8, ctx, dbmem_sync_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_delete", 1, SQLITE_UTF8, ctx, dbmem_delete, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_delete_context", 1, SQLITE_UTF8, ctx, dbmem_delete_context, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_clear", 0, SQLITE_UTF8, ctx, dbmem_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_cache_clear", 0, SQLITE_UTF8, ctx, dbmem_cache_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function_v2(db, "memory_cache_clear", 2, SQLITE_UTF8, ctx, dbmem_cache_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_register_search(db, ctx, pzErrMsg);
    if (rc != SQLITE_OK) return rc;

    return SQLITE_OK;
}

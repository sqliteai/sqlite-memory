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
#include <stdarg.h>
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
#define DBMEM_SETTINGS_KEY_EMBEDDING_CACHE      "embedding_cache"
#define DBMEM_SETTINGS_KEY_CACHE_MAX_ENTRIES    "cache_max_entries"
#define DBMEM_SETTINGS_KEY_SEARCH_OVERSAMPLE    "search_oversample"

// default values from https://docs.openclaw.ai/concepts/memory
#define DEFAULT_CHARS_PER_TOKEN                 4       // Approximate number of characters per token (GPT ≈ 4, Claude ≈ 3.5)
#define DEFAULT_MAX_TOKENS                      400     // Maximum tokens per chunk
#define DEFAULT_OVERLAY_TOKENS                  80      // Token overlap between consecutive chunks
#define DEFAULT_MAX_SNIPPET_CHARS               700     // Maximum characters for search result snippets
#define DEFAULT_MAX_RESULTS                     20      // Maximum number of search results
#define DEFAULT_VECTOR_WEIGHT                   0.6     // Semantic similarity weight in hybrid search scoring
#define DEFAULT_TEXT_WEIGHT                     0.4     // Full-text match weight in hybrid search scoring
#define DEFAULT_MIN_SCORE                       0.7     // Minimum score threshold to filter irrelevant results

struct dbmem_context {
    // Database and engine
    sqlite3                 *db;                // SQLite database connection
    bool                    is_local;           // Flag set based on memory_set_model
    dbmem_local_engine_t    *l_engine;          // Local embedding engine (llama.cpp based)
    dbmem_remote_engine_t   *r_engine;          // Remote embedding engine (vectors.space based)

    // Custom embedding provider
    dbmem_provider_t        custom_provider;    // User-registered callbacks
    char                    *custom_provider_name; // Provider name for matching
    void                    *custom_engine;     // Opaque engine from custom_provider.init
    bool                    is_custom;          // True when custom provider is active

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
    bool        sync_enabled;                   // True when memory_enable_sync has been successfully called
    bool        reindex_mode;                   // When true, process_buffer skips hash check and content insert (for post-sync reindex)
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

static int dbmem_bind_hash (sqlite3_stmt *vm, int index, uint64_t hash) {
    char hash_text[DBMEM_HASH_STR_MAXLEN];
    dbmem_hash_to_hex(hash, hash_text);
    return sqlite3_bind_text(vm, index, hash_text, -1, SQLITE_TRANSIENT);
}

static bool dbmem_column_hash (sqlite3_stmt *vm, int column, uint64_t *hash) {
    const char *hash_text = (const char *)sqlite3_column_text(vm, column);
    return dbmem_hash_from_hex(hash_text, hash);
}

static bool dbmem_value_hash (sqlite3_value *value, uint64_t *hash) {
    if (!value || !hash) return false;

    switch (sqlite3_value_type(value)) {
        case SQLITE_TEXT:
            return dbmem_hash_from_hex((const char *)sqlite3_value_text(value), hash);
        case SQLITE_INTEGER:
            *hash = (uint64_t)sqlite3_value_int64(value);
            return true;
        default:
            return false;
    }
}

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
        if (n >= 0) ctx->max_results = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_VECTOR_WEIGHT) == 0) {
        double n = sqlite3_value_double(value);
        if (n >= 0) ctx->vector_weight = n;
        return 0;
    }

    if (strcasecmp(key, DBMEM_SETTINGS_KEY_TEXT_WEIGHT) == 0) {
        double n = sqlite3_value_double(value);
        if (n >= 0) ctx->text_weight = n;
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MIN_SCORE) == 0) {
        double n = sqlite3_value_double(value);
        if (n >= 0) ctx->min_score = n;
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
            if (ctx->provider) dbmemory_free(ctx->provider);
            ctx->provider = provider;
        }
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_MODEL) == 0) {
        char *model = dbmem_strdup((const char *)sqlite3_value_text(value));
        if (model) {
            if (ctx->model) dbmemory_free(ctx->model);
            ctx->model = model;
        }
        return 0;
    }
    
    if (strcasecmp(key, DBMEM_SETTINGS_KEY_EXTENSIONS) == 0) {
        char *extensions = dbmem_strdup((const char *)sqlite3_value_text(value));
        if (extensions) {
            if (ctx->extensions) dbmemory_free(ctx->extensions);
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
        if (!key) continue;
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
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_content (hash TEXT PRIMARY KEY NOT NULL, path TEXT NOT NULL DEFAULT '' UNIQUE, value TEXT DEFAULT NULL, length INTEGER NOT NULL DEFAULT 0, context TEXT DEFAULT NULL, created_at INTEGER DEFAULT 0, last_accessed INTEGER DEFAULT 0);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_vault (hash TEXT NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, PRIMARY KEY (hash, seq));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_cache (text_hash TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL, embedding BLOB NOT NULL, dimension INTEGER NOT NULL, PRIMARY KEY (text_hash, provider, model));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    sql = "CREATE VIRTUAL TABLE IF NOT EXISTS dbmem_vault_fts USING fts5 (content, hash UNINDEXED, seq UNINDEXED, context UNINDEXED);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fts5_is_available = false;
        rc = SQLITE_OK;
    }
    
    // explicitly allows extension loading (only available when linked statically)
    // when loaded dynamically, the calling application must enable extension loading
    #if defined(SQLITE_CORE) && !defined(SQLITE_OMIT_LOAD_EXTENSION)
    rc = sqlite3_enable_load_extension(db, 1);
    if (rc != SQLITE_OK) return rc;
    #endif

    return rc;
}

static bool dbmem_database_check_if_stored (sqlite3 *db, uint64_t hash, int64_t len) {
    static const char *sql = "SELECT length FROM dbmem_content WHERE hash=? LIMIT 1;";
    
    bool result = false;
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = dbmem_bind_hash(vm, 1, hash);
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

static void dbmem_database_delete_hash (sqlite3 *db, uint64_t hash) {
    sqlite3_stmt *vm = NULL;
    if (fts5_is_available) {
        sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash=?1;", -1, &vm, NULL);
        dbmem_bind_hash(vm, 1, hash);
        sqlite3_step(vm);
        sqlite3_finalize(vm);
    }
    sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash=?1;", -1, &vm, NULL);
    dbmem_bind_hash(vm, 1, hash);
    sqlite3_step(vm);
    sqlite3_finalize(vm);

    sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash=?1;", -1, &vm, NULL);
    dbmem_bind_hash(vm, 1, hash);
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
        uint64_t old_hash = 0;
        bool has_old_hash = dbmem_column_hash(vm, 0, &old_hash);
        sqlite3_finalize(vm);
        if (has_old_hash && old_hash != new_hash) {
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

    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto cleanup;

    const char *path = ctx->path;
    char uuid[DBMEM_UUID_STR_MAXLEN];
    if (path == NULL) path = dbmem_uuid_v7(uuid);
    rc = sqlite3_bind_text(vm, 2, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    if (len > (int64_t)INT_MAX) { rc = SQLITE_TOOBIG; goto cleanup; }
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
    
    rc = dbmem_bind_hash(vm, 1, ctx->hash);
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
    
    rc = dbmem_bind_hash(vm, 2, ctx->hash);
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
    dbmem_context *ctx = (dbmem_context *)dbmemory_zeroalloc(sizeof(dbmem_context));
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

    if (ctx->provider) dbmemory_free(ctx->provider);
    if (ctx->model) dbmemory_free(ctx->model);
    if (ctx->api_key) dbmemory_free(ctx->api_key);
    if (ctx->extensions) dbmemory_free(ctx->extensions);
    if (ctx->cache_buffer) dbmemory_free(ctx->cache_buffer);

    // custom provider
    if (ctx->custom_engine && ctx->custom_provider.free) ctx->custom_provider.free(ctx->custom_engine, ctx->custom_provider.xdata);
    if (ctx->custom_provider_name) dbmemory_free(ctx->custom_provider_name);

    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (ctx->l_engine) dbmem_local_engine_free(ctx->l_engine);
    #endif

    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (ctx->r_engine) dbmem_remote_engine_free(ctx->r_engine);
    #endif

    dbmemory_free(ctx);
}

static void dbmem_context_reset_temp_values (dbmem_context *ctx) {
    ctx->counter = 0;
    ctx->hash = 0;
    ctx->context = NULL;
    ctx->path = NULL;
    ctx->error_msg[0] = 0;
}

void *dbmem_context_engine (dbmem_context *ctx, bool *is_local) {
    if (ctx->is_custom) {
        if (is_local) *is_local = false;
        return ctx->custom_engine;
    }
    if (is_local) *is_local = ctx->is_local;
    return (ctx->is_local) ? (void *)ctx->l_engine : (void *)ctx->r_engine;
}

bool dbmem_context_is_custom (dbmem_context *ctx) {
    return ctx->is_custom;
}

int dbmem_context_custom_compute (dbmem_context *ctx, const char *text, int text_len, embedding_result_t *result) {
    dbmem_embedding_result_t cr = {0};
    int rc = ctx->custom_provider.compute(ctx->custom_engine, text, text_len, ctx->custom_provider.xdata, &cr);
    if (rc != 0) return rc;
    result->n_tokens = cr.n_tokens;
    result->truncated = cr.truncated;
    result->n_embd = cr.n_embd;
    result->embedding = cr.embedding;
    return 0;
}

bool dbmem_context_load_vector (dbmem_context *ctx) {
    if (ctx->vector_extension_available) return true;
    
    // check if sqlite-vector is loaded
    
    // there's no built-in way to verify if sqlite-vector has already been already loaded for this specific database connection
    // the workaround is to attempt to execute vector_version and check for an error
    // an error indicates that initialization has not been performed
    if (sqlite3_exec(ctx->db, "SELECT vector_version();", NULL, NULL, NULL) != SQLITE_OK) {
        dbmem_context_set_error(ctx, "SQLite-vector extension not found, make sure to load it before using the memory_search function");
        return false;
    }

    if (ctx->dimension == 0) {
        dbmem_context_set_error(ctx, "SQLite-vector extension cannot be loaded because embedding dimension is not specified");
        return false;
    }
    
    // In the future can check for quantization options and embedding type here
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT vector_init('dbmem_vault', 'embedding', 'type=FLOAT32,distance=COSINE,dimension=%d');", ctx->dimension);
    int rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        dbmem_context_set_error(ctx, sqlite3_errmsg(ctx->db));
        return false;
    }
    
    ctx->vector_extension_available = true;
    return true;
}

bool dbmem_context_sync_available (dbmem_context *ctx) {
    if (ctx->sync_extension_available) return true;
    
    // there's no built-in way to verify if sqlite-sync has already been already loaded for this specific database connection
    // the workaround is to attempt to execute cloudsync_version and check for an error (an error indicates that initialization has not been performed)
    if (sqlite3_exec(ctx->db, "SELECT cloudsync_version();", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", "SQLite-sync extension not found, make sure to load it before using the memory_sync function");
        return false;
    }

    ctx->sync_extension_available = true;
    return true;
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

const char *dbmem_context_apikey (dbmem_context *ctx) {
    return ctx->api_key;
}

void dbmem_context_set_error (dbmem_context *ctx, const char *str) {
    snprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, "%s", str);
}

void dbmem_context_set_errorf (dbmem_context *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->error_msg, DBMEM_ERRBUF_SIZE, fmt, ap);
    va_end(ap);
}

// MARK: - Deletion -

static void dbmem_delete (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    uint64_t hash = 0;
    if (!dbmem_value_hash(argv[0], &hash)) {
        sqlite3_result_error(context, "The function memory_delete expects one argument of type TEXT (hash)", SQLITE_ERROR);
        return;
    }
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
            dbmem_bind_hash(vm, 1, hash);
            sqlite3_step(vm);
            sqlite3_finalize(vm);
        }
    }

    // Delete from vault
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    dbmem_bind_hash(vm, 1, hash);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    // Delete from content
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    dbmem_bind_hash(vm, 1, hash);
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
    sqlite3 *db = sqlite3_context_db_handle(context);

    // detect model change (only if a model was previously configured)
    bool model_changed = false;
    if (ctx->provider && ctx->model) {
        model_changed = (strcasecmp(ctx->provider, provider) != 0 || strcasecmp(ctx->model, model) != 0);
    }

    bool is_local_provider = (strcasecmp(provider, DBMEM_LOCAL_PROVIDER) == 0);

    // check if a custom provider matches
    bool is_custom_provider = (ctx->custom_provider_name && ctx->custom_provider.compute &&
                               strcasecmp(provider, ctx->custom_provider_name) == 0);

    if (!is_custom_provider) {
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
    }

    char *new_provider = dbmem_strdup(provider);
    char *new_model = dbmem_strdup(model);
    if (!new_provider || !new_model) {
        if (new_provider) dbmemory_free(new_provider);
        if (new_model) dbmemory_free(new_model);
        sqlite3_result_error_nomem(context);
        return;
    }

    char *old_provider = ctx->provider;
    char *old_model = ctx->model;
    bool old_is_local = ctx->is_local;
    bool old_is_custom = ctx->is_custom;

    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    dbmem_local_engine_t *old_l_engine = ctx->l_engine;
    dbmem_local_engine_t *new_l_engine = ctx->l_engine;
    #endif

    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    dbmem_remote_engine_t *old_r_engine = ctx->r_engine;
    dbmem_remote_engine_t *new_r_engine = ctx->r_engine;
    #endif

    void *old_custom_engine = ctx->custom_engine;
    void *new_custom_engine = ctx->custom_engine;
    bool set_model_started = false;
    int rc = SQLITE_OK;

    // custom provider path
    if (is_custom_provider) {
        new_custom_engine = ctx->custom_provider.init(model, ctx->api_key, ctx->custom_provider.xdata, ctx->error_msg);
        if (new_custom_engine == NULL) {
            dbmemory_free(new_provider);
            dbmemory_free(new_model);
            sqlite3_result_error(context, ctx->error_msg, -1);
            return;
        }
    }

    // if provider is local then make sure model file exists
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (!is_custom_provider && is_local_provider) {
        if (dbmem_file_exists(model) == false) {
            dbmemory_free(new_provider);
            dbmemory_free(new_model);
            sqlite3_result_error(context, "Local model not found in the specified path", SQLITE_ERROR);
            return;
        }

        new_l_engine = dbmem_local_engine_init(ctx, model, ctx->error_msg);
        if (new_l_engine == NULL) {
            dbmemory_free(new_provider);
            dbmemory_free(new_model);
            sqlite3_result_error(context, ctx->error_msg, -1);
            return;
        }

        if (ctx->engine_warmup) {
            dbmem_local_engine_warmup(new_l_engine);
        }
    }
    #endif

    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (!is_custom_provider && !is_local_provider) {
        new_r_engine = dbmem_remote_engine_init(ctx, provider, model, ctx->error_msg);
        if (new_r_engine == NULL) {
            dbmemory_free(new_provider);
            dbmemory_free(new_model);
            sqlite3_result_error(context, ctx->error_msg, -1);
            return;
        }
    }
    #endif

    ctx->provider = new_provider;
    ctx->model = new_model;
    ctx->is_local = is_custom_provider ? false : is_local_provider;
    ctx->is_custom = is_custom_provider;
    ctx->custom_engine = new_custom_engine;
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    ctx->l_engine = new_l_engine;
    #endif
    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    ctx->r_engine = new_r_engine;
    #endif

    rc = sqlite3_exec(db, "SAVEPOINT dbmem_set_model;", NULL, NULL, NULL);
    if (rc == SQLITE_OK) set_model_started = true;

    // update settings
    if (rc == SQLITE_OK) rc = dbmem_settings_write_text(db, DBMEM_SETTINGS_KEY_PROVIDER, provider);
    if (rc == SQLITE_OK) rc = dbmem_settings_write_text(db, DBMEM_SETTINGS_KEY_MODEL, model);

    // reindex all content if the model changed
    if (model_changed && rc == SQLITE_OK) {
        rc = dbmem_reindex(ctx);
    }

    if (rc == SQLITE_OK && set_model_started) {
        rc = sqlite3_exec(db, "RELEASE dbmem_set_model;", NULL, NULL, NULL);
        set_model_started = false;
    }

    if (rc != SQLITE_OK) {
        if (set_model_started) {
            sqlite3_exec(db, "ROLLBACK TO dbmem_set_model; RELEASE dbmem_set_model;", NULL, NULL, NULL);
        }

        ctx->provider = old_provider;
        ctx->model = old_model;
        ctx->is_local = old_is_local;
        ctx->is_custom = old_is_custom;
        ctx->custom_engine = old_custom_engine;
        #ifndef DBMEM_OMIT_LOCAL_ENGINE
        ctx->l_engine = old_l_engine;
        if (!is_custom_provider && is_local_provider && new_l_engine != old_l_engine && new_l_engine) {
            dbmem_local_engine_free(new_l_engine);
        }
        #endif
        #ifndef DBMEM_OMIT_REMOTE_ENGINE
        ctx->r_engine = old_r_engine;
        if (!is_custom_provider && !is_local_provider && new_r_engine != old_r_engine && new_r_engine) {
            dbmem_remote_engine_free(new_r_engine);
        }
        #endif
        if (is_custom_provider && new_custom_engine != old_custom_engine && new_custom_engine && ctx->custom_provider.free) {
            ctx->custom_provider.free(new_custom_engine, ctx->custom_provider.xdata);
        }
        dbmemory_free(new_provider);
        dbmemory_free(new_model);
        sqlite3_result_error(context, ctx->error_msg[0] ? ctx->error_msg : sqlite3_errmsg(db), -1);
        return;
    }

    if (old_provider) dbmemory_free(old_provider);
    if (old_model) dbmemory_free(old_model);
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (!is_custom_provider && is_local_provider) {
        if (old_l_engine && old_l_engine != new_l_engine) {
            dbmem_local_engine_free(old_l_engine);
        }
    } else if (old_l_engine) {
        // switching away from local provider: release the previous engine
        dbmem_local_engine_free(old_l_engine);
        ctx->l_engine = NULL;
    }
    #endif
    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (!is_custom_provider && !is_local_provider) {
        if (old_r_engine && old_r_engine != new_r_engine) {
            dbmem_remote_engine_free(old_r_engine);
        }
    } else if (old_r_engine) {
        // switching away from remote provider: release the previous engine
        dbmem_remote_engine_free(old_r_engine);
        ctx->r_engine = NULL;
    }
    #endif
    if (is_custom_provider) {
        if (old_custom_engine && old_custom_engine != new_custom_engine && ctx->custom_provider.free) {
            ctx->custom_provider.free(old_custom_engine, ctx->custom_provider.xdata);
        }
    } else if (old_custom_engine && ctx->custom_provider.free) {
        // switching away from custom provider: release the previous engine
        ctx->custom_provider.free(old_custom_engine, ctx->custom_provider.xdata);
        ctx->custom_engine = NULL;
    }

    sqlite3_result_int(context, 1);
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

    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (ctx->r_engine && !ctx->is_local && !ctx->is_custom) {
        int rc = dbmem_remote_engine_set_apikey(ctx->r_engine, apikey, ctx->error_msg);
        if (rc != SQLITE_OK) {
            dbmemory_free(apikey);
            sqlite3_result_error(context, ctx->error_msg[0] ? ctx->error_msg : "Unable to update remote API key", -1);
            return;
        }
    }
    #endif

    if (ctx->api_key) dbmemory_free(ctx->api_key);
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
    printf("  \"truncated\": %s,\n", result->truncated ? "true" : "false");
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

    dbmem_bind_hash(vm, 1, text_hash);
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
        float *new_buf = (float *)dbmemory_realloc(ctx->cache_buffer, dimension * sizeof(float));
        if (!new_buf) goto cleanup;
        ctx->cache_buffer = new_buf;
        ctx->cache_buffer_size = dimension;
    }

    memcpy(ctx->cache_buffer, blob, blob_bytes);
    result->embedding = ctx->cache_buffer;
    result->n_embd = dimension;
    result->n_tokens = 0;
    result->truncated = false;
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

    dbmem_bind_hash(vm, 1, text_hash);
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
        if (ctx->is_custom) {
            rc = dbmem_context_custom_compute(ctx, text, (int)len, &result);
            if (rc != 0) return rc;
        }

        else if (ctx->is_local) {
        #ifndef DBMEM_OMIT_LOCAL_ENGINE
            rc = dbmem_local_compute_embedding(ctx->l_engine, text, (int)len, &result);
            if (rc != 0) return rc;
        #else
            dbmem_context_set_error(ctx, "Local embedding cannot be computed because extension was compiled without local engine support");
            return 1;
        #endif
        }

        else {
        #ifndef DBMEM_OMIT_REMOTE_ENGINE
            rc = dbmem_remote_compute_embedding(ctx->r_engine, text, (int)len, &result);
            if (rc != 0) return rc;
        #else
            dbmem_context_set_error(ctx, "Remote embedding cannot be computed because extension was compiled without remote engine support");
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
        dbmem_context_set_error(ctx, "Embedding dimension mismatch from the one stored in the database.");
        return SQLITE_MISMATCH;
    }

    // save embedding to database
    rc = dbmem_database_add_chunk(ctx, &result, offset, length, index);
    if (rc != 0) {
        dbmem_context_set_error(ctx, sqlite3_errmsg(ctx->db));
        goto cleanup;
    }
    DEBUG_EMBEDDING(&result);

    // save FTS5 (if available)
    if (!fts5_is_available) goto cleanup;
    rc = dbmem_database_add_fts5(ctx, text, len, index);
    if (rc != 0) {
        dbmem_context_set_error(ctx, sqlite3_errmsg(ctx->db));
        goto cleanup;
    }
    
cleanup:
    return rc;
}

static int dbmem_process_buffer (dbmem_context *ctx, const char *buffer, int64_t len) {
    uint64_t hash = dbmem_hash_compute(buffer, (size_t)len);

    // In normal mode: skip if already indexed
    if (!ctx->reindex_mode) {
        if (dbmem_database_check_if_stored(ctx->db, hash, len)) return SQLITE_OK;
    }

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
    settings.mdx_mode = (ctx->path && dbmem_file_has_extension(ctx->path, "mdx"));

    sqlite3 *db = ctx->db;
    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) goto cleanup;

    if (!ctx->reindex_mode) {
        // delete old entry if this path was previously indexed with different content
        dbmem_database_delete_stale_path(db, ctx->path, hash);
        rc = dbmem_database_add_entry(ctx, db, hash, buffer, len);
        if (rc != SQLITE_OK) goto cleanup;
    }

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
        dbmem_context_set_errorf(ctx, "Unable to find file at path %s", path);
        return -1;
    }

    // check if the file needs to be skipped based on its extension
    const char *extensions = (ctx->extensions) ? ctx->extensions : "md,mdx";
    if (extensions && !dbmem_file_has_extension(path, extensions)) return 0;

    int64_t len = 0;
    char *buffer = dbmem_file_read(path, &len);
    if (!buffer) {
        dbmem_context_set_errorf(ctx, "Unable to read file at path %s", path);
        return -1;
    }
    
    // do real processing
    ctx->path = path;
    int rc = dbmem_process_buffer(ctx, buffer, len);
    dbmemory_free(buffer);
    
    DEBUG_DBMEM("%*d\t%s", 4, (int)ctx->counter, path);
    return rc;
}

static int dbmem_reindex (dbmem_context *ctx) {
    sqlite3 *db = ctx->db;
    int rc = SQLITE_OK;
    sqlite3_stmt *vm = NULL;
    int saved_dimension = ctx->dimension;
    bool saved_dimension_saved = ctx->dimension_saved;
    bool saved_vector_extension_available = ctx->vector_extension_available;
    bool reindex_started = false;

    // copy all content to a temp table
    sqlite3_exec(db, "DROP TABLE IF EXISTS dbmem_reindex;", NULL, NULL, NULL);
    rc = sqlite3_exec(db, "CREATE TEMP TABLE dbmem_reindex AS SELECT path, value, context FROM dbmem_content;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_exec(db, "SAVEPOINT dbmem_reindex;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    reindex_started = true;

    // clear all indexed data
    if (fts5_is_available) {
        rc = sqlite3_exec(db, "DELETE FROM dbmem_vault_fts;", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto cleanup;
    }
    rc = sqlite3_exec(db, "DELETE FROM dbmem_vault;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_exec(db, "DELETE FROM dbmem_content;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // reset dimension so the new model's dimension is auto-detected
    ctx->dimension = 0;
    ctx->dimension_saved = false;
    ctx->vector_extension_available = false;

    // iterate temp table one row at a time
    rc = sqlite3_prepare_v2(db, "SELECT path, value, context FROM dbmem_reindex;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(vm, 0);
        const char *value = (const char *)sqlite3_column_text(vm, 1);
        int value_len = sqlite3_column_bytes(vm, 1);
        const char *context = (const char *)sqlite3_column_text(vm, 2);

        dbmem_context_reset_temp_values(ctx);
        ctx->context = context;

        if (path && dbmem_file_exists(path)) {
            rc = dbmem_process_file(ctx, path);
        } else if (value && value_len > 0) {
            ctx->path = path;
            rc = dbmem_process_buffer(ctx, value, value_len);
        } else {
            rc = SQLITE_OK;
        }
        if (rc != SQLITE_OK) goto cleanup;
    }

    if (rc == SQLITE_DONE) rc = SQLITE_OK;

    if (rc == SQLITE_OK && reindex_started) {
        rc = sqlite3_exec(db, "RELEASE dbmem_reindex;", NULL, NULL, NULL);
        reindex_started = false;
    }

cleanup:
    if (vm) sqlite3_finalize(vm);
    if (rc != SQLITE_OK) {
        if (reindex_started) {
            sqlite3_exec(db, "ROLLBACK TO dbmem_reindex; RELEASE dbmem_reindex;", NULL, NULL, NULL);
        }
        ctx->dimension = saved_dimension;
        ctx->dimension_saved = saved_dimension_saved;
        ctx->vector_extension_available = saved_vector_extension_available;
    }
    dbmem_context_reset_temp_values(ctx);
    sqlite3_exec(db, "DROP TABLE IF EXISTS dbmem_reindex;", NULL, NULL, NULL);
    return rc;
}

static int dbmem_scan_callback (const char *path, void *data) {
    dbmem_context *ctx = (dbmem_context *)data;
    
    int rc = dbmem_process_file(ctx, path);
    if (rc == 0) ctx->counter++;
    
    return rc;
}

static void dbmem_add_text (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_text expects a parameter of type TEXT", SQLITE_ERROR);
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
static void dbmem_add_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_file expects the first parameter to be of type TEXT", SQLITE_ERROR);
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

static bool dbmem_path_is_under_directory (const char *path, const char *dir_path) {
    if (!path || !dir_path) return false;

    size_t dir_len = strlen(dir_path);
    if (dir_len == 0) return false;

    while (dir_len > 1 && (dir_path[dir_len - 1] == '/' || dir_path[dir_len - 1] == '\\')) {
        dir_len--;
    }

    if (dir_len == 1 && (dir_path[0] == '/' || dir_path[0] == '\\')) {
        return path[0] == dir_path[0];
    }

    if (strncmp(path, dir_path, dir_len) != 0) return false;
    if (path[dir_len] == '\0') return true;

    return (path[dir_len] == '/' || path[dir_len] == '\\');
}

static void dbmem_database_delete_missing_files (sqlite3 *db, const char *dir_path) {
    static const char *sql = "SELECT hash, path FROM dbmem_content WHERE path IS NOT NULL AND path != '';";
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return;

    rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(vm);
        return;
    }

    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        uint64_t hash = 0;
        const char *hash_text = (const char *)sqlite3_column_text(vm, 0);
        const char *path = (const char *)sqlite3_column_text(vm, 1);
        if (!dbmem_path_is_under_directory(path, dir_path)) continue;
        if (dbmem_file_exists(path)) continue;
        if (!dbmem_hash_from_hex(hash_text, &hash)) continue;
        dbmem_database_delete_hash(db, hash);
    }

    sqlite3_finalize(vm);

    if (rc == SQLITE_DONE || rc == SQLITE_OK) {
        dbmem_database_commit_transaction(db);
    } else {
        dbmem_database_rollback_transaction(db);
    }
}

static void dbmem_add_directory (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_directory expects the first parameter to be of type TEXT", SQLITE_ERROR);
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

// MARK: - Reindex

static void dbmem_sql_reindex (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    sqlite3 *db = ctx->db;

    if (!ctx->model) {
        sqlite3_result_error(context, "memory_reindex: no embedding model configured", -1);
        return;
    }

    // Process one row at a time: finalize read cursor before each write to avoid conflicts
    static const char *find_sql =
        "SELECT path, value, context FROM dbmem_content "
        "WHERE value IS NOT NULL AND hash NOT IN (SELECT DISTINCT hash FROM dbmem_vault) "
        "LIMIT 1;";

    ctx->reindex_mode = true;
    dbmem_context_reset_temp_values(ctx);

    int64_t processed = 0;
    int rc = SQLITE_OK;

    while (1) {
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db, find_sql, -1, &vm, NULL);
        if (rc != SQLITE_OK) break;

        int step = sqlite3_step(vm);
        if (step == SQLITE_DONE) {
            sqlite3_finalize(vm);
            break;
        }
        if (step != SQLITE_ROW) {
            sqlite3_finalize(vm);
            rc = step;
            break;
        }

        // Copy row data before finalizing so we can write in the next step
        const char *path_raw = (const char *)sqlite3_column_text(vm, 0);
        const char *value_raw = (const char *)sqlite3_column_text(vm, 1);
        int64_t value_len = (int64_t)sqlite3_column_bytes(vm, 1);
        const char *ctx_raw = (const char *)sqlite3_column_text(vm, 2);

        char *path = dbmem_strdup(path_raw);
        char *value = (char *)sqlite3_malloc64((sqlite3_uint64)(value_len + 1));
        if (value) { memcpy(value, value_raw, (size_t)value_len); value[value_len] = '\0'; }
        char *ctx_name = dbmem_strdup(ctx_raw);

        sqlite3_finalize(vm);

        if (!value) {
            dbmemory_free(path);
            dbmemory_free(ctx_name);
            rc = SQLITE_NOMEM;
            break;
        }

        ctx->path = path;
        ctx->context = ctx_name;
        rc = dbmem_process_buffer(ctx, value, value_len);

        // After CRDT sync the stored hash (PK) may differ from the hash computed
        // from the current value bytes. If so, update dbmem_content.hash so that:
        // (1) this row is excluded from future reindex loop iterations, and
        // (2) vector search JOINs on vault.hash = content.hash find the entry.
        if (rc == SQLITE_OK && path) {
            static const char *fix_sql =
                "UPDATE dbmem_content SET hash = ?1 WHERE path = ?2 AND hash != ?1;";
            sqlite3_stmt *fix_vm = NULL;
            if (sqlite3_prepare_v2(db, fix_sql, -1, &fix_vm, NULL) == SQLITE_OK) {
                dbmem_bind_hash(fix_vm, 1, ctx->hash);
                sqlite3_bind_text(fix_vm, 2, path, -1, SQLITE_STATIC);
                sqlite3_step(fix_vm);
                sqlite3_finalize(fix_vm);
            }
        }

        dbmemory_free(path);
        dbmemory_free(value);
        dbmemory_free(ctx_name);

        if (rc != SQLITE_OK) break;
        processed++;
    }

    ctx->reindex_mode = false;
    ctx->path = NULL;
    ctx->context = NULL;

    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, ctx->error_msg[0] ? ctx->error_msg : sqlite3_errmsg(db), -1);
        return;
    }

    sqlite3_result_int64(context, processed);
}

// MARK: - Sync

static void dbmem_enable_sync (sqlite3_context *context, int argc, sqlite3_value **argv) {
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);

    if (!dbmem_context_sync_available(ctx)) {
        sqlite3_result_error(context, "SQLite-sync must be loaded to enable memory syncing", -1);
        return;
    }

    for (int i = 0; i < argc; i++) {
        if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
            sqlite3_result_error(context, "memory_enable_sync: all arguments must be TEXT (context names)", -1);
            return;
        }
    }

    int rc = sqlite3_exec(db, "SELECT cloudsync_init('dbmem_content');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    rc = sqlite3_exec(db, "SELECT cloudsync_set_column('dbmem_content', 'value', 'algo', 'block');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    if (argc == 0) {
        // No context filter: clear any previously-set filter so all memory is synced
        rc = sqlite3_exec(db, "SELECT cloudsync_clear_filter('dbmem_content');", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(context, sqlite3_errmsg(db), -1);
            return;
        }
    } else {
        // Build: context IN ('ctx1','ctx2',...)
        char *filter = sqlite3_mprintf("context IN (");
        if (!filter) { sqlite3_result_error_nomem(context); return; }
        for (int i = 0; i < argc; i++) {
            const char *cname = (const char *)sqlite3_value_text(argv[i]);
            char *new_filter = (i < argc - 1)
                ? sqlite3_mprintf("%z'%q',", filter, cname)
                : sqlite3_mprintf("%z'%q')", filter, cname);
            filter = new_filter;
            if (!filter) { sqlite3_result_error_nomem(context); return; }
        }
        // Use %q to escape the filter string as a SQL literal passed to cloudsync_set_filter
        char *sql = sqlite3_mprintf("SELECT cloudsync_set_filter('dbmem_content', '%q');", filter);
        sqlite3_free(filter);
        if (!sql) { sqlite3_result_error_nomem(context); return; }
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(context, sqlite3_errmsg(db), -1);
            return;
        }
    }

    ctx->sync_enabled = true;
    sqlite3_result_int(context, 1);
}

static void dbmem_disable_sync (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    sqlite3 *db = sqlite3_context_db_handle(context);

    if (!dbmem_context_sync_available(ctx)) {
        sqlite3_result_error(context, "SQLite-sync must be loaded to disable memory syncing", -1);
        return;
    }

    int rc = sqlite3_exec(db, "SELECT cloudsync_cleanup('dbmem_content');", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    ctx->sync_enabled = false;
    sqlite3_result_int(context, 1);
}

// MARK: -

#define DBMEM_CTX_POINTER_TYPE "dbmem_context_ptr"

// helper to retrieve ctx pointer (registered during init)
static void dbmem_ctx_ptr (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    sqlite3_result_pointer(context, ctx, DBMEM_CTX_POINTER_TYPE, NULL);
}

SQLITE_DBMEMORY_API int sqlite3_memory_register_provider (sqlite3 *db, const char *provider_name, const dbmem_provider_t *provider) {
    if (!db || !provider_name || !provider || !provider->init || !provider->compute) return SQLITE_MISUSE;

    // retrieve dbmem_context from the helper function registered during init
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT _memory_ctx_ptr()", -1, &vm, NULL);
    if (rc != SQLITE_OK) return rc;

    if (sqlite3_step(vm) != SQLITE_ROW) {
        sqlite3_finalize(vm);
        return SQLITE_ERROR;
    }
    dbmem_context *ctx = (dbmem_context *)sqlite3_value_pointer(sqlite3_column_value(vm, 0), DBMEM_CTX_POINTER_TYPE);
    sqlite3_finalize(vm);
    if (!ctx) return SQLITE_ERROR;

    // free previous custom provider if any
    if (ctx->custom_engine && ctx->custom_provider.free) ctx->custom_provider.free(ctx->custom_engine, ctx->custom_provider.xdata);
    ctx->custom_engine = NULL;
    if (ctx->custom_provider_name) dbmemory_free(ctx->custom_provider_name);

    ctx->custom_provider_name = dbmem_strdup(provider_name);
    if (!ctx->custom_provider_name) return SQLITE_NOMEM;

    ctx->custom_provider = *provider;

    return SQLITE_OK;
}

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

    // Register all functions without destructor first; memory_version is registered last
    // so that ctx ownership only transfers to SQLite once all registrations succeed.
    rc = sqlite3_create_function_v2(db, "_memory_ctx_ptr", 0, SQLITE_UTF8, ctx, dbmem_ctx_ptr, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }
    
    rc = sqlite3_create_function_v2(db, "memory_set_option", 2, SQLITE_UTF8, ctx, dbmem_set_option, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_get_option", 1, SQLITE_UTF8, ctx, dbmem_get_option, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_set_model", 2, SQLITE_UTF8, ctx, dbmem_set_model, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_set_apikey", 1, SQLITE_UTF8, ctx, dbmem_set_apikey, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    #ifndef DBMEM_OMIT_IO
    rc = sqlite3_create_function_v2(db, "memory_add_file", 1, SQLITE_UTF8, ctx, dbmem_add_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_file", 2, SQLITE_UTF8, ctx, dbmem_add_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_directory", 1, SQLITE_UTF8, ctx, dbmem_add_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_directory", 2, SQLITE_UTF8, ctx, dbmem_add_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }
    #endif

    rc = sqlite3_create_function_v2(db, "memory_add_text", 1, SQLITE_UTF8, ctx, dbmem_add_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_text", 2, SQLITE_UTF8, ctx, dbmem_add_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_delete", 1, SQLITE_UTF8, ctx, dbmem_delete, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_delete_context", 1, SQLITE_UTF8, ctx, dbmem_delete_context, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_clear", 0, SQLITE_UTF8, ctx, dbmem_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_cache_clear", 0, SQLITE_UTF8, ctx, dbmem_cache_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_cache_clear", 2, SQLITE_UTF8, ctx, dbmem_cache_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_reindex", 0, SQLITE_UTF8, ctx, dbmem_sql_reindex, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_enable_sync", -1, SQLITE_UTF8, ctx, dbmem_enable_sync, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_disable_sync", 0, SQLITE_UTF8, ctx, dbmem_disable_sync, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = dbmem_register_search(db, ctx, pzErrMsg);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    // Register last: this transfers ctx ownership to SQLite (destructor fires on connection close).
    rc = sqlite3_create_function_v2(db, "memory_version", 0, SQLITE_UTF8, ctx, dbmem_version, NULL, NULL, dbmem_context_free);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    return SQLITE_OK;
}

//
//  sqlite-memory.c
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
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
#define DBMEM_SETTINGS_KEY_SCHEMA_VERSION       "schema_version"

#define DBMEM_SCHEMA_VERSION                    4

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
    const char  *path;                          // Portable relative file path (optional)
    const char  *source_path;                   // Local filesystem provenance for current operation (optional)
    const char  *root_path;                     // Filesystem root for current IO operation (optional)
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

static bool dbmem_database_column_exists (sqlite3 *db, const char *table, const char *column, int *out_rc) {
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) {
        if (out_rc) *out_rc = rc;
        return false;
    }

    bool exists = false;
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(vm, 1);
        if (name && strcmp(name, column) == 0) {
            exists = true;
            break;
        }
    }

    if (rc == SQLITE_DONE || rc == SQLITE_ROW) rc = SQLITE_OK;
    sqlite3_finalize(vm);
    if (out_rc) *out_rc = rc;
    return exists;
}

static int dbmem_database_add_column_if_missing (sqlite3 *db, const char *table, const char *column, const char *alter_sql) {
    int rc = SQLITE_OK;
    if (dbmem_database_column_exists(db, table, column, &rc)) return SQLITE_OK;
    if (rc != SQLITE_OK) return rc;
    return sqlite3_exec(db, alter_sql, NULL, NULL, NULL);
}

static bool dbmem_database_table_exists (sqlite3 *db, const char *table, int *out_rc) {
    static const char *sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) {
        if (out_rc) *out_rc = rc;
        return false;
    }

    rc = sqlite3_bind_text(vm, 1, table, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(vm);
        if (out_rc) *out_rc = rc;
        return false;
    }

    rc = sqlite3_step(vm);
    bool exists = rc == SQLITE_ROW;
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_OK;

    sqlite3_finalize(vm);
    if (out_rc) *out_rc = rc;
    return exists;
}

static int dbmem_database_schema_version (sqlite3 *db, int *version) {
    static const char *sql = "SELECT value FROM dbmem_settings WHERE key=?1 LIMIT 1;";

    *version = 0;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_text(vm, 1, DBMEM_SETTINGS_KEY_SCHEMA_VERSION, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        *version = sqlite3_column_int(vm, 0);
        rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }

cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static bool dbmem_database_is_enabled (sqlite3 *db, int *out_rc) {
    static const char *tables[] = {
        "dbmem_settings",
        "dbmem_content",
        "dbmem_content_source",
        "dbmem_vault",
        "dbmem_cache"
    };

    int rc = SQLITE_OK;
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        if (!dbmem_database_table_exists(db, tables[i], &rc)) {
            if (out_rc) *out_rc = rc;
            return false;
        }
        if (rc != SQLITE_OK) {
            if (out_rc) *out_rc = rc;
            return false;
        }
    }

    if (out_rc) *out_rc = rc;
    return true;
}

static int dbmem_database_set_schema_version (sqlite3 *db, int version) {
    return dbmem_settings_write_int(db, DBMEM_SETTINGS_KEY_SCHEMA_VERSION, version);
}

static int dbmem_database_begin_transaction (sqlite3 *db);
static int dbmem_database_commit_transaction (sqlite3 *db);
static int dbmem_database_rollback_transaction (sqlite3 *db);

static int dbmem_database_migrate_v1_to_v2 (sqlite3 *db) {
    int rc = dbmem_database_add_column_if_missing(db, "dbmem_vault", "n_tokens",
        "ALTER TABLE dbmem_vault ADD COLUMN n_tokens INTEGER NOT NULL DEFAULT 0;");
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_database_add_column_if_missing(db, "dbmem_vault", "truncated",
        "ALTER TABLE dbmem_vault ADD COLUMN truncated INTEGER NOT NULL DEFAULT 0;");
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_database_add_column_if_missing(db, "dbmem_cache", "n_tokens",
        "ALTER TABLE dbmem_cache ADD COLUMN n_tokens INTEGER NOT NULL DEFAULT 0;");
    if (rc != SQLITE_OK) return rc;

    return dbmem_database_add_column_if_missing(db, "dbmem_cache", "truncated",
        "ALTER TABLE dbmem_cache ADD COLUMN truncated INTEGER NOT NULL DEFAULT 0;");
}

static int dbmem_database_create_source_table (sqlite3 *db) {
    return sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS dbmem_content_source ("
        "path TEXT PRIMARY KEY NOT NULL, "
        "source_path TEXT NOT NULL UNIQUE"
        ");",
        NULL, NULL, NULL);
}

static int dbmem_database_migrate_v2_to_v3 (sqlite3 *db) {
    return dbmem_database_create_source_table(db);
}

static int dbmem_database_migrate_v3_to_v4 (sqlite3 *db) {
    int rc = dbmem_database_create_source_table(db);
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) return rc;

    sqlite3_int64 has_source_path = 0;
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM pragma_table_info('dbmem_content') WHERE name='source_path';",
        -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    if (sqlite3_step(vm) == SQLITE_ROW) has_source_path = sqlite3_column_int64(vm, 0);
    sqlite3_finalize(vm);
    vm = NULL;

    if (has_source_path == 0) {
        rc = dbmem_database_commit_transaction(db);
        return rc;
    }

    rc = sqlite3_exec(db,
        "INSERT OR REPLACE INTO dbmem_content_source (path, source_path) "
        "SELECT path, source_path FROM dbmem_content "
        "WHERE source_path IS NOT NULL AND source_path != '';",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;

    rc = sqlite3_exec(db,
        "ALTER TABLE dbmem_content RENAME TO dbmem_content_old;"
        "CREATE TABLE dbmem_content ("
        "hash TEXT PRIMARY KEY NOT NULL, "
        "path TEXT NOT NULL DEFAULT '' UNIQUE, "
        "value TEXT DEFAULT NULL, "
        "length INTEGER NOT NULL DEFAULT 0, "
        "context TEXT DEFAULT NULL, "
        "created_at INTEGER DEFAULT 0, "
        "last_accessed INTEGER DEFAULT 0"
        ");"
        "INSERT INTO dbmem_content (hash, path, value, length, context, created_at, last_accessed) "
        "SELECT hash, path, value, length, context, created_at, last_accessed FROM dbmem_content_old;"
        "DROP TABLE dbmem_content_old;",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto rollback;

    rc = dbmem_database_commit_transaction(db);
    return rc;

rollback:
    if (vm) sqlite3_finalize(vm);
    dbmem_database_rollback_transaction(db);
    return rc;
}

static int dbmem_database_migrate (sqlite3 *db) {
    int version = 0;
    int rc = dbmem_database_schema_version(db, &version);
    if (rc != SQLITE_OK) return rc;

    if (version > DBMEM_SCHEMA_VERSION) return SQLITE_MISMATCH;
    if (version <= 0) version = 1;

    if (version < 2) {
        rc = dbmem_database_migrate_v1_to_v2(db);
        if (rc != SQLITE_OK) return rc;
        version = 2;
        rc = dbmem_database_set_schema_version(db, version);
        if (rc != SQLITE_OK) return rc;
    }

    if (version < 3) {
        rc = dbmem_database_migrate_v2_to_v3(db);
        if (rc != SQLITE_OK) return rc;
        version = 3;
        rc = dbmem_database_set_schema_version(db, version);
        if (rc != SQLITE_OK) return rc;
    }

    if (version < 4) {
        rc = dbmem_database_migrate_v3_to_v4(db);
        if (rc != SQLITE_OK) return rc;
        version = 4;
        rc = dbmem_database_set_schema_version(db, version);
        if (rc != SQLITE_OK) return rc;
    }

    if (version != DBMEM_SCHEMA_VERSION) return SQLITE_MISMATCH;
    return SQLITE_OK;
}

static int dbmem_database_init (sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS dbmem_settings (key TEXT PRIMARY KEY, value TEXT);";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    sql = "CREATE TABLE IF NOT EXISTS dbmem_content (hash TEXT PRIMARY KEY NOT NULL, path TEXT NOT NULL DEFAULT '' UNIQUE, value TEXT DEFAULT NULL, length INTEGER NOT NULL DEFAULT 0, context TEXT DEFAULT NULL, created_at INTEGER DEFAULT 0, last_accessed INTEGER DEFAULT 0);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_database_create_source_table(db);
    if (rc != SQLITE_OK) return rc;

    sql = "CREATE TABLE IF NOT EXISTS dbmem_vault (hash TEXT NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, n_tokens INTEGER NOT NULL DEFAULT 0, truncated INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (hash, seq));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    sql = "CREATE TABLE IF NOT EXISTS dbmem_cache (text_hash TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL, embedding BLOB NOT NULL, dimension INTEGER NOT NULL, n_tokens INTEGER NOT NULL DEFAULT 0, truncated INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (text_hash, provider, model));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = dbmem_database_migrate(db);
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

static char *dbmem_database_path_for_hash_copy (sqlite3 *db, uint64_t hash) {
    sqlite3_stmt *vm = NULL;
    char *path = NULL;

    int rc = sqlite3_prepare_v2(db, "SELECT path FROM dbmem_content WHERE hash=?1 LIMIT 1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto cleanup;
    if (sqlite3_step(vm) == SQLITE_ROW) {
        path = dbmem_strdup((const char *)sqlite3_column_text(vm, 0));
    }

cleanup:
    if (vm) sqlite3_finalize(vm);
    return path;
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

    sqlite3_prepare_v2(db, "DELETE FROM dbmem_content_source WHERE path IN (SELECT path FROM dbmem_content WHERE hash=?1);", -1, &vm, NULL);
    dbmem_bind_hash(vm, 1, hash);
    sqlite3_step(vm);
    sqlite3_finalize(vm);

    sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash=?1;", -1, &vm, NULL);
    dbmem_bind_hash(vm, 1, hash);
    sqlite3_step(vm);
    sqlite3_finalize(vm);
}

static int dbmem_database_delete_index_hash (sqlite3 *db, uint64_t hash) {
    sqlite3_stmt *vm = NULL;
    int rc = SQLITE_OK;

    if (fts5_is_available) {
        rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash=?1;", -1, &vm, NULL);
        if (rc != SQLITE_OK) goto cleanup;
        rc = dbmem_bind_hash(vm, 1, hash);
        if (rc != SQLITE_OK) goto cleanup;
        rc = sqlite3_step(vm);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        sqlite3_finalize(vm);
        vm = NULL;
        if (rc != SQLITE_OK) goto cleanup;
    }

    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash=?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static bool dbmem_database_hash_has_vault (sqlite3 *db, uint64_t hash) {
    sqlite3_stmt *vm = NULL;
    bool found = false;

    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM dbmem_vault WHERE hash=?1 LIMIT 1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto cleanup;
    found = (sqlite3_step(vm) == SQLITE_ROW);

cleanup:
    if (vm) sqlite3_finalize(vm);
    return found;
}

static int dbmem_database_update_content_hash (sqlite3 *db, const char *path, uint64_t hash) {
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE dbmem_content SET hash = ?1 WHERE path = ?2 AND hash != ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_bind_text(vm, 2, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
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

static void dbmem_database_delete_stale_source_path (sqlite3 *db, const char *source_path, uint64_t new_hash) {
    if (!source_path) return;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT c.hash FROM dbmem_content c "
        "JOIN dbmem_content_source s ON s.path = c.path "
        "WHERE s.source_path=?1;",
        -1, &vm, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_bind_text(vm, 1, source_path, -1, SQLITE_STATIC);
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

static int dbmem_database_set_source_path (sqlite3 *db, const char *path, const char *source_path) {
    if (!path || !path[0] || !source_path || !source_path[0]) return SQLITE_OK;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO dbmem_content_source (path, source_path) VALUES (?1, ?2);",
        -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_text(vm, 1, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_bind_text(vm, 2, source_path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
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
    if (rc != SQLITE_OK) goto cleanup;

    sqlite3_finalize(vm);
    vm = NULL;

    rc = dbmem_database_set_source_path(db, path, ctx->source_path);

cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM_ALWAYS("Error in dbmem_database_add_entry: %s", sqlite3_errmsg(ctx->db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

static int dbmem_database_add_chunk (dbmem_context *ctx, embedding_result_t *result, size_t offset, size_t length, size_t index) {
    static const char *sql = "INSERT INTO dbmem_vault (hash, seq, embedding, offset, length, n_tokens, truncated) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";

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

    rc = sqlite3_bind_int(vm, 6, result->n_tokens);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_int(vm, 7, result->truncated ? 1 : 0);
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
    ctx->source_path = NULL;
    ctx->root_path = NULL;
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
        dbmem_context_set_error(ctx, "memory_search cannot run because no content has been indexed yet. Add content with memory_add_text(), memory_add_file(), memory_add_content(), or memory_add_directory() before searching.");
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

// MARK: - Status -

static void dbmem_is_enabled (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);

    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc = SQLITE_OK;
    bool enabled = dbmem_database_is_enabled(db, &rc);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    sqlite3_result_int(context, enabled ? 1 : 0);
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

    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content_source WHERE path IN (SELECT path FROM dbmem_content WHERE hash = ?1);", -1, &vm, NULL);
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

    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content_source WHERE path IN (SELECT path FROM dbmem_content WHERE context = ?1);", -1, &vm, NULL);
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

static int dbmem_resolve_content_hash_for_path (sqlite3 *db, const char *path, uint64_t *hash, int *matches) {
    static const char *sql =
        "SELECT c.hash FROM dbmem_content c "
        "LEFT JOIN dbmem_content_source s ON s.path = c.path "
        "WHERE c.path = ?1 OR s.source_path = ?1;";

    *matches = 0;
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_bind_text(vm, 1, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(vm);
        return rc;
    }

    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        (*matches)++;
        if (*matches == 1 && !dbmem_column_hash(vm, 0, hash)) {
            rc = SQLITE_MISMATCH;
            break;
        }
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

    sqlite3_finalize(vm);
    return rc;
}

static void dbmem_delete_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT || sqlite3_value_bytes(argv[0]) == 0) {
        sqlite3_result_error(context, "The function memory_delete_file expects one non-empty TEXT argument (path)", SQLITE_ERROR);
        return;
    }

    const char *path = (const char *)sqlite3_value_text(argv[0]);
    sqlite3 *db = sqlite3_context_db_handle(context);
    uint64_t hash = 0;
    int matches = 0;

    int rc = dbmem_resolve_content_hash_for_path(db, path, &hash, &matches);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }
    if (matches == 0) {
        sqlite3_result_int(context, 0);
        return;
    }
    if (matches > 1) {
        sqlite3_result_error(context, "memory_delete_file matched more than one row; use a unique logical path or exact local source_path", -1);
        return;
    }

    rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }

    // Delete from FTS first (if available)
    if (fts5_is_available) {
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault_fts WHERE hash=?1;", -1, &vm, NULL);
        if (rc == SQLITE_OK) {
            dbmem_bind_hash(vm, 1, hash);
            sqlite3_step(vm);
            sqlite3_finalize(vm);
        }
    }

    // Delete from vault
    sqlite3_stmt *vm = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_vault WHERE hash=?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    dbmem_bind_hash(vm, 1, hash);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content_source WHERE path IN (SELECT path FROM dbmem_content WHERE hash=?1);", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    dbmem_bind_hash(vm, 1, hash);
    rc = sqlite3_step(vm);
    sqlite3_finalize(vm);
    if (rc != SQLITE_DONE) goto rollback;

    // Delete from content
    rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_content WHERE hash=?1;", -1, &vm, NULL);
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

    rc = sqlite3_exec(db, "DELETE FROM dbmem_content_source;", NULL, NULL, NULL);
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

// MARK: - File Rename -

static void dbmem_rename_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT || sqlite3_value_type(argv[1]) != SQLITE_TEXT ||
        sqlite3_value_bytes(argv[0]) == 0 || sqlite3_value_bytes(argv[1]) == 0) {
        sqlite3_result_error(context, "The function memory_rename_file expects two non-empty TEXT arguments (old_path, new_path)", SQLITE_ERROR);
        return;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *old_path = (const char *)sqlite3_value_text(argv[0]);
    const char *new_path = (const char *)sqlite3_value_text(argv[1]);
    uint64_t hash = 0;
    int matches = 0;

    int rc = dbmem_resolve_content_hash_for_path(db, old_path, &hash, &matches);
    if (rc != SQLITE_OK) {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
        return;
    }
    if (matches == 0) {
        sqlite3_result_int(context, 0);
        return;
    }
    if (matches > 1) {
        sqlite3_result_error(context, "memory_rename_file matched more than one row; use a unique logical path or exact local source_path", -1);
        return;
    }

    sqlite3_stmt *vm = NULL;
    rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_prepare_v2(db, "UPDATE dbmem_content SET path = ?2 WHERE hash = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    rc = dbmem_bind_hash(vm, 1, hash);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_bind_text(vm, 2, new_path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto rollback;

    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) goto rollback;
    int changes = sqlite3_changes(db);
    sqlite3_finalize(vm);
    vm = NULL;

    rc = sqlite3_prepare_v2(db, "UPDATE dbmem_content_source SET path = ?2 WHERE path = ?1 OR source_path = ?1;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_bind_text(vm, 1, old_path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_bind_text(vm, 2, new_path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto rollback;
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) goto rollback;

    dbmem_database_commit_transaction(db);
    if (vm) sqlite3_finalize(vm);
    sqlite3_result_int(context, changes);
    return;

rollback:
    dbmem_database_rollback_transaction(db);

cleanup:
    if (vm) sqlite3_finalize(vm);
    sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

// MARK: - Path Listing -

typedef struct {
    char **items;
    int count;
    int capacity;
} dbmem_string_list;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} dbmem_json_buffer;

static bool dbmem_path_separator (char c) {
    return c == '/' || c == '\\';
}

static bool dbmem_path_is_absolute (const char *path) {
    if (!path || !path[0]) return false;
    if (dbmem_path_separator(path[0])) return true;
    return (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && dbmem_path_separator(path[2]));
}

static void dbmem_string_list_free (dbmem_string_list *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        dbmemory_free(list->items[i]);
    }
    if (list->items) dbmemory_free(list->items);
    memset(list, 0, sizeof(*list));
}

static int dbmem_string_list_add (dbmem_string_list *list, char *value) {
    if (!value) return SQLITE_NOMEM;

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity ? list->capacity * 2 : 8;
        char **new_items = (char **)dbmemory_realloc(list->items, (uint64_t)new_capacity * sizeof(char *));
        if (!new_items) {
            dbmemory_free(value);
            return SQLITE_NOMEM;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = value;
    return SQLITE_OK;
}

static size_t dbmem_common_directory_prefix_len (dbmem_string_list *paths) {
    if (paths->count == 0) return 0;

    for (int i = 0; i < paths->count; i++) {
        if (!dbmem_path_is_absolute(paths->items[i])) return 0;
    }

    size_t common_len = strlen(paths->items[0]);
    for (int i = 1; i < paths->count; i++) {
        size_t j = 0;
        while (j < common_len && paths->items[i][j] && paths->items[0][j] == paths->items[i][j]) {
            j++;
        }
        common_len = j;
    }

    size_t prefix_len = 0;
    for (size_t i = 0; i < common_len; i++) {
        if (dbmem_path_separator(paths->items[0][i])) prefix_len = i + 1;
    }

    if (paths->count == 1) {
        size_t len = strlen(paths->items[0]);
        prefix_len = 0;
        for (size_t i = 0; i < len; i++) {
            if (dbmem_path_separator(paths->items[0][i])) prefix_len = i + 1;
        }
    }

    return prefix_len;
}

static char *dbmem_path_copy_normalized (const char *path, size_t prefix_len) {
    size_t len = strlen(path);
    if (prefix_len > len) prefix_len = 0;

    const char *relative = path + prefix_len;
    size_t relative_len = strlen(relative);
    char *copy = (char *)dbmemory_alloc((uint64_t)relative_len + 1);
    if (!copy) return NULL;

    for (size_t i = 0; i < relative_len; i++) {
        copy[i] = dbmem_path_separator(relative[i]) ? '/' : relative[i];
    }
    copy[relative_len] = '\0';
    return copy;
}

static int dbmem_json_buffer_reserve (dbmem_json_buffer *json, size_t extra) {
    if (extra > SIZE_MAX - json->length - 1) return SQLITE_NOMEM;
    size_t needed = json->length + extra + 1;
    if (needed <= json->capacity) return SQLITE_OK;

    size_t new_capacity = json->capacity ? json->capacity * 2 : 128;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }

    char *new_data = (char *)dbmemory_realloc(json->data, (uint64_t)new_capacity);
    if (!new_data) return SQLITE_NOMEM;

    json->data = new_data;
    json->capacity = new_capacity;
    return SQLITE_OK;
}

static int dbmem_json_buffer_append_len (dbmem_json_buffer *json, const char *text, size_t len) {
    int rc = dbmem_json_buffer_reserve(json, len);
    if (rc != SQLITE_OK) return rc;

    memcpy(json->data + json->length, text, len);
    json->length += len;
    json->data[json->length] = '\0';
    return SQLITE_OK;
}

static int dbmem_json_buffer_append (dbmem_json_buffer *json, const char *text) {
    return dbmem_json_buffer_append_len(json, text, strlen(text));
}

static int dbmem_json_buffer_append_char (dbmem_json_buffer *json, char c) {
    return dbmem_json_buffer_append_len(json, &c, 1);
}

static int dbmem_json_buffer_append_escaped_len (dbmem_json_buffer *json, const char *text, size_t len) {
    static const char hex[] = "0123456789abcdef";
    int rc = dbmem_json_buffer_append_char(json, '"');
    if (rc != SQLITE_OK) return rc;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        char escaped[6];
        switch (c) {
            case '"': rc = dbmem_json_buffer_append(json, "\\\""); break;
            case '\\': rc = dbmem_json_buffer_append(json, "\\\\"); break;
            case '\b': rc = dbmem_json_buffer_append(json, "\\b"); break;
            case '\f': rc = dbmem_json_buffer_append(json, "\\f"); break;
            case '\n': rc = dbmem_json_buffer_append(json, "\\n"); break;
            case '\r': rc = dbmem_json_buffer_append(json, "\\r"); break;
            case '\t': rc = dbmem_json_buffer_append(json, "\\t"); break;
            default:
                if (c < 0x20) {
                    escaped[0] = '\\';
                    escaped[1] = 'u';
                    escaped[2] = '0';
                    escaped[3] = '0';
                    escaped[4] = hex[c >> 4];
                    escaped[5] = hex[c & 0x0f];
                    rc = dbmem_json_buffer_append_len(json, escaped, sizeof(escaped));
                } else {
                    rc = dbmem_json_buffer_append_char(json, (char)c);
                }
                break;
        }
        if (rc != SQLITE_OK) return rc;
    }

    return dbmem_json_buffer_append_char(json, '"');
}

static int dbmem_json_buffer_append_escaped (dbmem_json_buffer *json, const char *text) {
    return dbmem_json_buffer_append_escaped_len(json, text, strlen(text));
}

static size_t dbmem_path_segment_start (const char *path, size_t offset) {
    while (path[offset] == '/') offset++;
    return offset;
}

static size_t dbmem_path_segment_end (const char *path, size_t start) {
    while (path[start] && path[start] != '/') start++;
    return start;
}

static bool dbmem_path_has_more_segments (const char *path, size_t segment_end) {
    return path[dbmem_path_segment_start(path, segment_end)] != '\0';
}

static int dbmem_segment_compare (const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t min_len = (a_len < b_len) ? a_len : b_len;
    int cmp = memcmp(a, b, min_len);
    if (cmp != 0) return cmp;
    if (a_len == b_len) return 0;
    return (a_len < b_len) ? -1 : 1;
}

static bool dbmem_same_segment (const char *a, size_t a_start, size_t a_end, const char *b, size_t b_start, size_t b_end) {
    size_t a_len = a_end - a_start;
    size_t b_len = b_end - b_start;
    return a_len == b_len && memcmp(a + a_start, b + b_start, a_len) == 0;
}

static int dbmem_path_tree_compare (const void *a, const void *b) {
    const char *pa = *(const char * const *)a;
    const char *pb = *(const char * const *)b;
    size_t ia = 0;
    size_t ib = 0;

    while (true) {
        ia = dbmem_path_segment_start(pa, ia);
        ib = dbmem_path_segment_start(pb, ib);

        size_t ea = dbmem_path_segment_end(pa, ia);
        size_t eb = dbmem_path_segment_end(pb, ib);
        bool enda = ia == ea;
        bool endb = ib == eb;
        if (enda || endb) {
            if (enda == endb) return 0;
            return enda ? -1 : 1;
        }

        int cmp = dbmem_segment_compare(pa + ia, ea - ia, pb + ib, eb - ib);
        if (cmp != 0) return cmp;

        bool morea = dbmem_path_has_more_segments(pa, ea);
        bool moreb = dbmem_path_has_more_segments(pb, eb);
        if (!morea || !moreb) {
            if (morea == moreb) return 0;
            return morea ? -1 : 1;
        }

        ia = ea;
        ib = eb;
    }
}

static int dbmem_path_group_end (dbmem_string_list *paths, int start, int end, size_t offset) {
    const char *first = paths->items[start];
    size_t first_start = dbmem_path_segment_start(first, offset);
    size_t first_end = dbmem_path_segment_end(first, first_start);
    int i = start + 1;

    while (i < end) {
        const char *path = paths->items[i];
        size_t segment_start = dbmem_path_segment_start(path, offset);
        size_t segment_end = dbmem_path_segment_end(path, segment_start);
        if (!dbmem_same_segment(first, first_start, first_end, path, segment_start, segment_end)) break;
        i++;
    }

    return i;
}

static bool dbmem_path_group_has_child (dbmem_string_list *paths, int start, int end, size_t offset) {
    for (int i = start; i < end; i++) {
        const char *path = paths->items[i];
        size_t segment_start = dbmem_path_segment_start(path, offset);
        size_t segment_end = dbmem_path_segment_end(path, segment_start);
        if (segment_start != segment_end && dbmem_path_has_more_segments(path, segment_end)) return true;
    }
    return false;
}

static int dbmem_path_group_file_index (dbmem_string_list *paths, int start, int end, size_t offset) {
    for (int i = start; i < end; i++) {
        const char *path = paths->items[i];
        size_t segment_start = dbmem_path_segment_start(path, offset);
        size_t segment_end = dbmem_path_segment_end(path, segment_start);
        if (segment_start != segment_end && !dbmem_path_has_more_segments(path, segment_end)) return i;
    }
    return -1;
}

static int dbmem_json_append_file_node (dbmem_json_buffer *json, const char *path, size_t segment_start, size_t segment_end) {
    int rc = dbmem_json_buffer_append(json, "{\"type\":\"file\",\"name\":");
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append_escaped_len(json, path + segment_start, segment_end - segment_start);
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append(json, ",\"path\":");
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append_escaped(json, path);
    if (rc != SQLITE_OK) return rc;
    return dbmem_json_buffer_append_char(json, '}');
}

static int dbmem_json_append_tree_children (dbmem_json_buffer *json, dbmem_string_list *paths, int start, int end, size_t offset);

static int dbmem_json_append_directory_node (dbmem_json_buffer *json, dbmem_string_list *paths, int start, int end, size_t offset) {
    const char *path = paths->items[start];
    size_t segment_start = dbmem_path_segment_start(path, offset);
    size_t segment_end = dbmem_path_segment_end(path, segment_start);

    int rc = dbmem_json_buffer_append(json, "{\"type\":\"directory\",\"name\":");
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append_escaped_len(json, path + segment_start, segment_end - segment_start);
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append(json, ",\"path\":");
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append_escaped_len(json, path, segment_end);
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_buffer_append(json, ",\"children\":");
    if (rc != SQLITE_OK) return rc;
    rc = dbmem_json_append_tree_children(json, paths, start, end, segment_end);
    if (rc != SQLITE_OK) return rc;
    return dbmem_json_buffer_append_char(json, '}');
}

static int dbmem_json_append_tree_children (dbmem_json_buffer *json, dbmem_string_list *paths, int start, int end, size_t offset) {
    int rc = dbmem_json_buffer_append_char(json, '[');
    if (rc != SQLITE_OK) return rc;

    bool first = true;
    for (int pass = 0; pass < 2; pass++) {
        int i = start;
        while (i < end) {
            const char *path = paths->items[i];
            size_t segment_start = dbmem_path_segment_start(path, offset);
            if (path[segment_start] == '\0') {
                i++;
                continue;
            }

            int group_end = dbmem_path_group_end(paths, i, end, offset);
            bool emit_directory = (pass == 0 && dbmem_path_group_has_child(paths, i, group_end, offset));
            int file_index = (pass == 1) ? dbmem_path_group_file_index(paths, i, group_end, offset) : -1;
            bool emit_file = file_index >= 0;

            if (emit_directory || emit_file) {
                if (!first) {
                    rc = dbmem_json_buffer_append_char(json, ',');
                    if (rc != SQLITE_OK) return rc;
                }
                first = false;

                if (emit_directory) {
                    rc = dbmem_json_append_directory_node(json, paths, i, group_end, offset);
                } else {
                    const char *file_path = paths->items[file_index];
                    segment_start = dbmem_path_segment_start(file_path, offset);
                    size_t segment_end = dbmem_path_segment_end(file_path, segment_start);
                    rc = dbmem_json_append_file_node(json, file_path, segment_start, segment_end);
                }
                if (rc != SQLITE_OK) return rc;
            }

            i = group_end;
        }
    }

    return dbmem_json_buffer_append_char(json, ']');
}

static int dbmem_paths_to_json (dbmem_string_list *paths, char **result) {
    dbmem_json_buffer json = {0};
    int rc = SQLITE_OK;
    size_t prefix_len = dbmem_common_directory_prefix_len(paths);

    for (int i = 0; i < paths->count; i++) {
        char *normalized = dbmem_path_copy_normalized(paths->items[i], prefix_len);
        if (!normalized) { rc = SQLITE_NOMEM; goto cleanup; }

        dbmemory_free(paths->items[i]);
        paths->items[i] = normalized;
    }

    if (paths->count > 1) {
        qsort(paths->items, (size_t)paths->count, sizeof(char *), dbmem_path_tree_compare);
    }

    rc = dbmem_json_buffer_append(&json, "{\"root\":\"\",\"children\":");
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_json_append_tree_children(&json, paths, 0, paths->count, 0);
    if (rc != SQLITE_OK) goto cleanup;
    rc = dbmem_json_buffer_append_char(&json, '}');
    if (rc != SQLITE_OK) goto cleanup;

    *result = json.data;
    json.data = NULL;

cleanup:
    if (json.data) dbmemory_free(json.data);
    return rc;
}

static void dbmem_list_files (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc); UNUSED_PARAM(argv);

    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_stmt *vm = NULL;
    dbmem_string_list paths = {0};
    char *json = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT path FROM dbmem_content WHERE path IS NOT NULL AND path != '';",
        -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(vm, 0);
        char *copy = dbmem_strdup(path);
        rc = dbmem_string_list_add(&paths, copy);
        if (rc != SQLITE_OK) goto cleanup;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc != SQLITE_OK) goto cleanup;

    rc = dbmem_paths_to_json(&paths, &json);

cleanup:
    if (vm) sqlite3_finalize(vm);
    dbmem_string_list_free(&paths);

    if (rc == SQLITE_OK) {
        sqlite3_result_text(context, json ? json : "{\"root\":\"\",\"children\":[]}", -1, json ? dbmemory_free : SQLITE_TRANSIENT);
    } else if (rc == SQLITE_NOMEM) {
        if (json) dbmemory_free(json);
        sqlite3_result_error_nomem(context);
    } else {
        if (json) dbmemory_free(json);
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    }
}

// MARK: - Cache Clear -

static int dbmem_cache_clear_provider_model(sqlite3 *db, const char *provider, const char *model) {
    static const char *sql = "DELETE FROM dbmem_cache WHERE provider=?1 AND model=?2;";
    if (!provider || !model) return SQLITE_OK;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_text(vm, 1, provider, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_bind_text(vm, 2, model, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;

    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

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

        rc = dbmem_cache_clear_provider_model(db, provider, model);
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

        int max_context_tokens = (int)(ctx->max_tokens + ctx->overlay_tokens);
        new_l_engine = dbmem_local_engine_init(ctx, model, max_context_tokens, ctx->error_msg);
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

static bool dbmem_is_local_context_option(const char *key) {
    return (strcasecmp(key, DBMEM_SETTINGS_KEY_MAX_TOKENS) == 0 ||
            strcasecmp(key, DBMEM_SETTINGS_KEY_OVERLAY_TOKENS) == 0);
}

static int dbmem_rebuild_local_engine_for_context_options(dbmem_context *ctx) {
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (!ctx || !ctx->is_local || ctx->is_custom || !ctx->model || !ctx->l_engine) {
        return SQLITE_OK;
    }

    int max_context_tokens = (int)(ctx->max_tokens + ctx->overlay_tokens);
    dbmem_local_engine_t *new_l_engine = dbmem_local_engine_init(ctx, ctx->model, max_context_tokens, ctx->error_msg);
    if (new_l_engine == NULL) {
        return SQLITE_ERROR;
    }

    if (ctx->engine_warmup) {
        dbmem_local_engine_warmup(new_l_engine);
    }

    int rc = dbmem_cache_clear_provider_model(ctx->db, ctx->provider, ctx->model);
    if (rc != SQLITE_OK) {
        dbmem_local_engine_free(new_l_engine);
        return rc;
    }

    dbmem_local_engine_free(ctx->l_engine);
    ctx->l_engine = new_l_engine;
    #else
    UNUSED_PARAM(ctx);
    #endif

    return SQLITE_OK;
}

static void dbmem_set_option (sqlite3_context *context, int argc, sqlite3_value **argv) {
    UNUSED_PARAM(argc);

    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_set_option expects the key argument to be of type TEXT", SQLITE_ERROR);
        return;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);
    const char *key = (const char *)sqlite3_value_text(argv[0]);

    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    ctx->error_msg[0] = 0;

    bool context_option = dbmem_is_local_context_option(key);
    size_t old_max_tokens = ctx->max_tokens;
    size_t old_overlay_tokens = ctx->overlay_tokens;

    int rc = sqlite3_exec(db, "SAVEPOINT dbmem_set_option;", NULL, NULL, NULL);
    bool savepoint_started = (rc == SQLITE_OK);

    if (rc == SQLITE_OK) {
        rc = dbmem_settings_write_value(db, key, argv[1]);
    }

    if (rc == SQLITE_OK) {
        dbmem_settings_sync(ctx, key, argv[1]);
    }

    if (rc == SQLITE_OK && context_option &&
        (old_max_tokens != ctx->max_tokens || old_overlay_tokens != ctx->overlay_tokens)) {
        rc = dbmem_rebuild_local_engine_for_context_options(ctx);
    }

    if (rc == SQLITE_OK && savepoint_started) {
        rc = sqlite3_exec(db, "RELEASE dbmem_set_option;", NULL, NULL, NULL);
        savepoint_started = false;
    }

    if (rc != SQLITE_OK) {
        if (savepoint_started) {
            sqlite3_exec(db, "ROLLBACK TO dbmem_set_option; RELEASE dbmem_set_option;", NULL, NULL, NULL);
        }
        if (context_option) {
            ctx->max_tokens = old_max_tokens;
            ctx->overlay_tokens = old_overlay_tokens;
        }
    }

    (rc == SQLITE_OK) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg[0] ? ctx->error_msg : sqlite3_errmsg(db), -1);
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
    static const char *sql = "SELECT embedding, dimension, n_tokens, truncated FROM dbmem_cache WHERE text_hash=?1 AND provider=?2 AND model=?3 LIMIT 1;";

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
    result->n_tokens = sqlite3_column_int(vm, 2);
    result->truncated = sqlite3_column_int(vm, 3) != 0;
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
    static const char *sql = "INSERT OR REPLACE INTO dbmem_cache (text_hash, provider, model, embedding, dimension, n_tokens, truncated) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";

    if (!ctx->provider || !ctx->model) return;

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    dbmem_bind_hash(vm, 1, text_hash);
    sqlite3_bind_text(vm, 2, ctx->provider, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 3, ctx->model, -1, SQLITE_STATIC);
    sqlite3_bind_blob(vm, 4, result->embedding, result->n_embd * (int)sizeof(float), SQLITE_STATIC);
    sqlite3_bind_int(vm, 5, result->n_embd);
    sqlite3_bind_int(vm, 6, result->n_tokens);
    sqlite3_bind_int(vm, 7, result->truncated ? 1 : 0);

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
        if (!ctx->provider || !ctx->model) {
            dbmem_context_set_error(ctx, "memory_set_model must be called before adding content");
            return SQLITE_ERROR;
        }

        // compute embedding
        if (ctx->is_custom) {
            if (!ctx->custom_engine || !ctx->custom_provider.compute) {
                dbmem_context_set_error(ctx, "memory_set_model must be called before adding content");
                return SQLITE_ERROR;
            }
            rc = dbmem_context_custom_compute(ctx, text, (int)len, &result);
            if (rc != 0) return rc;
        }

        else if (ctx->is_local) {
        #ifndef DBMEM_OMIT_LOCAL_ENGINE
            if (!ctx->l_engine) {
                dbmem_context_set_error(ctx, "memory_set_model must be called before adding content");
                return SQLITE_ERROR;
            }
            rc = dbmem_local_compute_embedding(ctx->l_engine, text, (int)len, &result);
            if (rc != 0) return rc;
        #else
            dbmem_context_set_error(ctx, "Local embedding cannot be computed because extension was compiled without local engine support");
            return 1;
        #endif
        }

        else {
        #ifndef DBMEM_OMIT_REMOTE_ENGINE
            if (!ctx->r_engine) {
                dbmem_context_set_error(ctx, "memory_set_model must be called before adding content");
                return SQLITE_ERROR;
            }
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

static char *dbmem_path_unique_storage_copy (sqlite3 *db, const char *preferred_path, const char *source_path);

static int dbmem_process_buffer (dbmem_context *ctx, const char *buffer, int64_t len) {
    uint64_t hash = dbmem_hash_compute(buffer, (size_t)len);
    const char *saved_path = ctx->path;
    char *unique_path = NULL;
    bool transaction_started = false;

    if (!ctx->reindex_mode && ctx->path) {
        unique_path = dbmem_path_unique_storage_copy(ctx->db, ctx->path, ctx->source_path);
        if (!unique_path) return SQLITE_NOMEM;
        ctx->path = unique_path;
    }

    sqlite3 *db = ctx->db;
    int rc = dbmem_database_begin_transaction(db);
    if (rc != SQLITE_OK) goto cleanup;
    transaction_started = true;

    if (!ctx->reindex_mode) {
        if (ctx->source_path) {
            dbmem_database_delete_stale_source_path(db, ctx->source_path, hash);
        }
        dbmem_database_delete_stale_path(db, ctx->path, hash);

        if (dbmem_database_check_if_stored(ctx->db, hash, len)) {
            if (ctx->source_path) {
                char *stored_path = dbmem_database_path_for_hash_copy(ctx->db, hash);
                if (!stored_path) {
                    rc = SQLITE_NOMEM;
                    goto cleanup;
                }
                rc = dbmem_database_set_source_path(ctx->db, stored_path, ctx->source_path);
                dbmemory_free(stored_path);
            }
            goto cleanup;
        }
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

    if (!ctx->reindex_mode) {
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
    if (transaction_started) {
        int tx_rc = (rc == SQLITE_OK) ? dbmem_database_commit_transaction(db) : dbmem_database_rollback_transaction(db);
        if (rc == SQLITE_OK) rc = tx_rc;
    }
    ctx->path = saved_path;
    if (unique_path) dbmemory_free(unique_path);
    return rc;
}

static size_t dbmem_path_trimmed_len (const char *path) {
    size_t len = path ? strlen(path) : 0;
    while (len > 0 && dbmem_path_separator(path[len - 1])) len--;
    return len;
}

static const char *dbmem_path_basename_ptr (const char *path, size_t len) {
    if (!path) return NULL;
    while (len > 0 && dbmem_path_separator(path[len - 1])) len--;
    for (size_t i = len; i > 0; i--) {
        if (dbmem_path_separator(path[i - 1])) return path + i;
    }
    return path;
}

static char *dbmem_path_suffix_copy (const char *path, int components) {
    if (!path || components <= 0) return NULL;

    size_t end = dbmem_path_trimmed_len(path);
    size_t start = end;
    int found = 0;

    while (start > 0 && found < components) {
        while (start > 0 && dbmem_path_separator(path[start - 1])) start--;
        while (start > 0 && !dbmem_path_separator(path[start - 1])) start--;
        found++;
    }

    while (start < end && dbmem_path_separator(path[start])) start++;
    if (start >= end) return NULL;

    size_t len = end - start;
    char *copy = (char *)dbmemory_alloc((uint64_t)len + 1);
    if (!copy) return NULL;
    for (size_t i = 0; i < len; i++) {
        copy[i] = dbmem_path_separator(path[start + i]) ? '/' : path[start + i];
    }
    copy[len] = '\0';
    return copy;
}

static int dbmem_path_component_count (const char *path) {
    if (!path) return 0;

    size_t len = dbmem_path_trimmed_len(path);
    size_t i = 0;
    int count = 0;

    while (i < len) {
        while (i < len && dbmem_path_separator(path[i])) i++;
        if (i >= len) break;
        count++;
        while (i < len && !dbmem_path_separator(path[i])) i++;
    }

    return count;
}

static const char *dbmem_path_relative_ptr (const char *path, const char *root, size_t *out_len) {
    size_t path_len = dbmem_path_trimmed_len(path);
    if (out_len) *out_len = path_len;
    if (!path) return NULL;

    if (root && root[0]) {
        size_t root_len = dbmem_path_trimmed_len(root);
        if (root_len > 0 && strncmp(path, root, root_len) == 0 &&
            (path[root_len] == '\0' || dbmem_path_separator(path[root_len]))) {
            const char *relative = path + root_len;
            if (dbmem_path_separator(*relative)) relative++;
            if (out_len) *out_len = path_len - (size_t)(relative - path);
            return relative;
        }
    }

    if (dbmem_path_is_absolute(path)) {
        const char *base = dbmem_path_basename_ptr(path, path_len);
        if (out_len) *out_len = path_len - (size_t)(base - path);
        return base;
    }

    return path;
}

static char *dbmem_path_storage_copy (const char *path, const char *root) {
    if (path && (!root || !root[0]) && dbmem_path_is_absolute(path)) {
        char *suffix = dbmem_path_suffix_copy(path, 2);
        if (suffix) return suffix;
    }

    size_t len = 0;
    const char *relative = dbmem_path_relative_ptr(path, root, &len);
    if (!relative) return NULL;

    while (len > 0 && dbmem_path_separator(*relative)) {
        relative++;
        len--;
    }
    while (len > 0 && dbmem_path_separator(relative[len - 1])) len--;

    if (len == 0 && path) {
        relative = dbmem_path_basename_ptr(path, dbmem_path_trimmed_len(path));
        len = strlen(relative);
    }

    char *copy = (char *)dbmemory_alloc((uint64_t)len + 1);
    if (!copy) return NULL;
    for (size_t i = 0; i < len; i++) {
        copy[i] = dbmem_path_separator(relative[i]) ? '/' : relative[i];
    }
    copy[len] = '\0';
    return copy;
}

static bool dbmem_database_path_conflicts (sqlite3 *db, const char *path, const char *source_path) {
    static const char *sql =
        "SELECT s.source_path FROM dbmem_content c "
        "LEFT JOIN dbmem_content_source s ON s.path = c.path "
        "WHERE c.path=?1 LIMIT 1;";
    sqlite3_stmt *vm = NULL;
    bool conflict = false;

    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return true;

    sqlite3_bind_text(vm, 1, path, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc == SQLITE_ROW) {
        const char *stored_source_path = (const char *)sqlite3_column_text(vm, 0);
        conflict = source_path && stored_source_path && strcmp(source_path, stored_source_path) != 0;
    } else {
        conflict = false;
    }

    sqlite3_finalize(vm);
    return conflict;
}

static char *dbmem_path_disambiguated_copy (const char *source_path, int components) {
    char *suffix = dbmem_path_suffix_copy(source_path, components);
    if (suffix) return suffix;

    size_t len = strlen(source_path);
    uint64_t hash = dbmem_hash_compute(source_path, len);
    const char *base = dbmem_path_basename_ptr(source_path, dbmem_path_trimmed_len(source_path));
    char hash_text[DBMEM_HASH_STR_MAXLEN];
    dbmem_hash_to_hex(hash, hash_text);

    size_t base_len = strlen(base);
    size_t total = 9 + 1 + base_len;
    char *copy = (char *)dbmemory_alloc((uint64_t)total + 1);
    if (!copy) return NULL;
    memcpy(copy, hash_text, 8);
    copy[8] = '/';
    memcpy(copy + 9, base, base_len);
    copy[total] = '\0';
    return copy;
}

static char *dbmem_path_unique_storage_copy (sqlite3 *db, const char *preferred_path, const char *source_path) {
    if (!preferred_path) return NULL;

    if (!source_path || !dbmem_database_path_conflicts(db, preferred_path, source_path)) {
        return dbmem_strdup(preferred_path);
    }

    int components = dbmem_path_component_count(source_path);
    for (int count = 2; count <= components; count++) {
        char *candidate = dbmem_path_disambiguated_copy(source_path, count);
        if (!candidate) return NULL;
        bool conflict = dbmem_database_path_conflicts(db, candidate, source_path);
        if (!conflict) return candidate;
        dbmemory_free(candidate);
    }

    for (int salt = 0; salt < 1000; salt++) {
        uint64_t hash = dbmem_hash_compute(source_path, strlen(source_path));
        char hash_text[DBMEM_HASH_STR_MAXLEN];
        dbmem_hash_to_hex(hash + (uint64_t)salt, hash_text);
        const char *base = dbmem_path_basename_ptr(source_path, dbmem_path_trimmed_len(source_path));
        size_t base_len = strlen(base);
        size_t total = 16 + 1 + base_len;
        char *candidate = (char *)dbmemory_alloc((uint64_t)total + 1);
        if (!candidate) return NULL;
        memcpy(candidate, hash_text, 16);
        candidate[16] = '/';
        memcpy(candidate + 17, base, base_len);
        candidate[total] = '\0';
        bool conflict = dbmem_database_path_conflicts(db, candidate, source_path);
        if (!conflict) return candidate;
        dbmemory_free(candidate);
    }

    return NULL;
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
    char *stored_path = dbmem_path_storage_copy(path, ctx->root_path);
    if (!stored_path) {
        dbmemory_free(buffer);
        return SQLITE_NOMEM;
    }

    ctx->path = stored_path;
    ctx->source_path = path;
    int rc = dbmem_process_buffer(ctx, buffer, len);
    ctx->path = NULL;
    ctx->source_path = NULL;
    dbmemory_free(stored_path);
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
    rc = sqlite3_exec(db,
        "CREATE TEMP TABLE dbmem_reindex AS "
        "SELECT c.path, s.source_path, c.value, c.context "
        "FROM dbmem_content c LEFT JOIN dbmem_content_source s ON s.path = c.path;",
        NULL, NULL, NULL);
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
    rc = sqlite3_exec(db, "DELETE FROM dbmem_content_source;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    rc = sqlite3_exec(db, "DELETE FROM dbmem_content;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    // reset dimension so the new model's dimension is auto-detected
    ctx->dimension = 0;
    ctx->dimension_saved = false;
    ctx->vector_extension_available = false;

    // iterate temp table one row at a time
    rc = sqlite3_prepare_v2(db, "SELECT path, source_path, value, context FROM dbmem_reindex;", -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(vm, 0);
        const char *source_path = (const char *)sqlite3_column_text(vm, 1);
        const char *value = (const char *)sqlite3_column_text(vm, 2);
        int value_len = sqlite3_column_bytes(vm, 2);
        const char *context = (const char *)sqlite3_column_text(vm, 3);

        dbmem_context_reset_temp_values(ctx);
        ctx->context = context;

        if (source_path && dbmem_file_exists(source_path)) {
            rc = dbmem_process_file(ctx, source_path);
        } else if (path && dbmem_file_exists(path)) {
            rc = dbmem_process_file(ctx, path);
        } else if (value && value_len > 0) {
            ctx->path = path;
            ctx->source_path = source_path;
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

static void dbmem_add_content (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_content expects the first parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }
    if (sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_content expects the second parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }
    if (argc == 3 && sqlite3_value_type(argv[2]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_content expects the third parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }

    // retrieve dbmem_context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    const char *content = (const char *)sqlite3_value_text(argv[1]);
    int len = sqlite3_value_bytes(argv[1]);

    // reset temp values
    dbmem_context_reset_temp_values(ctx);

    // check for optional memory context
    if (argc == 3) {
        ctx->context = (const char *)sqlite3_value_text(argv[2]);
    }

    char *stored_path = dbmem_path_storage_copy(path, NULL);
    if (!stored_path) {
        sqlite3_result_error_nomem(context);
        return;
    }

    ctx->path = stored_path;
    int rc = dbmem_process_buffer(ctx, content, len);
    ctx->path = NULL;
    dbmemory_free(stored_path);

    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

#ifndef DBMEM_OMIT_IO
static int dbmem_scan_callback (const char *path, void *data) {
    dbmem_context *ctx = (dbmem_context *)data;

    int rc = dbmem_process_file(ctx, path);
    if (rc == 0) ctx->counter++;

    return rc;
}

static void dbmem_add_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_file expects the first parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }
    if (argc == 2 && sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_add_file expects the second parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }

    // retrieve dbmem_context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *path = (const char *)sqlite3_value_text(argv[0]);

    // reset temp values
    dbmem_context_reset_temp_values(ctx);

    // check for optional memory context
    if (argc == 2) {
        ctx->context = (const char *)sqlite3_value_text(argv[1]);
    }

    int rc = dbmem_process_file(ctx, path);
    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

static int dbmem_make_directory (const char *path) {
    if (dbmem_dir_exists(path)) return SQLITE_OK;

    #ifdef _WIN32
    int rc = _mkdir(path);
    #else
    int rc = mkdir(path, 0755);
    #endif

    if (rc == 0 || (errno == EEXIST && dbmem_dir_exists(path))) return SQLITE_OK;
    return SQLITE_IOERR;
}

static size_t dbmem_path_root_len (const char *path) {
    if (!path || !path[0]) return 0;

    if (dbmem_path_separator(path[0]) && dbmem_path_separator(path[1])) {
        size_t i = 2;
        while (path[i] && !dbmem_path_separator(path[i])) i++;
        if (path[i]) i++;
        while (path[i] && !dbmem_path_separator(path[i])) i++;
        if (path[i]) i++;
        return i;
    }

    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        return dbmem_path_separator(path[2]) ? 3 : 2;
    }

    return dbmem_path_separator(path[0]) ? 1 : 0;
}

static int dbmem_ensure_directory (const char *path) {
    if (!path || !path[0]) return SQLITE_OK;

    char *copy = dbmem_strdup(path);
    if (!copy) return SQLITE_NOMEM;

    size_t len = strlen(copy);
    size_t root_len = dbmem_path_root_len(copy);
    while (len > root_len && dbmem_path_separator(copy[len - 1])) copy[--len] = '\0';
    if (len <= root_len) { dbmemory_free(copy); return SQLITE_OK; }

    int rc = SQLITE_OK;
    for (size_t i = root_len; i < len; i++) {
        if (!dbmem_path_separator(copy[i])) continue;
        if (i == root_len || dbmem_path_separator(copy[i - 1])) continue;

        char saved = copy[i];
        copy[i] = '\0';
        rc = dbmem_make_directory(copy);
        copy[i] = saved;
        if (rc != SQLITE_OK) goto cleanup;
    }

    rc = dbmem_make_directory(copy);

cleanup:
    dbmemory_free(copy);
    return rc;
}

static int dbmem_ensure_parent_directory (const char *path) {
    if (!path || !path[0]) return SQLITE_ERROR;

    size_t len = strlen(path);
    size_t root_len = dbmem_path_root_len(path);
    size_t parent_len = 0;
    for (size_t i = len; i > root_len; i--) {
        if (dbmem_path_separator(path[i - 1])) {
            parent_len = i - 1;
            break;
        }
    }
    if (parent_len <= root_len) return SQLITE_OK;

    char *parent = (char *)dbmemory_alloc((uint64_t)parent_len + 1);
    if (!parent) return SQLITE_NOMEM;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';

    int rc = dbmem_ensure_directory(parent);
    dbmemory_free(parent);
    return rc;
}

static int dbmem_write_file_bytes (const char *path, const char *content, int len) {
    int64_t existing_len = 0;
    char *existing = dbmem_file_read(path, &existing_len);
    if (existing) {
        bool same = existing_len == len && memcmp(existing, content, (size_t)len) == 0;
        dbmemory_free(existing);
        if (same) return SQLITE_OK;
    }

    int rc = dbmem_ensure_parent_directory(path);
    if (rc != SQLITE_OK) return rc;

    FILE *file = fopen(path, "wb");
    if (!file) return SQLITE_IOERR;

    size_t written = fwrite(content, 1, (size_t)len, file);
    if (written != (size_t)len) rc = SQLITE_IOERR_WRITE;
    if (fclose(file) != 0 && rc == SQLITE_OK) rc = SQLITE_IOERR_CLOSE;

    return rc;
}

static char *dbmem_path_join_root (const char *root, const char *path) {
    if (!root || !root[0] || dbmem_path_is_absolute(path)) return dbmem_strdup(path);

    size_t root_len = dbmem_path_trimmed_len(root);
    size_t path_len = strlen(path);
    while (path_len > 0 && dbmem_path_separator(*path)) {
        path++;
        path_len--;
    }

    size_t total = root_len + 1 + path_len;
    char *joined = (char *)dbmemory_alloc((uint64_t)total + 1);
    if (!joined) return NULL;

    memcpy(joined, root, root_len);
    #ifdef _WIN32
    joined[root_len] = '\\';
    #else
    joined[root_len] = '/';
    #endif
    memcpy(joined + root_len + 1, path, path_len);
    joined[total] = '\0';
    return joined;
}

static bool dbmem_path_has_parent_segment (const char *path) {
    if (!path) return false;

    size_t i = 0;
    while (path[i]) {
        while (dbmem_path_separator(path[i])) i++;
        size_t start = i;
        while (path[i] && !dbmem_path_separator(path[i])) i++;
        if ((i - start) == 2 && path[start] == '.' && path[start + 1] == '.') return true;
    }

    return false;
}

static char *dbmem_materialize_path_copy (const char *root, const char *path, int *rc) {
    *rc = SQLITE_OK;

    if (root && root[0]) {
        char *logical_path = dbmem_path_storage_copy(path, NULL);
        if (!logical_path) {
            *rc = SQLITE_NOMEM;
            return NULL;
        }
        if (dbmem_path_has_parent_segment(logical_path)) {
            dbmemory_free(logical_path);
            *rc = SQLITE_MISUSE;
            return NULL;
        }

        char *write_path = dbmem_path_join_root(root, logical_path);
        dbmemory_free(logical_path);
        if (!write_path) *rc = SQLITE_NOMEM;
        return write_path;
    }

    if (dbmem_path_has_parent_segment(path)) {
        *rc = SQLITE_MISUSE;
        return NULL;
    }

    return dbmem_path_join_root(root, path);
}

static void dbmem_materialize_files (sqlite3_context *context, int argc, sqlite3_value **argv) {
    if (argc == 1 && sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function memory_materialize_files expects an optional root path of type TEXT", SQLITE_ERROR);
        return;
    }

    sqlite3 *db = sqlite3_context_db_handle(context);
    sqlite3_stmt *vm = NULL;
    const char *root = (argc == 1) ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    static const char *sql =
        "SELECT path, value FROM dbmem_content WHERE path IS NOT NULL AND path != '' ORDER BY path;";

    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;

    sqlite3_int64 count = 0;
    while ((rc = sqlite3_step(vm)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(vm, 0);
        const char *content = (const char *)sqlite3_column_text(vm, 1);
        int len = sqlite3_column_bytes(vm, 1);

        if (!content) {
            sqlite3_result_error(context, "memory_materialize_files cannot materialize rows with NULL content", -1);
            sqlite3_finalize(vm);
            return;
        }

        int path_rc = SQLITE_OK;
        char *write_path = dbmem_materialize_path_copy(root, path, &path_rc);
        if (!write_path) {
            if (path_rc == SQLITE_NOMEM) {
                sqlite3_result_error_nomem(context);
            } else {
                sqlite3_result_error(context, "memory_materialize_files refuses paths containing '..' segments", -1);
            }
            sqlite3_finalize(vm);
            return;
        }

        rc = dbmem_write_file_bytes(write_path, content, len);
        dbmemory_free(write_path);
        if (rc != SQLITE_OK) {
            sqlite3_result_error(context, "memory_materialize_files failed to write a file", -1);
            sqlite3_finalize(vm);
            return;
        }
        count++;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;

cleanup:
    if (vm) sqlite3_finalize(vm);
    if (rc == SQLITE_OK) {
        sqlite3_result_int64(context, count);
    } else {
        sqlite3_result_error(context, sqlite3_errmsg(db), -1);
    }
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
    static const char *sql =
        "SELECT c.hash, c.path, s.source_path "
        "FROM dbmem_content c "
        "LEFT JOIN dbmem_content_source s ON s.path = c.path "
        "WHERE c.path IS NOT NULL AND c.path != '';";
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
        const char *source_path = (const char *)sqlite3_column_text(vm, 2);

        bool exists = false;
        if (source_path && source_path[0]) {
            if (!dbmem_path_is_under_directory(source_path, dir_path)) continue;
            exists = dbmem_file_exists(source_path);
        } else {
            char *disk_path = dbmem_path_join_root(dir_path, path);
            if (!disk_path) continue;
            exists = dbmem_file_exists(disk_path);
            dbmemory_free(disk_path);
            if (dbmem_path_is_absolute(path) && !dbmem_path_is_under_directory(path, dir_path)) continue;
        }

        if (exists) continue;
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
    ctx->root_path = path;

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

    ctx->reindex_mode = true;
    dbmem_context_reset_temp_values(ctx);

    int64_t processed = 0;
    int rc = SQLITE_OK;

    sqlite3_exec(db, "DROP TABLE IF EXISTS dbmem_reindex_pending;", NULL, NULL, NULL);
    rc = sqlite3_exec(db,
        "CREATE TEMP TABLE dbmem_reindex_pending AS "
        "SELECT hash, path, value, context FROM dbmem_content WHERE value IS NOT NULL;",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto done;

    while (1) {
        sqlite3_stmt *vm = NULL;
        rc = sqlite3_prepare_v2(db,
            "SELECT rowid, hash, path, value, context FROM dbmem_reindex_pending LIMIT 1;",
            -1, &vm, NULL);
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
        sqlite3_int64 pending_rowid = sqlite3_column_int64(vm, 0);
        const char *hash_raw = (const char *)sqlite3_column_text(vm, 1);
        const char *path_raw = (const char *)sqlite3_column_text(vm, 2);
        const char *value_raw = (const char *)sqlite3_column_text(vm, 3);
        int64_t value_len = (int64_t)sqlite3_column_bytes(vm, 3);
        const char *ctx_raw = (const char *)sqlite3_column_text(vm, 4);

        char *hash_text = dbmem_strdup(hash_raw);
        char *path = dbmem_strdup(path_raw);
        char *value = (char *)sqlite3_malloc64((sqlite3_uint64)(value_len + 1));
        if (value) { memcpy(value, value_raw, (size_t)value_len); value[value_len] = '\0'; }
        char *ctx_name = dbmem_strdup(ctx_raw);

        sqlite3_finalize(vm);

        if (!hash_text || !path || !value) {
            dbmemory_free(hash_text);
            dbmemory_free(path);
            if (value) sqlite3_free(value);
            dbmemory_free(ctx_name);
            rc = SQLITE_NOMEM;
            break;
        }

        uint64_t stored_hash = 0;
        if (!dbmem_hash_from_hex(hash_text, &stored_hash)) {
            dbmemory_free(hash_text);
            dbmemory_free(path);
            sqlite3_free(value);
            dbmemory_free(ctx_name);
            rc = SQLITE_MISMATCH;
            break;
        }

        uint64_t value_hash = dbmem_hash_compute(value, (size_t)value_len);
        bool hash_matches = (stored_hash == value_hash);
        bool value_has_vault = dbmem_database_hash_has_vault(db, value_hash);
        bool needs_reindex = !hash_matches || !value_has_vault;

        if (needs_reindex && !value_has_vault) {
            ctx->path = path;
            ctx->context = ctx_name;
            rc = dbmem_process_buffer(ctx, value, value_len);
        }

        if (rc == SQLITE_OK && needs_reindex) {
            rc = dbmem_database_update_content_hash(db, path, value_hash);
            if (rc == SQLITE_OK && !hash_matches) {
                rc = dbmem_database_delete_index_hash(db, stored_hash);
            }
        }

        if (rc == SQLITE_OK) {
            sqlite3_stmt *delete_vm = NULL;
            rc = sqlite3_prepare_v2(db, "DELETE FROM dbmem_reindex_pending WHERE rowid=?1;", -1, &delete_vm, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(delete_vm, 1, pending_rowid);
                rc = sqlite3_step(delete_vm);
                if (rc == SQLITE_DONE) rc = SQLITE_OK;
            }
            if (delete_vm) sqlite3_finalize(delete_vm);
        }

        ctx->path = NULL;
        ctx->context = NULL;
        dbmemory_free(hash_text);
        dbmemory_free(path);
        sqlite3_free(value);
        dbmemory_free(ctx_name);

        if (rc != SQLITE_OK) break;
        if (needs_reindex) processed++;
    }

done:
    ctx->reindex_mode = false;
    ctx->path = NULL;
    ctx->context = NULL;
    sqlite3_exec(db, "DROP TABLE IF EXISTS dbmem_reindex_pending;", NULL, NULL, NULL);

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

    rc = sqlite3_create_function_v2(db, "memory_is_enabled", 0, SQLITE_UTF8, ctx, dbmem_is_enabled, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_set_model", 2, SQLITE_UTF8, ctx, dbmem_set_model, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_set_apikey", 1, SQLITE_UTF8, ctx, dbmem_set_apikey, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_content", 2, SQLITE_UTF8, ctx, dbmem_add_content, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_add_content", 3, SQLITE_UTF8, ctx, dbmem_add_content, NULL, NULL, NULL);
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

    rc = sqlite3_create_function_v2(db, "memory_materialize_files", 0, SQLITE_UTF8, ctx, dbmem_materialize_files, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_materialize_files", 1, SQLITE_UTF8, ctx, dbmem_materialize_files, NULL, NULL, NULL);
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

    rc = sqlite3_create_function_v2(db, "memory_delete_file", 1, SQLITE_UTF8, ctx, dbmem_delete_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_clear", 0, SQLITE_UTF8, ctx, dbmem_clear, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_rename_file", 2, SQLITE_UTF8, ctx, dbmem_rename_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { dbmem_context_free(ctx); return rc; }

    rc = sqlite3_create_function_v2(db, "memory_list_files", 0, SQLITE_UTF8, ctx, dbmem_list_files, NULL, NULL, NULL);
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

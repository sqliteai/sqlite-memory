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
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "sqlite-memory.h"
#include "dbmem-utils.h"
#include "dbmem-lembed.h"
#include "dbmem-parser.h"

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

// available compilation options
// DBMEM_OMIT_IO                                // to be used when compiled for WASM
// DBMEM_OMIT_LOCAL_ENGINE                      // to be used when compiled for WASM or when the local inference engine is not needed

#define DBMEM_TYPE_VALUE                        500
#define DBMEM_LOCAL_PROVIDER                    "local"

#define DBMEM_SETTINGS_KEY_PROVIDER             "provider"
#define DBMEM_SETTINGS_KEY_MODEL                "model"
#define DBMEM_SETTINGS_KEY_MAX_TOKENS           "max_tokens"
#define DBMEM_SETTINGS_KEY_OVERLAY_TOKENS       "overlay_tokens"
#define DBMEM_SETTINGS_KEY_CHARS_PER_TOKENS     "chars_per_tokens"
#define DBMEM_SETTINGS_KEY_SAVE_CONTEXT         "save_content"
#define DBMEM_SETTINGS_KEY_SKIP_SEMANTIC        "skip_semantic"
#define DBMEM_SETTINGS_KEY_SKIP_HTML            "skip_html"

// default values from https://docs.openclaw.ai/concepts/memory
#define DEFAULT_CHARS_PER_TOKEN                 4       // Approximate number of characters per token (GPT ≈ 4, Claude ≈ 3.5)
#define DEFAULT_MAX_TOKENS                      400
#define DEFAULT_OVERLAY_TOKENS                  80
#define DEFAULT_MAX_SNIPPET_CHARS               700

typedef struct {
    sqlite3     *db;
    char        *provider;
    char        *model;
    char        *api_key;
    size_t      max_tokens;
    size_t      overlay_tokens;
    size_t      chars_per_tokens;
    size_t      snippet_max_chars;
    bool        save_content;
    bool        skip_semantic;
    bool        skip_html;
    char        error_msg[DBMEM_MAXERROR_SIZE];
} dbmem_context;

// MARK: - Settings -

int dbmem_database_init (sqlite3 *db) {
    const char *sql = "CREATE TABLE IF NOT EXISTS dbmem_settings (key TEXT PRIMARY KEY, value TEXT);";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_content (hash BLOB PRIMARY KEY NOT NULL, path TEXT NOT NULL UNIQUE, value TEXT DEFAULT NULL, context TEXT DEFAULT NULL);";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    sql = "CREATE TABLE IF NOT EXISTS dbmem_vault (hash BLOB NOT NULL, seq INTEGER NOT NULL, embedding BLOB NOT NULL, offset INTEGER NOT NULL, length INTEGER NOT NULL, PRIMARY KEY (hash, seq));";
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    return rc;
}

int dbmem_settings_write (sqlite3 *db, const char *key, const char *text_value, sqlite3_int64 int_value, const sqlite3_value *sql_value, int bind_type) {
    static const char *sql = "REPLACE INTO dbmem_settings (key, value) VALUES (?1, ?2);";
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 1, key, -1, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    switch (bind_type) {
        case SQLITE_TEXT: rc = sqlite3_bind_text(vm, 2, text_value, -1, NULL); break;
        case SQLITE_INTEGER: rc = sqlite3_bind_int64(vm, 2, int_value); break;
        case DBMEM_TYPE_VALUE: rc = sqlite3_bind_value(vm, 2, sql_value);
        default: rc = SQLITE_MISUSE; goto cleanup;
    }
    
    rc = sqlite3_step(vm);
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    
cleanup:
    if (vm) sqlite3_finalize(vm);
    return rc;
}

int dbmem_settings_write_text (sqlite3 *db, const char *key, const char *value) {
    return dbmem_settings_write(db, key, value, 0, NULL, SQLITE_TEXT);
}

int dbmem_settings_write_int (sqlite3 *db, const char *key, sqlite3_int64 value) {
    return dbmem_settings_write(db, key, NULL, value, NULL, SQLITE_INTEGER);
}

int dbmem_settings_write_value (sqlite3 *db, const char *key, sqlite3_value *value) {
    return dbmem_settings_write(db, key, NULL, 0, value, DBMEM_TYPE_VALUE);
}

int dbmem_settings_sync (dbmem_context *ctx, const char *key, sqlite3_value *value) {
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
    
    return 0;
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
    
    return (void *)ctx;
}

static void dbmem_context_free (void *ptr) {
    if (ptr) sqlite3_free(ptr);
}

// MARK: - General -

static void dbmem_version (sqlite3_context *context, int argc, sqlite3_value **argv) {
    sqlite3_result_text(context, SQLITE_DBMEMORY_VERSION, -1, NULL);
}

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
    
    // if provider is local then make sure model file exists
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (strcasecmp(provider, DBMEM_LOCAL_PROVIDER) == 0) {
        if (dbmem_file_exists(model) == false) {
            sqlite3_result_error(context, "Local model not found in the specified path", SQLITE_ERROR);
            return;
        }
    }
    #endif
    
    // update settings
    sqlite3 *db = sqlite3_context_db_handle(context);
    int rc = dbmem_settings_write_text (db, DBMEM_SETTINGS_KEY_PROVIDER, provider);
    if (rc == SQLITE_OK) rc = dbmem_settings_write_text (db, DBMEM_SETTINGS_KEY_MODEL, model);
    
    (rc == SQLITE_OK) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, sqlite3_errmsg(db), -1);
}

static void dbmem_set_apikey (sqlite3_context *context, int argc, sqlite3_value **argv) {

}

// MARK: -

static void dbmem_set_option (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function dbmem_set_option expects the key argument to be of type TEXT", SQLITE_ERROR);
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
        sqlite3_result_error(context, "The function dbmem_get_option expects the key argument to be of type TEXT", SQLITE_ERROR);
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

static int dbmem_process_buffer (dbmem_context *ctx, const char *buffer, int64_t len) {
    dbmem_parse_settings settings = {0};
    settings.chars_per_token = ctx->chars_per_tokens;
    settings.max_tokens = ctx->max_tokens;
    settings.overlay_tokens = ctx->overlay_tokens;
    settings.skip_semantic = ctx->skip_semantic;
    settings.skip_html = ctx->skip_html;
    
    int rc = dbmem_parse(buffer, (size_t)len, &settings);
    
    return 0;
}

static int dbmem_process_file (dbmem_context *ctx, const char *path) {
    if (!dbmem_file_exists(path)) {
        snprintf(ctx->error_msg, DBMEM_MAXERROR_SIZE, "Unable to find file at path %s", path);
        return -1;
    }
    
    int64_t len = 0;
    char *buffer = dbmem_file_read(path, &len);
    if (!buffer) {
        snprintf(ctx->error_msg, DBMEM_MAXERROR_SIZE, "Unable to read file at path %s", path);
        return -1;
    }
    
    // do real processing
    return dbmem_process_buffer(ctx, buffer, len);
}

static void dbmem_add_text (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function dbmem_add_text expects a parameter of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *content = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    
    int rc = dbmem_process_buffer(ctx, content, len);
    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

#ifndef DBMEM_OMIT_IO
static void dbmem_add_file (sqlite3_context *context, int argc, sqlite3_value **argv) {
    // sanity check type
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "The function dbmem_add_file expects the first parameter to be of type TEXT", SQLITE_ERROR);
        return;
    }
    
    // retrieve context
    dbmem_context *ctx = (dbmem_context *)sqlite3_user_data(context);
    const char *path = (const char *)sqlite3_value_text(argv[0]);
    int rc = dbmem_process_file(ctx, path);
    
    (rc == 0) ? sqlite3_result_int(context, 1) : sqlite3_result_error(context, ctx->error_msg, -1);
}

static void dbmem_add_directory (sqlite3_context *context, int argc, sqlite3_value **argv) {
    
}
#endif
    
// MARK: -

SQLITE_DBMEMORY_API int sqlite3_memory_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    #ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
    #endif
    int rc = SQLITE_OK;
    
    // there's no built-in way to verify if sqlite3_memory_init has already been called for this specific database connection
    // the workaround is to attempt to execute vector_version and check for an error
    // an error indicates that initialization has not been performed
    // if (sqlite3_exec(db, "SELECT memory_version();", NULL, NULL, NULL) == SQLITE_OK) return SQLITE_OK;
    
    rc = dbmem_database_init(db);
    if (rc != SQLITE_OK) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("An error occurred while creating internal tables (%s)", sqlite3_errmsg(db));
        return rc;
    }
    
    void *ctx = dbmem_context_create(db);
    if (!ctx) {
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("Not enought memory to create a database context");
        return SQLITE_NOMEM;
    }
    
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
    rc = sqlite3_create_function_v2(db, "memory_add_file", 1, SQLITE_UTF8, ctx, dbmem_add_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_add_file", 2, SQLITE_UTF8, ctx, dbmem_add_file, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_add_directory", 1, SQLITE_UTF8, ctx, dbmem_add_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_add_directory", 2, SQLITE_UTF8, ctx, dbmem_add_directory, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    #endif
    
    rc = sqlite3_create_function_v2(db, "memory_add_text", 1, SQLITE_UTF8, ctx, dbmem_add_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    
    rc = sqlite3_create_function_v2(db, "memory_add_text", 2, SQLITE_UTF8, ctx, dbmem_add_text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;

    return SQLITE_OK;
}

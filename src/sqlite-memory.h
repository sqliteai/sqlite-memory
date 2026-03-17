//
//  sqlite-memory.h
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#ifndef __SQLITE_DBMEMORY__
#define __SQLITE_DBMEMORY__

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#include <stdbool.h>

#ifdef _WIN32
  #define SQLITE_DBMEMORY_API __declspec(dllexport)
#else
  #define SQLITE_DBMEMORY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_DBMEMORY_VERSION "0.8.1"

// public API
SQLITE_DBMEMORY_API int sqlite3_memory_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);

// Custom embedding provider API
// Allows registering a user-defined embedding engine that works regardless of
// DBMEM_OMIT_LOCAL_ENGINE / DBMEM_OMIT_REMOTE_ENGINE compile flags.
// The api_key set via memory_set_apikey() is passed to the init callback.
typedef struct dbmem_context dbmem_context;

typedef struct {
    int      n_tokens;
    int      n_tokens_truncated;
    int      n_embd;
    float    *embedding;          // Engine-owned buffer, valid until next call or free
} dbmem_embedding_result_t;

typedef struct {
    // Called when memory_set_model(provider, model) matches this provider.
    // api_key is the value set via memory_set_apikey() (may be NULL).
    // xdata is the user-supplied generic pointer from the struct.
    // Return opaque engine pointer, or NULL on error (fill err_msg).
    void *(*init)(const char *model, const char *api_key, void *xdata, char err_msg[1024]);

    // Compute embedding for text. Return 0 on success, non-zero on error.
    int   (*compute)(void *engine, const char *text, int text_len, void *xdata, dbmem_embedding_result_t *result);

    // Free the engine. Called on context teardown or model change. May be NULL.
    void  (*free)(void *engine, void *xdata);

    // User-supplied generic data pointer, passed to all callbacks.
    void  *xdata;
} dbmem_provider_t;

// Register a custom embedding provider.
// provider_name: matched against the first argument of memory_set_model().
// Returns SQLITE_OK on success.
SQLITE_DBMEMORY_API int sqlite3_memory_register_provider (sqlite3 *db, const char *provider_name, const dbmem_provider_t *provider);

void  *dbmem_context_engine (dbmem_context *ctx, bool *is_local);
bool   dbmem_context_is_custom (dbmem_context *ctx);
bool   dbmem_context_load_vector (dbmem_context *ctx);
bool   dbmem_context_load_sync (dbmem_context *ctx);
bool   dbmem_context_perform_fts (dbmem_context *ctx);
int    dbmem_context_max_results (dbmem_context *ctx);
double dbmem_context_vector_weight (dbmem_context *ctx);
double dbmem_context_text_weight (dbmem_context *ctx);
double dbmem_context_min_score (dbmem_context *ctx);
bool   dbmem_context_update_access (dbmem_context *ctx);
int    dbmem_context_search_oversample (dbmem_context *ctx);
const char *dbmem_context_errmsg (dbmem_context *ctx);
const char *dbmem_context_apikey (dbmem_context *ctx);
void   dbmem_context_set_error (dbmem_context *ctx, const char *str);

#ifdef __cplusplus
}
#endif


#endif

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

#define SQLITE_DBMEMORY_VERSION "0.5.5"

// public API
SQLITE_DBMEMORY_API int sqlite3_memory_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);

// internal APIs
typedef struct dbmem_context dbmem_context;

void  *dbmem_context_engine (dbmem_context *ctx, bool *is_local);
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

#ifdef __cplusplus
}
#endif


#endif

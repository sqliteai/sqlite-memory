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

#ifdef _WIN32
  #define SQLITE_DBMEMORY_API __declspec(dllexport)
#else
  #define SQLITE_DBMEMORY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_DBMEMORY_VERSION "0.0.4"

SQLITE_DBMEMORY_API int sqlite3_memory_init (sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi);

#ifdef __cplusplus
}
#endif


#endif

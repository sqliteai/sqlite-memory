//
//  dbmem-search.h
//  sqlitememory
//
//  Created by Marco Bambini on 07/02/26.
//

#ifndef __DBMEM_SEARCH__
#define __DBMEM_SEARCH__

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

int dbmem_register_search (sqlite3 *db, void *ctx, char **pzErrMsg);

#endif

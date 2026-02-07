//
//  dbmem-utils.h
//  sqlitememory
//
//  Created by Marco Bambini on 30/01/26.
//

#ifndef __DBMEM_UTILS__
#define __DBMEM_UTILS__

#include <stdint.h>
#include <stdbool.h>

#define UNUSED_PARAM(p)                     (void)(p)
#define ENABLE_DBMEM_DEBUG                  0

#define DEBUG_DBMEM_ALWAYS(...)             do {printf(__VA_ARGS__ );printf("\n");} while (0)

#if ENABLE_DBMEM_DEBUG
#define DEBUG_DBMEM(...)                    do {printf(__VA_ARGS__ );printf("\n");} while (0)
#define DEBUG_EMBEDDING(_res)               do {dbmem_dump_embeding(_res);} while (0)
#else
#define DEBUG_DBMEM(...)
#define DEBUG_EMBEDDING(_res)
#endif

#define DBMEM_ERRBUF_SIZE                   1024
#define DBMEM_UUID_STR_MAXLEN               37

// MEMORY
void     *dbmem_alloc (uint64_t size);
void     *dbmem_zeroalloc (uint64_t size);
void     *dbmem_realloc (void *ptr, uint64_t new_size);
void      dbmem_free (void *ptr);
uint64_t  dbmem_size (void *ptr);
char     *dbmem_strdup (const char *s);

// IO
typedef  int (*dir_scan_callback) (const char *path, void *data);
int    dbmem_dir_scan (const char *dir_path, dir_scan_callback callback, void *data);
bool   dbmem_dir_exists (const char *dir_path);
char  *dbmem_file_read (const char *path, int64_t *len);
bool   dbmem_file_exists (const char *path);
bool   dbmem_file_has_extension (const char *path, const char *extensions);

// GENERAL
uint64_t  dbmem_hash_compute (const void *data, size_t len);
char     *dbmem_uuid_v7 (char value[DBMEM_UUID_STR_MAXLEN]);

#endif

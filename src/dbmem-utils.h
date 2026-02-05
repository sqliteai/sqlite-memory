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

#define DEBUG_DBMEM_ALWAYS(...)             do {printf(__VA_ARGS__ );printf("\n");} while (0)

#if ENABLE_DBMEM_DEBUG
#define DEBUG_DBMEM(...)                    do {printf(__VA_ARGS__ );printf("\n");} while (0)
#else
#define DEBUG_DBMEM(...)
#endif

#define DBMEM_MAXERROR_SIZE                 1024

// MEMORY
void    *dbmem_alloc (uint64_t size);
void    *dbmem_zeroalloc (uint64_t size);
void    *dbmem_realloc (void *ptr, uint64_t new_size);
void     dbmem_free (void *ptr);
uint64_t dbmem_size (void *ptr);
char    *dbmem_strdup (const char *s);

// IO
char    *dbmem_file_read (const char *path, int64_t *len);
bool     dbmem_file_exists (const char *path);

// GENERAL
uint64_t dbmem_hash_compute (const void *data, size_t len);

#endif

//
//  dbmem-lembed.h
//  sqlitememory
//
//  Created by Marco Bambini on 04/02/26.
//

#ifndef __DBMEM_LOCAL_EMBED__
#define __DBMEM_LOCAL_EMBED__

#include "dbmem-utils.h"

typedef struct dbmem_local_engine_t dbmem_local_engine_t;

// Result structure for a single chunk
typedef struct {
    int      index;
    int      token_start;
    int      token_length;
    int      char_start;
    int      char_length;
    float   *embedding;
} chunk_result_t;

// Overall result structure
typedef struct {
    int              total_tokens;
    int              total_chars;
    int              n_embd;
    int              chunk_size;
    int              overlap;
    int              n_chunks;
    chunk_result_t  *chunks;
    bool             used_pool;  // If true, don't free individual embeddings
    char             err_msg[DBMEM_MAXERROR_SIZE];
} embedding_result_t;

dbmem_local_engine_t *dbmem_local_engine_init (const char *model_path, char err_msg[DBMEM_MAXERROR_SIZE]);
int  dbmem_local_compute_embedding (dbmem_local_engine_t *engine, const char *text, int text_len, embedding_result_t *result);
bool dbmem_local_engine_warmup (dbmem_local_engine_t *engine);
void dbmem_local_engine_free (dbmem_local_engine_t *engine);

#endif

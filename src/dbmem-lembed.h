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

// Embedding result structure (always one embedding per call)
typedef struct {
    int      n_tokens;              // Number of tokens processed
    int      n_tokens_truncated;    // Number of tokens truncated (0 if none)
    int      n_embd;                // Embedding dimension
    float    *embedding;            // Pointer to embedding (points to engine's buffer, do not free)
} embedding_result_t;

dbmem_local_engine_t *dbmem_local_engine_init (const char *model_path, char err_msg[DBMEM_ERRBUF_SIZE]);
int  dbmem_local_compute_embedding (dbmem_local_engine_t *engine, const char *text, int text_len, embedding_result_t *result);
bool dbmem_local_engine_warmup (dbmem_local_engine_t *engine);
void dbmem_local_engine_free (dbmem_local_engine_t *engine);
const char *dbmem_local_errmsg (dbmem_local_engine_t *engine);

#endif

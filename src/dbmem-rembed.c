//
//  dbmem-rembed.c
//  sqlitememory
//
//  Created by Marco Bambini on 09/02/26.
//

#include "dbmem-embed.h"

dbmem_remote_engine_t *dbmem_remote_engine_init (const char *provider, const char *model, char err_msg[DBMEM_ERRBUF_SIZE]) {
    UNUSED_PARAM(provider);
    UNUSED_PARAM(model);
    
    snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Vectors.space service not yet implemented");
    return NULL;
}

int dbmem_remote_compute_embedding (dbmem_remote_engine_t *engine, const char *text, int text_len, embedding_result_t *result) {
    UNUSED_PARAM(engine);
    UNUSED_PARAM(text);
    UNUSED_PARAM(text_len);
    UNUSED_PARAM(result);
    return -1;
}

void dbmem_remote_engine_free (dbmem_remote_engine_t *engine) {
    UNUSED_PARAM(engine);
}

const char *dbmem_remote_errmsg (dbmem_remote_engine_t *engine) {
    UNUSED_PARAM(engine);
    return "Vectors.space service not yet implemented";
}

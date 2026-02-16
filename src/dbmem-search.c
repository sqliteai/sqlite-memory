//
//  dbmem-search.c
//  sqlitememory_test
//
//  Created by Marco Bambini on 07/02/26.
//

#include "dbmem-utils.h"
#include "dbmem-search.h"
#include "dbmem-embed.h"
#include "sqlite-memory.h"
#ifndef SQLITE_CORE
#include "sqlite3ext.h"
#else
#include "sqlite3.h"
#endif

#include <string.h>
#include <stdio.h>
#include <float.h>
#include <time.h>

#ifndef SQLITE_CORE
SQLITE_EXTENSION_INIT3
#endif

#define SEARCH_COLUMN_QUERY                     0
#define SEARCH_COLUMN_MAXITEMS                  1
#define SEARCH_COLUMN_CONTEXT                   2
#define SEARCH_COLUMN_HASH                      3
#define SEARCH_COLUMN_SEQ                       4
#define SEARCH_COLUMN_RANKING                   5
#define SEARCH_COLUMN_PATH                      6
#define SEARCH_COLUMN_SNIPPET                   7

typedef struct {
    sqlite3_vtab    base;                   // Base class - must be first
    sqlite3         *db;
    dbmem_context   *ctx;
} vMemorySearchTable;

typedef struct {
    sqlite3_vtab_cursor base;               // Base class - must be first
    int             max_results;
    bool            perform_fts;

    int             index;
    int             count;
    char            *buffer;

    struct {
        int             count;
        double          *rank;
        sqlite3_int64   *hash;
        sqlite3_int64   *seq;
    } fts;

    struct {
        int             count;
        double          *rank;
        sqlite3_int64   *hash;
        sqlite3_int64   *seq;
    } semantic;

    struct {
        int             count;
        double          *vectorScore;
        double          *textScore;
        sqlite3_int64   *hash;
        sqlite3_int64   *seq;
        int             *hasVector;
        int             *hasFts;
    } merge;

    double              *rank;
    sqlite3_int64       *hash;
    sqlite3_int64       *seq;

} vMemorySearchCursor;

// MARK: - UTILS -

int vMemorySearchCursorAllocate (vMemorySearchCursor *c, int entries, bool perform_fts) {
    // one buffer to rule them all
    // fts (if enabled): rank, hash, seq = 3 arrays * entries
    // semantic: rank, hash, seq = 3 arrays * entries
    // merge: vectorScore, textScore, hash, seq, hasVector, hasFts = 6 arrays * 2*entries (can have both sources)
    // final: rank, hash, seq = 3 arrays * entries

    int merge_entries = entries * 2;
    size_t size = 0;

    // fts arrays
    if (perform_fts) {
        size += sizeof(double) * entries;           // fts.rank
        size += sizeof(sqlite3_int64) * entries;    // fts.hash
        size += sizeof(sqlite3_int64) * entries;    // fts.seq
    }

    // semantic arrays
    size += sizeof(double) * entries;               // semantic.rank
    size += sizeof(sqlite3_int64) * entries;        // semantic.hash
    size += sizeof(sqlite3_int64) * entries;        // semantic.seq

    // merge arrays (2x entries for union of both sources)
    size += sizeof(double) * merge_entries;         // merge.vectorScore
    size += sizeof(double) * merge_entries;         // merge.textScore
    size += sizeof(sqlite3_int64) * merge_entries;  // merge.hash
    size += sizeof(sqlite3_int64) * merge_entries;  // merge.seq
    size += sizeof(int) * merge_entries;            // merge.hasVector
    size += sizeof(int) * merge_entries;            // merge.hasFts

    // final arrays
    size += sizeof(double) * entries;               // rank
    size += sizeof(sqlite3_int64) * entries;        // hash
    size += sizeof(sqlite3_int64) * entries;        // seq

    char *buffer = (char *)dbmem_zeroalloc(size);
    if (!buffer) return SQLITE_NOMEM;

    // adjust all internal pointers
    c->max_results = entries;
    c->perform_fts = perform_fts;
    c->buffer = buffer;

    if (perform_fts) {
        c->fts.rank = (double *)buffer;
        buffer += sizeof(double) * entries;
        c->fts.hash = (sqlite3_int64 *)buffer;
        buffer += sizeof(sqlite3_int64) * entries;
        c->fts.seq = (sqlite3_int64 *)buffer;
        buffer += sizeof(sqlite3_int64) * entries;
    }

    // semantic
    c->semantic.rank = (double *)buffer;
    buffer += sizeof(double) * entries;
    c->semantic.hash = (sqlite3_int64 *)buffer;
    buffer += sizeof(sqlite3_int64) * entries;
    c->semantic.seq = (sqlite3_int64 *)buffer;
    buffer += sizeof(sqlite3_int64) * entries;

    // merge
    c->merge.vectorScore = (double *)buffer;
    buffer += sizeof(double) * merge_entries;
    c->merge.textScore = (double *)buffer;
    buffer += sizeof(double) * merge_entries;
    c->merge.hash = (sqlite3_int64 *)buffer;
    buffer += sizeof(sqlite3_int64) * merge_entries;
    c->merge.seq = (sqlite3_int64 *)buffer;
    buffer += sizeof(sqlite3_int64) * merge_entries;
    c->merge.hasVector = (int *)buffer;
    buffer += sizeof(int) * merge_entries;
    c->merge.hasFts = (int *)buffer;
    buffer += sizeof(int) * merge_entries;

    // final rowset
    c->rank = (double *)buffer;
    buffer += sizeof(double) * entries;
    c->hash = (sqlite3_int64 *)buffer;
    buffer += sizeof(sqlite3_int64) * entries;
    c->seq = (sqlite3_int64 *)buffer;

    return SQLITE_OK;
}

int vMemorySearchCursorMerge(vMemorySearchCursor *c, double vectorWeight, double textWeight, double min_score, int max_results) {
    c->merge.count = 0;

    // add semantic (vector) results
    // convert distance to score: vectorScore = 1 / (1 + distance)
    for (int i = 0; i < c->semantic.count; i++) {
        int idx = c->merge.count;
        c->merge.hash[idx] = c->semantic.hash[i];
        c->merge.seq[idx] = c->semantic.seq[i];
        c->merge.vectorScore[idx] = 1.0 / (1.0 + c->semantic.rank[i]);
        c->merge.textScore[idx] = 0.0;
        c->merge.hasVector[idx] = 1;
        c->merge.hasFts[idx] = 0;
        c->merge.count++;
    }

    // add/merge FTS results (already normalized to 0..1)
    for (int i = 0; i < c->fts.count; i++) {
        sqlite3_int64 hash = c->fts.hash[i];
        sqlite3_int64 seq = c->fts.seq[i];

        // check if already in merge list
        int found = -1;
        for (int j = 0; j < c->merge.count; j++) {
            if (c->merge.hash[j] == hash && c->merge.seq[j] == seq) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            c->merge.textScore[found] = c->fts.rank[i];
            c->merge.hasFts[found] = 1;
        } else {
            int idx = c->merge.count;
            c->merge.hash[idx] = hash;
            c->merge.seq[idx] = seq;
            c->merge.vectorScore[idx] = 0.0;
            c->merge.textScore[idx] = c->fts.rank[i];
            c->merge.hasVector[idx] = 0;
            c->merge.hasFts[idx] = 1;
            c->merge.count++;
        }
    }

    // compute final scores and store in vectorScore temporarily (reuse as finalScore)
    for (int i = 0; i < c->merge.count; i++) {
        double vw = vectorWeight;
        double tw = textWeight;

        // if only one source available, use it exclusively
        if (!c->merge.hasVector[i] && c->merge.hasFts[i]) {
            vw = 0.0; tw = 1.0;
        } else if (c->merge.hasVector[i] && !c->merge.hasFts[i]) {
            vw = 1.0; tw = 0.0;
        }

        // store finalScore in textScore (reusing the array)
        c->merge.textScore[i] = vw * c->merge.vectorScore[i] + tw * c->merge.textScore[i];
    }

    // sort by finalScore descending (simple insertion sort)
    // swap all parallel arrays together
    for (int i = 1; i < c->merge.count; i++) {
        double tempScore = c->merge.textScore[i];
        sqlite3_int64 tempHash = c->merge.hash[i];
        sqlite3_int64 tempSeq = c->merge.seq[i];

        int j = i - 1;
        while (j >= 0 && c->merge.textScore[j] < tempScore) {
            c->merge.textScore[j + 1] = c->merge.textScore[j];
            c->merge.hash[j + 1] = c->merge.hash[j];
            c->merge.seq[j + 1] = c->merge.seq[j];
            j--;
        }
        c->merge.textScore[j + 1] = tempScore;
        c->merge.hash[j + 1] = tempHash;
        c->merge.seq[j + 1] = tempSeq;
    }

    // copy top results to final cursor arrays, filtering by min_score
    c->count = 0;
    for (int i = 0; i < c->merge.count && c->count < max_results; i++) {
        if (c->merge.textScore[i] < min_score) break;  // sorted descending, so stop at first below threshold
        c->hash[c->count] = c->merge.hash[i];
        c->seq[c->count] = c->merge.seq[i];
        c->rank[c->count] = c->merge.textScore[i];
        c->count++;
    }

    return c->count;
}

static void vMemorySearchUpdateAccess(sqlite3 *db, vMemorySearchCursor *c) {
    static const char *sql = "UPDATE dbmem_content SET last_accessed = ?1 WHERE hash = ?2;";

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_int64 now = (sqlite3_int64)time(NULL);

    for (int i = 0; i < c->count; i++) {
        sqlite3_bind_int64(vm, 1, now);
        sqlite3_bind_int64(vm, 2, c->hash[i]);
        sqlite3_step(vm);
        sqlite3_reset(vm);
    }

    sqlite3_finalize(vm);
}

// MARK: - SEARCH -

static char *dbmem_fts_query_normalize (const char *query) {
    char *result = (char *)dbmem_zeroalloc(strlen(query) + 1);
    if (!result) return NULL;

    // Only the following characters can be part of the FTS5 query
    // Letters (a-z, A-Z)
    // Digits (0-9)
    // Underscore (_)
    
    size_t j = 0;
    for (size_t i = 0; query[i] != '\0'; i++) {
        char c = query[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == ' ') {
            result[j++] = c;
        }
    }

    return result;
}

static int dbmem_fts_search (sqlite3 *db, vMemorySearchCursor *c, const char *input_query, const char *context, int max_entries) {
    // Using dynamic IN clause would be faster for large context lists
    static const char *sql_no_context =
        "SELECT rank, hash, seq FROM dbmem_vault_fts WHERE content MATCH ?1 ORDER BY rank LIMIT ?2";
    static const char *sql_with_context =
        "SELECT fts.rank, fts.hash, fts.seq FROM dbmem_vault_fts AS fts "
        "JOIN dbmem_vault AS v ON fts.hash = v.hash AND fts.seq = v.seq "
        "WHERE fts.content MATCH ?1 AND INSTR(',' || ?3 || ',', ',' || v.context || ',') > 0 "
        "ORDER BY fts.rank LIMIT ?2";
    const char *sql = (context) ? sql_with_context : sql_no_context;
    
    char *query = dbmem_fts_query_normalize (input_query);
    if (!query) return SQLITE_NOMEM; // if something goes wrong then silently skip FTS5
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_text(vm, 1, query, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 2, max_entries);
    if (rc != SQLITE_OK) goto cleanup;
    
    if (context) {
        rc = sqlite3_bind_text(vm, 3, context, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto cleanup;
    }
    
    // used in ranking normalization
    double rank_min = DBL_MAX;
    double rank_max = -DBL_MAX;
    
    int count = 0;
    while (1) {
        rc = sqlite3_step(vm);
        if (rc == SQLITE_DONE) {rc = SQLITE_OK; break;}
        else if (rc != SQLITE_ROW) break;
        
        // SQLITE_ROW
        double rank = sqlite3_column_double(vm, 0);
        if (rank < rank_min) rank_min = rank;
        if (rank > rank_max) rank_max = rank;
        
        c->fts.rank[count] = rank;
        c->fts.hash[count] = sqlite3_column_int64(vm, 1);
        c->fts.seq[count] = sqlite3_column_int64(vm, 2);
        c->fts.count++;
        
        ++count;
    }
    
    // normalize ranking
    if (count > 0) {
        double denom = rank_max - rank_min;
        for (int i=0; i<count; ++i) {
            double rank = c->fts.rank[i];
            c->fts.rank[i] = (denom > 0.0) ? ((rank_max - rank) / denom) : 1.0f;
        }
    }
    
cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM("Error in dbmem_fts_search: %s", sqlite3_errmsg(db));
    if (query) dbmem_free(query);
    if (vm) sqlite3_finalize(vm);
    return rc;
}

// SELECT * FROM memory_search('Query', [max_entries], ['context'])
// MAX_ENTRIES (numeric, default 25)
// context (text, comma separated memory context)

static int dbmem_semantic_search (sqlite3 *db, vMemorySearchCursor *c, float *embedding, int embedding_size, const char *context, int max_entries) {
    // Using dynamic IN clause, would be faster for large context lists
    static const char *sql_no_context =
        "SELECT v.distance, e.hash, e.seq FROM dbmem_vault AS e "
        "JOIN vector_full_scan('dbmem_vault', 'embedding', ?1, ?2) AS v ON e.rowid = v.rowid";
    static const char *sql_with_context =
        "SELECT v.distance, e.hash, e.seq FROM dbmem_vault AS e "
        "JOIN vector_full_scan('dbmem_vault', 'embedding', ?1, ?2) AS v ON e.rowid = v.rowid "
        "WHERE INSTR(',' || ?3 || ',', ',' || e.context || ',') > 0";
    const char *sql = (context) ? sql_with_context : sql_no_context;
    
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_blob(vm, 1, embedding, embedding_size, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto cleanup;
    
    rc = sqlite3_bind_int(vm, 2, max_entries);
    if (rc != SQLITE_OK) goto cleanup;
    
    if (context) {
        rc = sqlite3_bind_text(vm, 3, context, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto cleanup;
    }
    
    int count = 0;
    while (1) {
        rc = sqlite3_step(vm);
        if (rc == SQLITE_DONE) {rc = SQLITE_OK; break;}
        else if (rc != SQLITE_ROW) break;
        
        // SQLITE_ROW
        c->semantic.rank[count] = sqlite3_column_double(vm, 0);
        c->semantic.hash[count] = sqlite3_column_int64(vm, 1);
        c->semantic.seq[count] = sqlite3_column_int64(vm, 2);
        c->semantic.count++;
        
        ++count;
    }
    
cleanup:
    if (rc != SQLITE_OK) DEBUG_DBMEM("Error in dbmem_semantic_search: %s", sqlite3_errmsg(db));
    if (vm) sqlite3_finalize(vm);
    return rc;
}

// MARK: - MODULE -

static int vMemorySearchConnect (sqlite3 *db, void *pAux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr) {
    dbmem_context *ctx = (dbmem_context *)pAux;
    
    // check for vector extension loaded and inited here
    if (dbmem_context_load_vector(ctx) == false) {
        if (pzErr) *pzErr = sqlite3_mprintf(dbmem_context_errmsg(ctx));
        return SQLITE_NOTFOUND;
    }
    
    // https://www.sqlite.org/vtab.html#table_valued_functions
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(query hidden, max_entries hidden, context hidden, hash, seq, ranking, path, snippet);");
    if (rc != SQLITE_OK) return rc;
    
    vMemorySearchTable *vtab = (vMemorySearchTable *)dbmem_zeroalloc(sizeof(vMemorySearchTable));
    if (!vtab) return SQLITE_NOMEM;
    
    vtab->db = db;
    vtab->ctx = ctx;
    
    *ppVtab = (sqlite3_vtab *)vtab;
    return SQLITE_OK;
}

static int vMemorySearchDisconnect (sqlite3_vtab *pVtab) {
    vMemorySearchTable *vtab = (vMemorySearchTable *)pVtab;
    dbmem_free(vtab);
    return SQLITE_OK;
}

static int vMemorySearchBestIndex (sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
    UNUSED_PARAM(tab);
    pIdxInfo->estimatedCost = (double)1;
    pIdxInfo->estimatedRows = 100;
    pIdxInfo->orderByConsumed = 1;
    pIdxInfo->idxNum = 1;
    
    const struct sqlite3_index_constraint *pConstraint = pIdxInfo->aConstraint;
    for(int i=0; i<pIdxInfo->nConstraint; i++, pConstraint++){
        if( pConstraint->usable == 0 ) continue;
        if( pConstraint->op != SQLITE_INDEX_CONSTRAINT_EQ ) continue;
        switch( pConstraint->iColumn ){
            case SEARCH_COLUMN_QUERY:
                pIdxInfo->aConstraintUsage[i].argvIndex = 1;
                pIdxInfo->aConstraintUsage[i].omit = 1;
                break;
            case SEARCH_COLUMN_MAXITEMS:
                pIdxInfo->aConstraintUsage[i].argvIndex = 2;
                pIdxInfo->aConstraintUsage[i].omit = 1;
                break;
            case SEARCH_COLUMN_CONTEXT:
                pIdxInfo->aConstraintUsage[i].argvIndex = 3;
                pIdxInfo->aConstraintUsage[i].omit = 1;
                break;
        }
    }
    return SQLITE_OK;
}

static int vMemorySearchCursorOpen (sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor){
    vMemorySearchCursor *c = (vMemorySearchCursor *)dbmem_zeroalloc(sizeof(vMemorySearchCursor));
    if (!c) return SQLITE_NOMEM;
    
    *ppCursor = (sqlite3_vtab_cursor *)c;
    return SQLITE_OK;
}

static int vMemorySearchCursorClose (sqlite3_vtab_cursor *cur){
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    if (c->buffer) dbmem_free(c->buffer);
    dbmem_free(c);
    return SQLITE_OK;
}

static int vMemorySearchCursorNext (sqlite3_vtab_cursor *cur){
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    c->index++;
    return SQLITE_OK;
}


static int vMemorySearchCursorEof (sqlite3_vtab_cursor *cur){
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    return (c->index == c->count);
}

static int vMemorySearchCursorColumn (sqlite3_vtab_cursor *cur, sqlite3_context *context, int iCol) {
    static const char *path_sql = "SELECT path FROM dbmem_content WHERE hash = ?1;";
    static const char *snippet_sql = "SELECT substr(c.value, v.offset + 1, v.length) FROM dbmem_vault v JOIN dbmem_content c ON v.hash = c.hash WHERE v.hash = ?1 AND v.seq = ?2;";
    
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    sqlite3 *db = ((vMemorySearchTable *)cur->pVtab)->db;
    
    switch (iCol) {
        case SEARCH_COLUMN_HASH:
            sqlite3_result_int64(context, c->hash[c->index]);
            break;
            
        case SEARCH_COLUMN_SEQ:
            sqlite3_result_int64(context, c->seq[c->index]);
            break;
            
        case SEARCH_COLUMN_RANKING:
            sqlite3_result_double(context, c->rank[c->index]);
            break;
            
        case SEARCH_COLUMN_PATH:
        case SEARCH_COLUMN_SNIPPET:{
            const char *sql = (iCol == SEARCH_COLUMN_PATH) ? path_sql : snippet_sql;
            sqlite3_stmt *vm = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &vm, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(vm, 1, c->hash[c->index]);
                if (iCol == SEARCH_COLUMN_SNIPPET) sqlite3_bind_int64(vm, 2, c->seq[c->index]);
                if (sqlite3_step(vm) == SQLITE_ROW) sqlite3_result_value(context, sqlite3_column_value(vm, 0));
            }
            if (vm) sqlite3_finalize(vm);
        }
            break;
    }
    
    return SQLITE_OK;
}

static int vMemorySearchCursorRowid (sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid) {
    // This gives each row a unique rowid (0, 1, 2, ...) which is sufficient and safe
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    *pRowid = c->index;
    return SQLITE_OK;
}

static int vMemorySearchCursorFilter (sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv) {
    sqlite3_vtab *sqlvTab = cur->pVtab;
    vMemorySearchTable *searchTab = (vMemorySearchTable *)sqlvTab;
    vMemorySearchCursor *c = (vMemorySearchCursor *)cur;
    dbmem_context *ctx = searchTab->ctx;
    sqlite3 *db = searchTab->db;
    
    // check and retrieve arguments
    int  max_results = dbmem_context_max_results(ctx);
    bool perform_fts = dbmem_context_perform_fts(ctx);
    const char *query = NULL;
    const char *context = NULL;
    
    if (argc <= 0) {
        sqlvTab->zErrMsg = sqlite3_mprintf("The memory_search function expects at least one query argument of type TEXT");
        return SQLITE_ERROR;
    }
    
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlvTab->zErrMsg = sqlite3_mprintf("The first query argument of memory_search must be of type TEXT");
        return SQLITE_ERROR;
    }
    query = (const char *)sqlite3_value_text(argv[0]);
    
    if (argc > 1) {
        // only the next two arguments are handled
        for (int i=1; i<argc && i<=2; ++i) {
            if (sqlite3_value_type(argv[i]) == SQLITE_INTEGER) max_results = sqlite3_value_int(argv[i]);
            else if (sqlite3_value_type(argv[i]) == SQLITE_TEXT) context = (const char *)sqlite3_value_text(argv[i]);
            // ignore any other type
        }
    }
    
    // compute fetch count (oversampling)
    int oversample = dbmem_context_search_oversample(ctx);
    int fetch_count = (oversample > 0) ? max_results * oversample : max_results;

    // allocate internal cursor buffer
    int rc = vMemorySearchCursorAllocate(c, fetch_count, perform_fts);
    if (rc != SQLITE_OK) return SQLITE_NOMEM;

    // perform semantic search
    // retrieve engine
    bool is_local;
    void *engine = dbmem_context_engine(ctx, &is_local);
    if (!engine) {
        sqlvTab->zErrMsg = sqlite3_mprintf("%s", "Unable to obtain a valid embedding engine");
        return SQLITE_ERROR;
    }
    
    // compute embedding
    embedding_result_t result = {0};
    
    rc = SQLITE_MISUSE;
    #ifndef DBMEM_OMIT_LOCAL_ENGINE
    if (is_local) {
        rc = dbmem_local_compute_embedding((dbmem_local_engine_t *)engine, query, (int)strlen(query), &result);
        if (rc != 0) {
            sqlvTab->zErrMsg = sqlite3_mprintf("%s", dbmem_local_errmsg((dbmem_local_engine_t *)engine));
            return SQLITE_ERROR;
        }
    }
    #endif
    
    #ifndef DBMEM_OMIT_REMOTE_ENGINE
    if (!is_local) {
        rc = dbmem_remote_compute_embedding((dbmem_remote_engine_t *)engine, query, (int)strlen(query), &result);
        if (rc != 0) {
            sqlvTab->zErrMsg = sqlite3_mprintf("%s", dbmem_remote_errmsg((dbmem_remote_engine_t *)engine));
            return SQLITE_ERROR;
        }
    }
    #endif
    
    if (rc == SQLITE_MISUSE) {
        sqlvTab->zErrMsg = sqlite3_mprintf("%s", "Unable to obtain a valid embedding engine");
        return SQLITE_ERROR;
    }
    
    // perform search
    rc = dbmem_semantic_search(db, c, result.embedding, (int)(result.n_embd * sizeof(float)), context, fetch_count);
    if (rc != 0) {
        sqlvTab->zErrMsg = sqlite3_mprintf("%s", sqlite3_errmsg(db));
        return SQLITE_ERROR;
    }

    // perform fts search
    if (perform_fts) {
        // in case of FTS error ignore its contribution
        rc = dbmem_fts_search(db, c, query, context, fetch_count);
        if (rc != SQLITE_OK) perform_fts = false;
    }
    
    // check for empty result
    if (c->semantic.count + c->fts.count == 0) {
        c->count = 0;
        return SQLITE_OK;
    }
    
    // merge results for hybrid search
    // weights are normalized to sum to 1.0
    double vectorWeight = dbmem_context_vector_weight(ctx);
    double textWeight = dbmem_context_text_weight(ctx);
    double min_score = dbmem_context_min_score(ctx);
    vMemorySearchCursorMerge(c, vectorWeight, textWeight, min_score, max_results);

    // update last_accessed timestamps for returned results
    if (dbmem_context_update_access(ctx) && c->count > 0) {
        vMemorySearchUpdateAccess(db, c);
    }

    #if 0
    printf("=================================\n");
    for (int i = 0; i < c->count; i++) {
        double rank = c->rank[i];
        sqlite3_int64 hash = c->hash[i];
        sqlite3_int64 seq = c->seq[i];
        printf("%3d %.3f %20lld %2lld\n", i, rank, (long long)hash, (long long)seq);
    }
    printf("=================================\n");
    #endif
    
    return SQLITE_OK;
}

static sqlite3_module vMemorySearchModule = {
  /* iVersion    */ 0,
  /* xCreate     */ 0,
  /* xConnect    */ vMemorySearchConnect,
  /* xBestIndex  */ vMemorySearchBestIndex,
  /* xDisconnect */ vMemorySearchDisconnect,
  /* xDestroy    */ 0,
  /* xOpen       */ vMemorySearchCursorOpen,
  /* xClose      */ vMemorySearchCursorClose,
  /* xFilter     */ vMemorySearchCursorFilter,
  /* xNext       */ vMemorySearchCursorNext,
  /* xEof        */ vMemorySearchCursorEof,
  /* xColumn     */ vMemorySearchCursorColumn,
  /* xRowid      */ vMemorySearchCursorRowid,
  /* xUpdate     */ 0,
  /* xBegin      */ 0,
  /* xSync       */ 0,
  /* xCommit     */ 0,
  /* xRollback   */ 0,
  /* xFindMethod */ 0,
  /* xRename     */ 0,
  /* xSavepoint  */ 0,
  /* xRelease    */ 0,
  /* xRollbackTo */ 0,
  /* xShadowName */ 0,
  /* xIntegrity  */ 0
};

// MARK: -

int dbmem_register_search (sqlite3 *db, void *ctx, char **pzErrMsg) {
    // lazy loading the init of the vector extension
    return sqlite3_create_module(db, "memory_search", &vMemorySearchModule, ctx);
}

//
//  e2e.c
//  sqlite-memory end-to-end tests
//
//  Tests all SQL functions from the API with actual HTTP requests
//  to the remote embedding API and embedding validation.
//  Requires: APIKEY and VECTOR_LIB environment variables
//  Runs on remote and full build variants only.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "sqlite-memory.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(path) _mkdir(path)
#define RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0755)
#define RMDIR(path) rmdir(path)
#endif

#ifndef TEST_TMP_DIR
#ifdef _WIN32
#define TEST_TMP_DIR "build/test_tmp"
#else
#define TEST_TMP_DIR "/tmp"
#endif
#endif

// Expected embedding dimension for embeddinggemma-300m
#define EXPECTED_DIMENSION 768

// Reference embedding values for the test text below.
// Deterministic for embeddinggemma-300m via vectors.space.
static const char *EMBED_TEST_TEXT = "The quick brown fox jumps over the lazy dog. This is a test of the remote embedding API.";
static const float EXPECTED_EMBEDDING[] = {
     0.05142519f,
     0.01374194f,
    -0.02152035f,
     0.02774420f
};
#define EXPECTED_EMBEDDING_COUNT 4
#define EMBEDDING_TOLERANCE 0.001f

// Test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Running %s... ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("PASSED\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED\n    Assertion failed: %s\n    At %s:%d\n", #cond, __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_SQL_OK(db, sql) do { \
    char *_err = NULL; \
    int _rc = sqlite3_exec(db, sql, NULL, NULL, &_err); \
    if (_rc != SQLITE_OK) { \
        printf("FAILED\n    SQL error: %s\n    Query: %s\n    At %s:%d\n", _err ? _err : "unknown", sql, __FILE__, __LINE__); \
        if (_err) sqlite3_free(_err); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

// Globals
static sqlite3 *db = NULL;
static const char *apikey = NULL;
static const char *vector_lib = NULL;

// Result capture helpers
static char result_buf[4096];
static int capture_string(void *unused, int ncols, char **values, char **names) {
    (void)unused; (void)names;
    if (ncols > 0 && values[0]) {
        snprintf(result_buf, sizeof(result_buf), "%s", values[0]);
    }
    return 0;
}

static int result_int = 0;
static int capture_int(void *unused, int ncols, char **values, char **names) {
    (void)unused; (void)names;
    if (ncols > 0 && values[0]) {
        result_int = atoi(values[0]);
    }
    return 0;
}

// File helper
static void create_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

// ============================================================================
// Phase 1: Setup
// ============================================================================

TEST(memory_version) {
    result_buf[0] = '\0';
    char *err = NULL;
    int rc = sqlite3_exec(db, "SELECT memory_version();", capture_string, NULL, &err);
    ASSERT(rc == SQLITE_OK);
    ASSERT(strlen(result_buf) > 0);
    printf("(v%s) ", result_buf);
}

TEST(load_vector) {
    // Strip file extension — load_extension() appends it automatically
    char path[512];
    snprintf(path, sizeof(path), "%s", vector_lib);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT load_extension('%s');", path);
    ASSERT_SQL_OK(db, sql);

    result_buf[0] = '\0';
    char *err = NULL;
    int rc = sqlite3_exec(db, "SELECT vector_version();", capture_string, NULL, &err);
    ASSERT(rc == SQLITE_OK);
    ASSERT(strlen(result_buf) > 0);
    printf("(vector v%s) ", result_buf);
}

TEST(memory_set_apikey) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_set_apikey('%s');", apikey);
    ASSERT_SQL_OK(db, sql);
}

TEST(memory_set_model) {
    ASSERT_SQL_OK(db, "SELECT memory_set_model('llama', 'embeddinggemma-300m');");
}

// ============================================================================
// Phase 2: Configuration — memory_set_option / memory_get_option
// ============================================================================

TEST(memory_set_get_option) {
    // Set max_tokens
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 512);");

    // Verify via get_option
    result_int = 0;
    sqlite3_exec(db, "SELECT memory_get_option('max_tokens');", capture_int, NULL, NULL);
    ASSERT(result_int == 512);

    // Verify provider persisted from set_model
    result_buf[0] = '\0';
    sqlite3_exec(db, "SELECT memory_get_option('provider');", capture_string, NULL, NULL);
    ASSERT(strcmp(result_buf, "llama") == 0);

    // Verify model persisted from set_model
    result_buf[0] = '\0';
    sqlite3_exec(db, "SELECT memory_get_option('model');", capture_string, NULL, NULL);
    ASSERT(strcmp(result_buf, "embeddinggemma-300m") == 0);

    // Restore default
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 400);");
}

// ============================================================================
// Phase 3: Content Management — network calls
// ============================================================================

// memory_add_text: basic (triggers remote embedding)
TEST(memory_add_text) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_text('%s');", EMBED_TEST_TEXT);
    ASSERT_SQL_OK(db, sql);

    // Verify content stored in dbmem_content
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    ASSERT(result_int == 1);

    // Verify chunk stored in dbmem_vault
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_vault;", capture_int, NULL, NULL);
    ASSERT(result_int == 1);
}

// Verify embedding dimension and values match hardcoded reference
TEST(verify_embedding) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT embedding FROM dbmem_vault LIMIT 1;", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);

    const float *blob = (const float *)sqlite3_column_blob(stmt, 0);
    int bytes = sqlite3_column_bytes(stmt, 0);
    int dim = bytes / (int)sizeof(float);

    ASSERT(blob != NULL);
    ASSERT(dim == EXPECTED_DIMENSION);

    // Compare first N embedding values against hardcoded reference
    for (int i = 0; i < EXPECTED_EMBEDDING_COUNT; i++) {
        float diff = fabsf(blob[i] - EXPECTED_EMBEDDING[i]);
        if (diff > EMBEDDING_TOLERANCE) {
            printf("FAILED\n    Embedding[%d] = %.8f, expected %.8f (diff=%.8f)\n    At %s:%d\n",
                   i, blob[i], EXPECTED_EMBEDDING[i], diff, __FILE__, __LINE__);
            sqlite3_finalize(stmt);
            tests_failed++;
            tests_passed--;
            return;
        }
    }

    printf("(dim=%d, values verified) ", dim);
    sqlite3_finalize(stmt);
}

// memory_add_text with context (triggers remote embedding)
TEST(memory_add_text_context) {
    ASSERT_SQL_OK(db, "SELECT memory_add_text('SQLite is a C-language library that implements a small, fast, self-contained SQL database engine.', 'test-context');");

    // Verify context stored
    result_buf[0] = '\0';
    sqlite3_exec(db, "SELECT context FROM dbmem_content WHERE context IS NOT NULL LIMIT 1;", capture_string, NULL, NULL);
    ASSERT(strcmp(result_buf, "test-context") == 0);

    // Verify total count
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    ASSERT(result_int == 2);
}

// Idempotency: adding the same text again should be a no-op
TEST(memory_add_text_idempotent) {
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_vault;", capture_int, NULL, NULL);
    int before = result_int;

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_text('%s');", EMBED_TEST_TEXT);
    ASSERT_SQL_OK(db, sql);

    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_vault;", capture_int, NULL, NULL);
    ASSERT(result_int == before);
}

#ifndef DBMEM_OMIT_IO

// memory_add_file (triggers remote embedding)
TEST(memory_add_file) {
    const char *filepath = TEST_TMP_DIR "/e2e_test_file.md";
    create_test_file(filepath, "# Test Document\n\nThis markdown file tests the memory_add_file function with remote embedding.");

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_file('%s');", filepath);
    ASSERT_SQL_OK(db, sql);

    // Verify file path stored in dbmem_content
    result_buf[0] = '\0';
    snprintf(sql, sizeof(sql), "SELECT path FROM dbmem_content WHERE path = '%s';", filepath);
    sqlite3_exec(db, sql, capture_string, NULL, NULL);
    ASSERT(strcmp(result_buf, filepath) == 0);

    remove(filepath);
}

// memory_add_directory (triggers remote embedding for each file)
TEST(memory_add_directory) {
    const char *dir = TEST_TMP_DIR "/e2e_test_dir";
    const char *file1 = TEST_TMP_DIR "/e2e_test_dir/doc1.md";
    const char *file2 = TEST_TMP_DIR "/e2e_test_dir/doc2.md";

    MKDIR(dir);
    create_test_file(file1, "# Document One\n\nFirst test document for directory sync.");
    create_test_file(file2, "# Document Two\n\nSecond test document for directory sync.");

    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    int before = result_int;

    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT memory_add_directory('%s');", dir);
    ASSERT_SQL_OK(db, sql);

    // Verify count increased by 2
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    ASSERT(result_int == before + 2);

    remove(file1);
    remove(file2);
    RMDIR(dir);
}

#endif // DBMEM_OMIT_IO

// ============================================================================
// Phase 4: Search — network calls (embeds query via remote API)
// ============================================================================

// memory_search: basic search, verify all columns populated
TEST(memory_search) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT hash, path, context, snippet, ranking FROM memory_search('fox', 5);", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);

    int64_t hash = sqlite3_column_int64(stmt, 0);
    const char *path = (const char *)sqlite3_column_text(stmt, 1);
    const char *snippet = (const char *)sqlite3_column_text(stmt, 3);
    double ranking = sqlite3_column_double(stmt, 4);

    ASSERT(hash != 0);
    ASSERT(path != NULL && strlen(path) > 0);
    ASSERT(snippet != NULL && strlen(snippet) > 0);
    ASSERT(ranking > 0.0 && ranking <= 1.0);

    // Top result should contain the word 'fox'
    ASSERT(strstr(snippet, "fox") != NULL);
    printf("(ranking=%.4f) ", ranking);

    sqlite3_finalize(stmt);
}

// memory_search: verify ranking is within valid bounds
TEST(memory_search_ranking) {
    // Lower min_score so we get results with any ranking
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.0);");

    // Search for something that should match the 'test-context' text
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ranking FROM memory_search('SQL database engine', 10);",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    // Verify all returned rankings are in (0, 1]
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double ranking = sqlite3_column_double(stmt, 0);
        ASSERT(ranking > 0.0 && ranking <= 1.0);
        row_count++;
    }
    ASSERT(row_count > 0);
    printf("(%d results) ", row_count);

    sqlite3_finalize(stmt);

    // Restore default
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.7);");
}

// ============================================================================
// Phase 5: Deletion
// ============================================================================

// memory_delete: delete by hash
TEST(memory_delete) {
    // Get a hash from a context-less entry
    int64_t hash = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT hash FROM dbmem_content WHERE context IS NULL LIMIT 1;", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    hash = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    int before = result_int;

    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT memory_delete(%lld);", (long long)hash);
    ASSERT_SQL_OK(db, sql);

    // Verify count decreased by 1
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    ASSERT(result_int == before - 1);
}

// memory_delete_context: delete all entries with a context
TEST(memory_delete_context) {
    // Verify entries with 'test-context' exist
    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'test-context';", capture_int, NULL, NULL);
    ASSERT(result_int > 0);

    ASSERT_SQL_OK(db, "SELECT memory_delete_context('test-context');");

    // Verify they are gone
    result_int = -1;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'test-context';", capture_int, NULL, NULL);
    ASSERT(result_int == 0);
}

// memory_cache_clear with provider/model args
TEST(memory_cache_clear_model) {
    ASSERT_SQL_OK(db, "SELECT memory_cache_clear('llama', 'embeddinggemma-300m');");
}

// memory_cache_clear: clear all cache
TEST(memory_cache_clear) {
    ASSERT_SQL_OK(db, "SELECT memory_cache_clear();");
}

// memory_clear: clear all data
TEST(memory_clear) {
    ASSERT_SQL_OK(db, "SELECT memory_clear();");

    // Verify tables are empty
    result_int = -1;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    ASSERT(result_int == 0);

    result_int = -1;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_vault;", capture_int, NULL, NULL);
    ASSERT(result_int == 0);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    apikey = getenv("APIKEY");
    if (!apikey || strlen(apikey) == 0) {
        printf("E2E FAILED: APIKEY environment variable not set\n");
        return 1;
    }

    vector_lib = getenv("VECTOR_LIB");
    if (!vector_lib || strlen(vector_lib) == 0) {
        printf("E2E FAILED: VECTOR_LIB environment variable not set\n");
        return 1;
    }

    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        printf("Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Init sqlite-memory (SQLITE_CORE mode — also enables load_extension)
    sqlite3_memory_init(db, NULL, NULL);

    printf("E2E tests (all API functions + embedding validation):\n");

    // Phase 1: Setup
    RUN_TEST(memory_version);
    RUN_TEST(load_vector);
    RUN_TEST(memory_set_apikey);
    RUN_TEST(memory_set_model);

    // Phase 2: Configuration
    RUN_TEST(memory_set_get_option);

    // Phase 3: Content Management (network calls)
    RUN_TEST(memory_add_text);
    RUN_TEST(verify_embedding);
    RUN_TEST(memory_add_text_context);
    RUN_TEST(memory_add_text_idempotent);
#ifndef DBMEM_OMIT_IO
    RUN_TEST(memory_add_file);
    RUN_TEST(memory_add_directory);
#endif

    // Phase 4: Search (network calls)
    RUN_TEST(memory_search);
    RUN_TEST(memory_search_ranking);

    // Phase 5: Deletion
    RUN_TEST(memory_delete);
    RUN_TEST(memory_delete_context);
    RUN_TEST(memory_cache_clear_model);
    RUN_TEST(memory_cache_clear);
    RUN_TEST(memory_clear);

    sqlite3_close(db);

    printf("\n=== E2E Results ===\n");
    printf("Tests run:     %d\n", tests_run);
    printf("Tests passed:  %d\n", tests_passed);
    printf("Tests failed:  %d\n", tests_failed);
    printf("\n");

    return tests_failed > 0 ? 1 : 0;
}

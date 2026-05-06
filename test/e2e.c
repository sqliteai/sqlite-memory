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
#include "dbmem-utils.h"
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
    int _failed_before = tests_failed; \
    printf("  Running %s... ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    if (tests_failed == _failed_before) printf("PASSED\n"); \
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

static int get_vault_metadata(const char *hash, int *chunk_count, int *min_tokens, int *min_truncated, int *max_truncated) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*), COALESCE(MIN(n_tokens), 0), COALESCE(MIN(truncated), 0), COALESCE(MAX(truncated), 0) "
        "FROM dbmem_vault WHERE hash = ?1;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return rc;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (chunk_count) *chunk_count = sqlite3_column_int(stmt, 0);
        if (min_tokens) *min_tokens = sqlite3_column_int(stmt, 1);
        if (min_truncated) *min_truncated = sqlite3_column_int(stmt, 2);
        if (max_truncated) *max_truncated = sqlite3_column_int(stmt, 3);
        rc = SQLITE_OK;
    }

    sqlite3_finalize(stmt);
    return rc;
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
    char path[1024];
    snprintf(path, sizeof(path), "%s", vector_lib);
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';

    char sql[1100];
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

// Verify remote embedding metadata is persisted on the stored chunk.
TEST(verify_embedding_metadata) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT n_tokens, truncated FROM dbmem_vault LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);

    int n_tokens = sqlite3_column_int(stmt, 0);
    int truncated = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);

    ASSERT(n_tokens > 0);
    ASSERT(truncated == 0);
    printf("(n_tokens=%d, truncated=%d) ", n_tokens, truncated);
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

    const char *hash = (const char *)sqlite3_column_text(stmt, 0);
    const char *path = (const char *)sqlite3_column_text(stmt, 1);
    const char *snippet = (const char *)sqlite3_column_text(stmt, 3);
    double ranking = sqlite3_column_double(stmt, 4);

    ASSERT(hash != NULL && strlen(hash) == DBMEM_HASH_HEX_LEN);
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

TEST(memory_search_statement_reuse) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT hash, snippet FROM memory_search(?1, ?2);",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    rc = sqlite3_bind_text(stmt, 1, "fox", -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_int(stmt, 2, 5);
    ASSERT(rc == SQLITE_OK);

    int first_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        const char *snippet = (const char *)sqlite3_column_text(stmt, 1);
        ASSERT(hash != NULL && strlen(hash) == DBMEM_HASH_HEX_LEN);
        ASSERT(snippet != NULL && strlen(snippet) > 0);
        first_count++;
    }
    ASSERT(rc == SQLITE_DONE);
    ASSERT(first_count > 0);

    rc = sqlite3_reset(stmt);
    ASSERT(rc == SQLITE_OK);
    sqlite3_clear_bindings(stmt);

    rc = sqlite3_bind_text(stmt, 1, "SQL database engine", -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_int(stmt, 2, 10);
    ASSERT(rc == SQLITE_OK);

    int second_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        const char *snippet = (const char *)sqlite3_column_text(stmt, 1);
        ASSERT(hash != NULL && strlen(hash) == DBMEM_HASH_HEX_LEN);
        ASSERT(snippet != NULL && strlen(snippet) > 0);
        second_count++;
    }
    ASSERT(rc == SQLITE_DONE);
    ASSERT(second_count > 0);

    sqlite3_finalize(stmt);
}

// ============================================================================
// Phase 4b: Long-text chunking + multi-section retrieval
// ============================================================================

// A long text with 4 clearly distinct sections, each tagged with a unique
// anchor token so we can verify both (a) the chunker covers the whole text
// and (b) section-specific queries retrieve the matching chunk.
#define LONG_TEXT_ANCHOR_COOKING  "ZANZIBAR-PASTA"
#define LONG_TEXT_ANCHOR_KERNEL   "QUOKKA-SCHEDULER"
#define LONG_TEXT_ANCHOR_VIOLIN   "TARANTELLA-BRIDGE"
#define LONG_TEXT_ANCHOR_ASTRO    "BETELGEUSE-PARALLAX"

static const char *LONG_TEXT =
    // Section 1 - cooking
    "Cooking pasta well begins with abundant salted water at a rolling boil. "
    "The " LONG_TEXT_ANCHOR_COOKING " technique calls for finishing the noodles "
    "directly in the sauce, ladling in starchy cooking water until the emulsion "
    "clings to each strand. Timing matters more than the package suggests: pull "
    "the pasta a minute early and let the residual heat do the rest. "
    "Salt aggressively. Stir often. Reserve water before draining. Toss vigorously. "
    "Salt aggressively. Stir often. Reserve water before draining. Toss vigorously. "
    "\n\n"
    // Section 2 - kernel scheduling
    "Operating system schedulers balance throughput against latency under load. "
    "The " LONG_TEXT_ANCHOR_KERNEL " design favors short interactive tasks by "
    "boosting their effective priority for a brief window after a wakeup event, "
    "then decaying that boost as CPU time accumulates. This avoids starving "
    "background batch work while keeping UI threads responsive. "
    "Run queues, vruntime, and load balancing across cores all interact here. "
    "Run queues, vruntime, and load balancing across cores all interact here. "
    "\n\n"
    // Section 3 - violin
    "A violin's tone depends as much on setup as on the maker. The "
    LONG_TEXT_ANCHOR_VIOLIN " is shaped from well-aged maple and positioned to "
    "transmit string vibration to the top plate without damping the upper "
    "partials. Soundpost placement, tailgut tension, and bow rosin all subtly "
    "shift the instrument's voice. "
    "Maple, spruce, varnish, and time. Maple, spruce, varnish, and time. "
    "\n\n"
    // Section 4 - astronomy
    "Measuring stellar distances requires careful baseline geometry. The "
    LONG_TEXT_ANCHOR_ASTRO " measurement is challenging because the star is a "
    "pulsating red supergiant whose photosphere is not well defined. Modern "
    "interferometry combined with Gaia astrometry has narrowed the uncertainty "
    "but not eliminated it. "
    "Parallax, redshift, standard candles, distance ladder. "
    "Parallax, redshift, standard candles, distance ladder. ";

// Structural: long text produces multiple chunks that fully cover the input,
// every chunk has a valid embedding, and chunk offsets are well-formed.
TEST(memory_add_long_text_chunking) {
    // Force raw-text chunking so the chunk count is determined by
    // max_tokens/overlay_tokens, not by markdown structure.
    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 1);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 80);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 16);");

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT memory_add_text(?1, 'long-text');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, LONG_TEXT, -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    char hash[DBMEM_HASH_STR_MAXLEN] = {0};
    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'long-text' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(hash, sizeof(hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    ASSERT(strlen(hash) == DBMEM_HASH_HEX_LEN);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%s';", hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    int chunk_count = result_int;
    ASSERT(chunk_count >= 3);

    snprintf(sql, sizeof(sql),
        "SELECT seq, offset, length, embedding, n_tokens, truncated FROM dbmem_vault "
        "WHERE hash = '%s' ORDER BY seq;", hash);
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    int prev_seq = -1;
    int prev_offset = -1;
    int last_offset = 0, last_length = 0;
    int seen = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int seq    = sqlite3_column_int(stmt, 0);
        int offset = sqlite3_column_int(stmt, 1);
        int length = sqlite3_column_int(stmt, 2);
        int bytes  = sqlite3_column_bytes(stmt, 3);
        int n_tokens = sqlite3_column_int(stmt, 4);
        int truncated = sqlite3_column_int(stmt, 5);

        ASSERT(seq == prev_seq + 1);
        ASSERT(offset >= prev_offset);
        ASSERT(length > 0);
        ASSERT(bytes == EXPECTED_DIMENSION * (int)sizeof(float));
        ASSERT(n_tokens > 0);
        ASSERT(truncated == 0);

        prev_seq = seq;
        prev_offset = offset;
        last_offset = offset;
        last_length = length;
        seen++;
    }
    sqlite3_finalize(stmt);
    ASSERT(seen == chunk_count);

    int total = (int)strlen(LONG_TEXT);
    // Allow small tail slack for trailing-whitespace trimming by the parser.
    ASSERT(last_offset + last_length >= total - 8);

    printf("(%d chunks covering %d bytes) ", chunk_count, total);

    // Restore defaults for downstream tests.
    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 400);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 80);");
}

// Retrieval: each section is reachable by a query phrase from that section.
// Asserts on anchor-token presence in the top-3 snippets, not absolute
// ranking, so minor embedding drift will not flake the test.
TEST(memory_search_long_text_sections) {
    struct { const char *query; const char *anchor; } cases[] = {
        { "finishing pasta in the sauce with starchy water", LONG_TEXT_ANCHOR_COOKING },
        { "boosting interactive task priority after wakeup", LONG_TEXT_ANCHOR_KERNEL  },
        { "soundpost placement and string vibration",        LONG_TEXT_ANCHOR_VIOLIN  },
        { "measuring stellar distance with parallax",        LONG_TEXT_ANCHOR_ASTRO   },
    };
    int n_cases = (int)(sizeof(cases) / sizeof(cases[0]));

    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.0);");

    int matched = 0;
    for (int i = 0; i < n_cases; i++) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db,
            "SELECT snippet FROM memory_search(?1, 3);", -1, &stmt, NULL);
        ASSERT(rc == SQLITE_OK);
        rc = sqlite3_bind_text(stmt, 1, cases[i].query, -1, SQLITE_STATIC);
        ASSERT(rc == SQLITE_OK);

        int found = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *snippet = (const char *)sqlite3_column_text(stmt, 0);
            if (snippet && strstr(snippet, cases[i].anchor)) { found = 1; break; }
        }
        sqlite3_finalize(stmt);

        if (!found) {
            printf("FAILED\n    Query '%s' did not retrieve anchor '%s' in top 3\n",
                   cases[i].query, cases[i].anchor);
            tests_failed++;
            tests_passed--;
            return;
        }
        matched++;
    }

    printf("(%d/%d sections retrieved) ", matched, n_cases);

    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.7);");
}

// ============================================================================
// Phase 4c: Single-chunk near the provider token ceiling
// ============================================================================

// Control test for memory_search_truncation_signature below: same setup
// (single-chunk-everything, pure-vector ranking, leading-mosaics + tail-
// vents text alongside a short vent reference) but the long text is sized
// to land *under* vectors.space's 1024-token batch ceiling. Expectations:
//
//   1) The long chunk embeds successfully (no provider rejection).
//   2) Stored as exactly one chunk in dbmem_vault.
//   3) A tail-topic query retrieves both the short reference and the long
//      chunk in the top-10 — confirming the tail was included in the
//      embedding when the input fit in one batch.
//
// Sized at ~5200 bytes. Empirical calibration: 7159 / 9346 / 10075 bytes
// all rejected with the same "input (1026 tokens)" template (so "1026" is
// not a real count — just an "exceeded" sentinel). 7159 / 1024 ≈ 7.0
// chars-per-token actual ratio for this filler, so 5200 bytes ≈ ~740
// tokens — clear of the 1024 ceiling.
TEST(memory_search_under_token_limit) {
    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 1);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 2048);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 1.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.0);");

    static const char *SHORT_REF =
        "Hydrothermal vents on the deep ocean floor sustain chemosynthetic "
        "microbial ecosystems independent of sunlight. Tubeworms and "
        "thermophilic archaea metabolize sulfur compounds emitted by the "
        "vent fluids in total darkness.";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'under-limit-short');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, SHORT_REF, -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    static const char *MOSAIC_LEAD =
        "Andalusian zellige mosaics from medieval Granada and Cordoba feature "
        "interlocking geometric tiles arranged in repeating decagonal motifs "
        "of cobalt and ochre glaze. ";
    static const char *MOSAIC_FILLER =
        "Master craftsmen historically cut tesserae from glazed terracotta "
        "and fit them into intricate patterns whose mathematical foundations "
        "anticipate aperiodic tilings by centuries; pigments include lapis "
        "lazuli, copper carbonate, and iron oxides. ";
    static const char *VENT_TAIL =
        " And entirely separately, deep ocean hydrothermal vents host "
        "chemosynthetic communities of microbial mats, tubeworms, and "
        "thermophilic archaea metabolizing sulfur compounds in total darkness.";

    size_t cap = 16 * 1024;
    char *long_text = (char *)malloc(cap);
    ASSERT(long_text != NULL);
    size_t pos = 0;
    int n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_LEAD);
    pos += (size_t)n;
    while (pos < 5000
           && pos + strlen(MOSAIC_FILLER) + strlen(VENT_TAIL) + 4 < cap) {
        n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_FILLER);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    n = snprintf(long_text + pos, cap - pos, "%s", VENT_TAIL);
    pos += (size_t)n;
    int long_text_len = (int)pos;

    rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'under-limit-long');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, long_text, long_text_len, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("FAILED\n    memory_add_text(%d bytes) returned rc=%d\n    sqlite error: %s\n",
               long_text_len, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(long_text);
        tests_failed++;
        tests_passed--;
        return;
    }
    sqlite3_finalize(stmt);
    free(long_text);

    char short_hash[DBMEM_HASH_STR_MAXLEN] = {0};
    char long_hash[DBMEM_HASH_STR_MAXLEN]  = {0};
    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'under-limit-short' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(short_hash, sizeof(short_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'under-limit-long' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(long_hash, sizeof(long_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    // Single chunk, length around ~5KB but under the rejection threshold.
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%s';", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    ASSERT(result_int == 1);

    snprintf(sql, sizeof(sql),
        "SELECT length FROM dbmem_vault WHERE hash = '%s' LIMIT 1;", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    int long_chunk_bytes = result_int;
    ASSERT(long_chunk_bytes > 4500);

    int chunk_count = 0, min_tokens = 0, min_truncated = 0, max_truncated = 0;
    rc = get_vault_metadata(short_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 0 && max_truncated == 0);

    rc = get_vault_metadata(long_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 0 && max_truncated == 0);

    // Same query as the truncation test; with the full chunk embedded we
    // expect both the short ref and the long chunk to surface in top-10.
    rc = sqlite3_prepare_v2(db,
        "SELECT hash, ranking FROM memory_search("
        "  'chemosynthesis around deep-sea volcanic vents', 10);",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    int short_rank = -1, long_rank = -1;
    double short_score = 0.0, long_score = 0.0;
    int row = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        double rank = sqlite3_column_double(stmt, 1);
        if (hash && strcmp(hash, short_hash) == 0) {
            short_rank = row; short_score = rank;
        }
        if (hash && strcmp(hash, long_hash) == 0) {
            long_rank  = row; long_score  = rank;
        }
        row++;
    }
    sqlite3_finalize(stmt);

    ASSERT(short_rank >= 0);
    ASSERT(long_rank >= 0);

    printf("(%d bytes; short rank=%d score=%.3f, long rank=%d score=%.3f) ",
           long_chunk_bytes, short_rank, short_score, long_rank, long_score);

    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 400);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 80);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 0.6);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.4);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.7);");
}

// ============================================================================
// Phase 4d: Model-level truncation behavioral signature
// ============================================================================

// When a single chunk exceeds the embedding model's input context window
// (embeddinggemma-300m: ~2048 tokens), the service truncates and returns an
// embedding that only represents the leading portion. The truncated flag is
// persisted on dbmem_vault, and this test also checks the observable search
// behavior:
//
//   1) Store a SHORT reference (fully embedded) entirely about topic T.
//   2) Store a LONG single-chunk document whose LEADING ~10KB is about an
//      unrelated topic and whose final ~250 bytes (well past the 2048-token
//      window) introduce topic T.
//   3) Search for topic T with pure-vector ranking.
//
// If the long chunk's embedding includes the tail, both should rank in the
// same neighborhood. If truncated, the long chunk's embedding only encodes
// the unrelated leading topic and ranks far below the short reference (or
// drops out of the top-K entirely).
TEST(memory_search_truncation_signature) {
    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 1);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 3000);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 1.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.0);");

    // Short reference (~50 tokens), fully embedded, entirely about the topic.
    static const char *SHORT_REF =
        "Hydrothermal vents on the deep ocean floor sustain chemosynthetic "
        "microbial ecosystems independent of sunlight. Tubeworms and "
        "thermophilic archaea metabolize sulfur compounds emitted by the "
        "vent fluids in total darkness.";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'trunc-short');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, SHORT_REF, -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    // Build ~10KB single-chunk text: leading + filler about Andalusian
    // mosaics, then a final ~250-byte tail introducing hydrothermal vents.
    // ~10KB / ~4 chars-per-token ≈ 2500 tokens — past gemma's 2048 window.
    static const char *MOSAIC_LEAD =
        "Andalusian zellige mosaics from medieval Granada and Cordoba feature "
        "interlocking geometric tiles arranged in repeating decagonal motifs "
        "of cobalt and ochre glaze. ";
    static const char *MOSAIC_FILLER =
        "Master craftsmen historically cut tesserae from glazed terracotta "
        "and fit them into intricate patterns whose mathematical foundations "
        "anticipate aperiodic tilings by centuries; pigments include lapis "
        "lazuli, copper carbonate, and iron oxides. ";
    static const char *VENT_TAIL =
        " And entirely separately, deep ocean hydrothermal vents host "
        "chemosynthetic communities of microbial mats, tubeworms, and "
        "thermophilic archaea metabolizing sulfur compounds in total darkness.";

    size_t cap = 16 * 1024;
    char *long_text = (char *)malloc(cap);
    ASSERT(long_text != NULL);
    size_t pos = 0;
    int n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_LEAD);
    pos += (size_t)n;
    while (pos < 9800
           && pos + strlen(MOSAIC_FILLER) + strlen(VENT_TAIL) + 4 < cap) {
        n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_FILLER);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    n = snprintf(long_text + pos, cap - pos, "%s", VENT_TAIL);
    pos += (size_t)n;
    int long_text_len = (int)pos;

    rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'trunc-long');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, long_text, long_text_len, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("FAILED\n    memory_add_text(%d bytes) returned rc=%d\n    sqlite error: %s\n",
               long_text_len, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(long_text);
        tests_failed++;
        tests_passed--;
        return;
    }
    sqlite3_finalize(stmt);
    free(long_text);

    // Capture both hashes.
    char short_hash[DBMEM_HASH_STR_MAXLEN] = {0};
    char long_hash[DBMEM_HASH_STR_MAXLEN]  = {0};
    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'trunc-short' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(short_hash, sizeof(short_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'trunc-long' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(long_hash, sizeof(long_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    // Confirm the long content stored as one chunk past gemma's window.
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%s';", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    ASSERT(result_int == 1);

    snprintf(sql, sizeof(sql),
        "SELECT length FROM dbmem_vault WHERE hash = '%s' LIMIT 1;", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    int long_chunk_bytes = result_int;
    // ~2048 tokens × ~4 chars/token = ~8192 chars; chunk must clearly exceed.
    ASSERT(long_chunk_bytes > 9000);

    int chunk_count = 0, min_tokens = 0, min_truncated = 0, max_truncated = 0;
    rc = get_vault_metadata(short_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 0 && max_truncated == 0);

    rc = get_vault_metadata(long_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 1 && max_truncated == 1);

    // Query for the topic that appears throughout the short reference and
    // only in the *tail* of the long chunk. Paraphrased so any residual FTS
    // contribution would match both texts roughly equally.
    rc = sqlite3_prepare_v2(db,
        "SELECT hash, ranking FROM memory_search("
        "  'chemosynthesis around deep-sea volcanic vents', 10);",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    int short_rank = -1, long_rank = -1;
    double short_score = 0.0, long_score = 0.0;
    int row = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        double rank = sqlite3_column_double(stmt, 1);
        if (hash && strcmp(hash, short_hash) == 0) {
            short_rank = row; short_score = rank;
        }
        if (hash && strcmp(hash, long_hash) == 0) {
            long_rank  = row; long_score  = rank;
        }
        row++;
    }
    sqlite3_finalize(stmt);

    ASSERT(short_rank >= 0);
    if (long_rank == -1) {
        printf("(short rank=%d score=%.3f, long absent from top-10) ",
               short_rank, short_score);
    } else {
        // With a fully-embedded long chunk we'd expect comparable rankings;
        // truncation pushes the long chunk strictly below the short ref.
        ASSERT(short_rank < long_rank);
        printf("(short rank=%d score=%.3f, long rank=%d score=%.3f) ",
               short_rank, short_score, long_rank, long_score);
    }

    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 400);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 80);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 0.6);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.4);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.7);");
}

// ============================================================================
// Phase 4e: Truncation signature near the model context window (~2000 tok)
// ============================================================================

// Same shape as memory_search_truncation_signature, but with a long text
// sized at ~19500 bytes / ~9.8 chars-per-token ≈ ~1990 tokens — close to
// embeddinggemma-300m's documented 2048-token context window. Useful for
// observing how the provider behaves further past the 1024-token batch
// ceiling: same rejection error, a different message, or (if the batch
// size is raised on the server) a successful embed where truncation
// actually occurs.
TEST(memory_search_truncation_near_model_context) {
    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 1);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 6000);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 1.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.0);");

    static const char *SHORT_REF =
        "Hydrothermal vents on the deep ocean floor sustain chemosynthetic "
        "microbial ecosystems independent of sunlight. Tubeworms and "
        "thermophilic archaea metabolize sulfur compounds emitted by the "
        "vent fluids in total darkness.";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'trunc-large-short');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, SHORT_REF, -1, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    ASSERT(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    static const char *MOSAIC_LEAD =
        "Andalusian zellige mosaics from medieval Granada and Cordoba feature "
        "interlocking geometric tiles arranged in repeating decagonal motifs "
        "of cobalt and ochre glaze. ";
    static const char *MOSAIC_FILLER =
        "Master craftsmen historically cut tesserae from glazed terracotta "
        "and fit them into intricate patterns whose mathematical foundations "
        "anticipate aperiodic tilings by centuries; pigments include lapis "
        "lazuli, copper carbonate, and iron oxides. ";
    static const char *VENT_TAIL =
        " And entirely separately, deep ocean hydrothermal vents host "
        "chemosynthetic communities of microbial mats, tubeworms, and "
        "thermophilic archaea metabolizing sulfur compounds in total darkness.";

    size_t cap = 32 * 1024;
    char *long_text = (char *)malloc(cap);
    ASSERT(long_text != NULL);
    size_t pos = 0;
    int n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_LEAD);
    pos += (size_t)n;
    while (pos < 19300
           && pos + strlen(MOSAIC_FILLER) + strlen(VENT_TAIL) + 4 < cap) {
        n = snprintf(long_text + pos, cap - pos, "%s", MOSAIC_FILLER);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    n = snprintf(long_text + pos, cap - pos, "%s", VENT_TAIL);
    pos += (size_t)n;
    int long_text_len = (int)pos;

    rc = sqlite3_prepare_v2(db,
        "SELECT memory_add_text(?1, 'trunc-large-long');", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_bind_text(stmt, 1, long_text, long_text_len, SQLITE_STATIC);
    ASSERT(rc == SQLITE_OK);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("FAILED\n    memory_add_text(%d bytes) returned rc=%d\n    sqlite error: %s\n",
               long_text_len, rc, sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        free(long_text);
        tests_failed++;
        tests_passed--;
        return;
    }
    sqlite3_finalize(stmt);
    free(long_text);

    char short_hash[DBMEM_HASH_STR_MAXLEN] = {0};
    char long_hash[DBMEM_HASH_STR_MAXLEN]  = {0};
    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'trunc-large-short' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(short_hash, sizeof(short_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db,
        "SELECT hash FROM dbmem_content WHERE context = 'trunc-large-long' LIMIT 1;",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(long_hash, sizeof(long_hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM dbmem_vault WHERE hash = '%s';", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    ASSERT(result_int == 1);

    snprintf(sql, sizeof(sql),
        "SELECT length FROM dbmem_vault WHERE hash = '%s' LIMIT 1;", long_hash);
    result_int = 0;
    sqlite3_exec(db, sql, capture_int, NULL, NULL);
    int long_chunk_bytes = result_int;
    ASSERT(long_chunk_bytes > 18000);

    int chunk_count = 0, min_tokens = 0, min_truncated = 0, max_truncated = 0;
    rc = get_vault_metadata(short_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 0 && max_truncated == 0);

    rc = get_vault_metadata(long_hash, &chunk_count, &min_tokens, &min_truncated, &max_truncated);
    ASSERT(rc == SQLITE_OK);
    ASSERT(chunk_count == 1);
    ASSERT(min_tokens > 0);
    ASSERT(min_truncated == 1 && max_truncated == 1);

    rc = sqlite3_prepare_v2(db,
        "SELECT hash, ranking FROM memory_search("
        "  'chemosynthesis around deep-sea volcanic vents', 10);",
        -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK);

    int short_rank = -1, long_rank = -1;
    double short_score = 0.0, long_score = 0.0;
    int row = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        double rank = sqlite3_column_double(stmt, 1);
        if (hash && strcmp(hash, short_hash) == 0) {
            short_rank = row; short_score = rank;
        }
        if (hash && strcmp(hash, long_hash) == 0) {
            long_rank  = row; long_score  = rank;
        }
        row++;
    }
    sqlite3_finalize(stmt);

    ASSERT(short_rank >= 0);
    if (long_rank == -1) {
        printf("(%d bytes; short rank=%d score=%.3f, long absent from top-10) ",
               long_chunk_bytes, short_rank, short_score);
    } else {
        ASSERT(short_rank < long_rank);
        printf("(%d bytes; short rank=%d score=%.3f, long rank=%d score=%.3f) ",
               long_chunk_bytes, short_rank, short_score, long_rank, long_score);
    }

    ASSERT_SQL_OK(db, "SELECT memory_set_option('skip_semantic', 0);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('max_tokens', 400);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('overlay_tokens', 80);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('vector_weight', 0.6);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('text_weight', 0.4);");
    ASSERT_SQL_OK(db, "SELECT memory_set_option('min_score', 0.7);");
}

// ============================================================================
// Phase 5: Deletion
// ============================================================================

// memory_delete: delete by hash
TEST(memory_delete) {
    // Get a hash from a context-less entry
    char hash[DBMEM_HASH_STR_MAXLEN] = {0};
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT hash FROM dbmem_content WHERE context IS NULL LIMIT 1;", -1, &stmt, NULL);
    ASSERT(rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW);
    snprintf(hash, sizeof(hash), "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    result_int = 0;
    sqlite3_exec(db, "SELECT COUNT(*) FROM dbmem_content;", capture_int, NULL, NULL);
    int before = result_int;

    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT memory_delete('%s');", hash);
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
    RUN_TEST(verify_embedding_metadata);
    RUN_TEST(memory_add_text_context);
    RUN_TEST(memory_add_text_idempotent);
#ifndef DBMEM_OMIT_IO
    RUN_TEST(memory_add_file);
    RUN_TEST(memory_add_directory);
#endif

    // Phase 4: Search (network calls)
    RUN_TEST(memory_search);
    RUN_TEST(memory_search_ranking);
    RUN_TEST(memory_search_statement_reuse);

    // Phase 4b: Long-text chunking + multi-section retrieval
    RUN_TEST(memory_add_long_text_chunking);
    RUN_TEST(memory_search_long_text_sections);

    // Phase 4c: Single-chunk near (under) the provider token ceiling
    RUN_TEST(memory_search_under_token_limit);

    // Phase 4d: Model-level truncation behavioral signature
    RUN_TEST(memory_search_truncation_signature);

    // Phase 4e: Same shape, but text size pushed near the model context window
    RUN_TEST(memory_search_truncation_near_model_context);

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

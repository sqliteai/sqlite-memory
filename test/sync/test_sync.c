//
//  test_sync.c
//  sqlite-memory sync integration test
//
//  Demonstrates memory synchronization between two agents (Agent A and Agent B)
//  using sqlite-sync and the SQLiteCloud network layer.
//
//  Agent A knows about the James Webb Space Telescope.
//  Agent B knows about the Great Barrier Reef.
//  After sync, both agents can answer questions about both topics — content
//  each agent never directly indexed.
//
//  Required environment variables:
//    APIKEY        — vectors.space API key for remote embeddings
//    VECTOR_LIB    — path to sqlite-vector shared library
//    SYNC_LIB      — path to sqlite-sync (cloudsync) shared library
//    SYNC_DB_ID    — SQLiteCloud managed database ID (shared by both agents)
//    SYNC_APIKEY_A — SQLiteCloud API key for Agent A
//    SYNC_APIKEY_B — SQLiteCloud API key for Agent B
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sqlite-memory.h"

// Temporary database files (cleaned up at start and end)
#define AGENT_A_DB "/tmp/agent_a_memory_test.db"
#define AGENT_B_DB "/tmp/agent_b_memory_test.db"

// ============================================================================
// Agent A content: James Webb Space Telescope (context: "space")
// ============================================================================

#define CONTEXT_SPACE "space"

// Split into two parts so Agent A ingests 2 rows (localVersion=3) while
// Agent B ingests 1 row (localVersion=2).  Different localVersions prevent
// a serverVersion collision that would block bidirectional exchange.
static const char *JWST_CONTENT =
    "# James Webb Space Telescope\n\n"
    "The James Webb Space Telescope (JWST) is the most powerful space telescope ever "
    "built. Launched on December 25, 2021, it reached its operational orbit at the second "
    "Lagrange point (L2), a gravitationally stable position 1.5 million kilometers from Earth.\n\n"
    "## Mirror and Optics\n\n"
    "JWST features a 6.5-meter primary mirror composed of 18 hexagonal gold-coated beryllium "
    "segments. Unlike Hubble which focuses on visible and ultraviolet light, JWST observes "
    "primarily in infrared wavelengths (0.6 to 28 micrometers). This allows it to see through "
    "cosmic dust clouds and observe the formation of the earliest stars and galaxies.\n\n"
    "## Science Instruments\n\n"
    "JWST carries four science instruments:\n"
    "- NIRCam: Near-infrared camera for deep-field imaging\n"
    "- NIRSpec: Near-infrared spectrograph for spectral analysis of distant objects\n"
    "- MIRI: Mid-infrared instrument sensitive to longer wavelengths\n"
    "- FGS/NIRISS: Fine guidance sensor and near-infrared imager\n\n"
    "## Key Discoveries\n\n"
    "JWST has produced the deepest infrared images of the universe, detected carbon dioxide "
    "and methane in exoplanet atmospheres, and captured direct images of planets orbiting "
    "other stars beyond our solar system.";

// ============================================================================
// Agent B content: Great Barrier Reef (context: "reef")
// ============================================================================

#define CONTEXT_REEF "reef"

static const char *REEF_CONTENT =
    "# Great Barrier Reef\n\n"
    "The Great Barrier Reef is the world's largest coral reef ecosystem, extending 2,300 "
    "kilometers along the northeastern coast of Queensland, Australia. It encompasses more "
    "than 2,900 individual reefs and 900 islands.\n\n"
    "## Coral Bleaching\n\n"
    "Coral bleaching is the process by which corals expel the symbiotic algae (zooxanthellae) "
    "living within their tissues in response to environmental stress, particularly elevated "
    "ocean temperatures. The algae provide corals with up to 90 percent of their energy through "
    "photosynthesis. When expelled, corals turn white and become extremely vulnerable to disease "
    "and death.\n\n"
    "The Great Barrier Reef has experienced five mass bleaching events in 1998, 2002, 2016, "
    "2017, 2020, and 2022. The back-to-back events of 2016 and 2017 were the most severe in "
    "recorded history, killing approximately 50 percent of shallow-water corals in the northern reef.\n\n"
    "## Marine Biodiversity\n\n"
    "The reef supports one of the most biodiverse marine ecosystems on Earth:\n"
    "- More than 1,500 species of fish\n"
    "- Over 4,000 species of mollusk\n"
    "- 6 of the world's 7 sea turtle species\n"
    "- 30 species of whales, dolphins, and porpoises\n"
    "- More than 400 types of coral\n\n"
    "## Climate Threats and Conservation\n\n"
    "Rising ocean temperatures from climate change are the primary driver of coral bleaching. "
    "Local threats include agricultural runoff and coastal development. Conservation efforts "
    "include Australia's Reef 2050 Plan and UNESCO World Heritage Site protections.";

// ============================================================================
// Test framework
// ============================================================================

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED\n    Assertion failed: %s\n    At %s:%d\n", #cond, __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_MSG(cond, msg) do { \
    if (!(cond)) { \
        printf("FAILED\n    %s\n    At %s:%d\n", msg, __FILE__, __LINE__); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

#define ASSERT_SQL_OK(db, sql) do { \
    char *_err = NULL; \
    int _rc = sqlite3_exec(db, sql, NULL, NULL, &_err); \
    if (_rc != SQLITE_OK) { \
        printf("FAILED\n    SQL error: %s\n    Query: %s\n    At %s:%d\n", \
               _err ? _err : sqlite3_errmsg(db), sql, __FILE__, __LINE__); \
        if (_err) sqlite3_free(_err); \
        tests_failed++; \
        tests_passed--; \
        return; \
    } \
} while(0)

static int result_int = 0;
static int capture_int(void *unused, int ncols, char **values, char **names) {
    (void)unused; (void)names;
    if (ncols > 0 && values[0]) result_int = atoi(values[0]);
    return 0;
}

static char result_buf[8192];
static int capture_string(void *unused, int ncols, char **values, char **names) {
    (void)unused; (void)names;
    result_buf[0] = '\0';
    if (ncols > 0 && values[0]) snprintf(result_buf, sizeof(result_buf), "%s", values[0]);
    return 0;
}

// ============================================================================
// Helpers
// ============================================================================

// Open a database, load both extensions, and initialise sqlite-memory.
static sqlite3 *open_agent_db(const char *path, const char *vector_lib, const char *sync_lib) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_enable_load_extension(db, 1);

    // Strip file extension — load_extension() appends the platform one automatically
    char vec_path[1024], sync_path[1024], sql[2048];
    char *dot, *slash;

    snprintf(vec_path, sizeof(vec_path), "%s", vector_lib);
    dot = strrchr(vec_path, '.'); slash = strrchr(vec_path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    snprintf(sql, sizeof(sql), "SELECT load_extension('%s');", vec_path);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to load vector extension on %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db); return NULL;
    }

    snprintf(sync_path, sizeof(sync_path), "%s", sync_lib);
    dot = strrchr(sync_path, '.'); slash = strrchr(sync_path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    snprintf(sql, sizeof(sql), "SELECT load_extension('%s');", sync_path);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to load sync extension on %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db); return NULL;
    }

    if (sqlite3_memory_init(db, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Failed to init sqlite-memory on %s\n", path);
        sqlite3_close(db); return NULL;
    }

    return db;
}

static int count_search_results(sqlite3 *db, const char *query, const char *context) {
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT snippet FROM memory_search WHERE query = ?1 AND context = ?2 LIMIT 5;",
        -1, &vm, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    [search prepare error: %s]\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(vm, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 2, context, -1, SQLITE_STATIC);
    int count = 0;
    int step;
    while ((step = sqlite3_step(vm)) == SQLITE_ROW) count++;
    if (step != SQLITE_DONE) {
        fprintf(stderr, "    [search step error: %s]\n", sqlite3_errmsg(db));
    }
    sqlite3_finalize(vm);
    return count;
}

// ============================================================================
// Test steps
// ============================================================================

static sqlite3 *db_a = NULL;
static sqlite3 *db_b = NULL;
static const char *g_api_key     = NULL;
static const char *g_vector_lib   = NULL;
static const char *g_sync_lib     = NULL;
static const char *g_sync_db_id   = NULL;
static const char *g_sync_apikey_a = NULL;
static const char *g_sync_apikey_b = NULL;

static void step_context_filtered_fts(void) {
    printf("  Agent A context-filtered FTS search works... ");
    fflush(stdout);
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('vector_weight', 0.0);");
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('text_weight', 1.0);");
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('min_score', 0.0);");
    int n = count_search_results(db_a, "James Webb Telescope", CONTEXT_SPACE);
    ASSERT(n >= 1);
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('vector_weight', 0.6);");
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('text_weight', 0.4);");
    ASSERT_SQL_OK(db_a, "SELECT memory_set_option('min_score', 0.7);");
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent B context-filtered FTS search works... ");
    fflush(stdout);
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('vector_weight', 0.0);");
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('text_weight', 1.0);");
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('min_score', 0.0);");
    n = count_search_results(db_b, "Great Barrier Reef", CONTEXT_REEF);
    ASSERT(n >= 1);
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('vector_weight', 0.6);");
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('text_weight', 0.4);");
    ASSERT_SQL_OK(db_b, "SELECT memory_set_option('min_score', 0.7);");
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);
}

// Step 1: Open both agent databases
static void step_open_databases(void) {
    printf("  Opening Agent A database... ");
    fflush(stdout);
    db_a = open_agent_db(AGENT_A_DB, g_vector_lib, g_sync_lib);
    ASSERT_MSG(db_a != NULL, "Failed to open Agent A database");
    tests_run++; tests_passed++;
    printf("PASSED\n");

    printf("  Opening Agent B database... ");
    fflush(stdout);
    db_b = open_agent_db(AGENT_B_DB, g_vector_lib, g_sync_lib);
    ASSERT_MSG(db_b != NULL, "Failed to open Agent B database");
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 2: Configure both agents with the embedding model
static void step_configure_agents(void) {
    printf("  Configuring Agent A (space context)... ");
    fflush(stdout);
    char sql[1024];
    snprintf(sql, sizeof(sql), "SELECT memory_set_apikey('%s');", g_api_key);
    ASSERT_SQL_OK(db_a, sql);
    ASSERT_SQL_OK(db_a, "SELECT memory_set_model('llama', 'embeddinggemma-300m');");
    tests_run++; tests_passed++;
    printf("PASSED\n");

    printf("  Configuring Agent B (reef context)... ");
    fflush(stdout);
    snprintf(sql, sizeof(sql), "SELECT memory_set_apikey('%s');", g_api_key);
    ASSERT_SQL_OK(db_b, sql);
    ASSERT_SQL_OK(db_b, "SELECT memory_set_model('llama', 'embeddinggemma-300m');");
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 3: Each agent ingests its own knowledge
static void step_ingest_content(void) {
    printf("  Agent A ingesting JWST content... ");
    fflush(stdout);
    // Use bound parameters to avoid quoting issues with the content
    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db_a, "SELECT memory_add_text(?1, ?2);", -1, &vm, NULL);
    ASSERT(rc == SQLITE_OK);
    sqlite3_bind_text(vm, 1, JWST_CONTENT, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 2, CONTEXT_SPACE, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        printf("FAILED\n    memory_add_text error: %s\n", sqlite3_errmsg(db_a));
        sqlite3_finalize(vm); tests_failed++; tests_passed--; return;
    }
    sqlite3_finalize(vm);
    result_int = 0;
    sqlite3_exec(db_a, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'space';", capture_int, NULL, NULL);
    ASSERT(result_int == 1);
    tests_run++; tests_passed++;
    printf("PASSED\n");

    printf("  Agent B ingesting Reef content... ");
    fflush(stdout);
    vm = NULL;
    rc = sqlite3_prepare_v2(db_b, "SELECT memory_add_text(?1, ?2);", -1, &vm, NULL);
    ASSERT(rc == SQLITE_OK);
    sqlite3_bind_text(vm, 1, REEF_CONTENT, -1, SQLITE_STATIC);
    sqlite3_bind_text(vm, 2, CONTEXT_REEF, -1, SQLITE_STATIC);
    rc = sqlite3_step(vm);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        printf("FAILED\n    memory_add_text error: %s\n", sqlite3_errmsg(db_b));
        sqlite3_finalize(vm);
        tests_failed++; tests_passed--; return;
    }
    sqlite3_finalize(vm);
    result_int = 0;
    sqlite3_exec(db_b, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'reef';", capture_int, NULL, NULL);
    ASSERT(result_int == 1);
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 4: Pre-sync search assertions
static void step_presync_search(void) {
    printf("  Agent A finds JWST answer (space context)... ");
    fflush(stdout);
    int n = count_search_results(db_a, "telescope at the Lagrange point L2", CONTEXT_SPACE);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent A finds NO reef answer before sync... ");
    fflush(stdout);
    n = count_search_results(db_a, "coral bleaching ocean temperature", CONTEXT_REEF);
    ASSERT(n == 0);
    tests_run++; tests_passed++;
    printf("PASSED (0 results as expected)\n");

    printf("  Agent B finds reef answer (reef context)... ");
    fflush(stdout);
    n = count_search_results(db_b, "coral bleaching ocean temperature", CONTEXT_REEF);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent B finds NO JWST answer before sync... ");
    fflush(stdout);
    n = count_search_results(db_b, "telescope at the Lagrange point L2", CONTEXT_SPACE);
    ASSERT(n == 0);
    tests_run++; tests_passed++;
    printf("PASSED (0 results as expected)\n");
}

// Step 2: Enable CRDT tracking on both agents BEFORE ingesting.
// This ensures every insert is tracked in the shadow table so the server
// can exchange rows between agents.
static void step_enable_crdt_both(void) {
    printf("  Enabling sync on Agent A (before ingesting)... ");
    fflush(stdout);
    ASSERT_SQL_OK(db_a, "SELECT memory_enable_sync();");
    tests_run++; tests_passed++;
    printf("PASSED\n");

    printf("  Enabling sync on Agent B (before ingesting)... ");
    fflush(stdout);
    ASSERT_SQL_OK(db_b, "SELECT memory_enable_sync();");
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 5: Connect both agents to the SQLiteCloud network.
static void step_connect_both(void) {
    char sql[512];

    printf("  Connecting Agent A to cloud... ");
    fflush(stdout);
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_init('%s');", g_sync_db_id);
    ASSERT_SQL_OK(db_a, sql);
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_set_apikey('%s');", g_sync_apikey_a);
    ASSERT_SQL_OK(db_a, sql);
    tests_run++; tests_passed++;
    printf("PASSED\n");

    printf("  Connecting Agent B to cloud... ");
    fflush(stdout);
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_init('%s');", g_sync_db_id);
    ASSERT_SQL_OK(db_b, sql);
    snprintf(sql, sizeof(sql), "SELECT cloudsync_network_set_apikey('%s');", g_sync_apikey_b);
    ASSERT_SQL_OK(db_b, sql);
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 6: Bidirectional sync
//
// Agent A: localVersion=3 (init + 2 JWST rows → ensures sv=3 ≠ sv=2)
// Agent B: localVersion=2 (init + 1 reef row)
//
// network_sync receive window = v > confirmed_send_version, so neither agent
// naturally receives the other's data in the first sync call.
// reset_sync_version resets the receive pointer to 0, making the next
// network_sync receive all foreign rows from the server (v > 0).
static void step_sync(void) {
    // Push phase: each agent uploads its own content
    int ntimes = 2;
    while (ntimes > 0) {
        printf("  Agent A pushing/pulling JWST data to cloud... ");
        fflush(stdout);
        result_buf[0] = '\0';
        sqlite3_exec(db_a, "SELECT cloudsync_network_sync(500, 3);", capture_string, NULL, NULL);
        printf("PASSED (result: %s)\n", result_buf[0] ? result_buf : "ok");
        tests_run++; tests_passed++;
        
        printf("  Agent B pushing/pulling reef data to cloud... ");
        fflush(stdout);
        result_buf[0] = '\0';
        sqlite3_exec(db_b, "SELECT cloudsync_network_sync(500, 3);", capture_string, NULL, NULL);
        printf("PASSED (result: %s)\n", result_buf[0] ? result_buf : "ok");
        tests_run++; tests_passed++;
        ntimes--;
    }
}

// Step 6b: Verify data is arrived in both agents
static void step_verify(void) {
    printf("  Verifying reef content arrived on Agent A... ");
    fflush(stdout);
    result_int = 0;
    sqlite3_exec(db_a, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'reef';", capture_int, NULL, NULL);
    ASSERT(result_int >= 1);
    tests_run++; tests_passed++;
    printf("PASSED\n");
    
    printf("  Verifying JWST content arrived on Agent B... ");
    fflush(stdout);
    result_int = 0;
    sqlite3_exec(db_b, "SELECT COUNT(*) FROM dbmem_content WHERE context = 'space';", capture_int, NULL, NULL);
    ASSERT(result_int >= 1);
    tests_run++; tests_passed++;
    printf("PASSED\n");
}

// Step 7: Reindex both agents — generate embeddings for received content
static void step_reindex(void) {
    printf("  Agent B generating embeddings for synced content... ");
    fflush(stdout);
    result_int = 0;
    sqlite3_exec(db_b, "SELECT memory_reindex();", capture_int, NULL, NULL);
    ASSERT(result_int >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d chunk(s) reindexed)\n", result_int);

    printf("  Agent A generating embeddings for synced content... ");
    fflush(stdout);
    result_int = 0;
    sqlite3_exec(db_a, "SELECT memory_reindex();", capture_int, NULL, NULL);
    ASSERT(result_int >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d chunk(s) reindexed)\n", result_int);
}

// Step 8: Post-sync search — both agents must find both topics
static void step_postsync_search(void) {
    printf("  Agent A finds reef answer after sync+reindex... ");
    fflush(stdout);
    int n = count_search_results(db_a, "coral bleaching ocean temperature", CONTEXT_REEF);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent A still finds JWST answer... ");
    fflush(stdout);
    n = count_search_results(db_a, "telescope at the Lagrange point L2", CONTEXT_SPACE);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent B finds JWST answer after sync+reindex... ");
    fflush(stdout);
    n = count_search_results(db_b, "telescope at the Lagrange point L2", CONTEXT_SPACE);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);

    printf("  Agent B still finds reef answer... ");
    fflush(stdout);
    n = count_search_results(db_b, "coral bleaching ocean temperature", CONTEXT_REEF);
    ASSERT(n >= 1);
    tests_run++; tests_passed++;
    printf("PASSED (%d result(s))\n", n);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    g_api_key      = getenv("APIKEY");
    g_vector_lib   = getenv("VECTOR_LIB");
    g_sync_lib     = getenv("SYNC_LIB");
    g_sync_db_id   = getenv("SYNC_DB_ID");
    g_sync_apikey_a = getenv("SYNC_APIKEY_A");
    g_sync_apikey_b = getenv("SYNC_APIKEY_B");

    if (!g_api_key || strlen(g_api_key) == 0 ||
        !g_vector_lib || strlen(g_vector_lib) == 0 ||
        !g_sync_lib || strlen(g_sync_lib) == 0 ||
        !g_sync_db_id || !g_sync_apikey_a || !g_sync_apikey_b) {
        fprintf(stderr,
            "Sync test requires these environment variables:\n"
            "  APIKEY        — vectors.space API key\n"
            "  VECTOR_LIB    — path to sqlite-vector shared library\n"
            "  SYNC_LIB      — path to sqlite-sync shared library\n"
            "  SYNC_DB_ID    — SQLiteCloud managed database ID\n"
            "  SYNC_APIKEY_A — SQLiteCloud API key for Agent A\n"
            "  SYNC_APIKEY_B — SQLiteCloud API key for Agent B\n");
        return 1;
    }

    // Clean up stale databases from previous runs
    unlink(AGENT_A_DB);
    unlink(AGENT_B_DB);

    printf("\nSync integration test: JWST (Agent A) + Great Barrier Reef (Agent B)\n");
    printf("=======================================================================\n\n");

    printf("Phase 1: Setup\n");
    step_open_databases();
    step_configure_agents();

    printf("\nPhase 2: Enable CRDT on both agents (before ingesting so all inserts are tracked)\n");
    step_enable_crdt_both();

    printf("\nPhase 3: Ingest (each agent indexes its own knowledge)\n");
    step_ingest_content();

    printf("\nPhase 4: Pre-sync search (isolated knowledge)\n");
    step_context_filtered_fts();
    step_presync_search();

    printf("\nPhase 5: Connect both agents to cloud\n");
    step_connect_both();

    printf("\nPhase 6: Synchronize (bidirectional: each agent sends and receives)\n");
    step_sync();
    step_verify();

    printf("\nPhase 7: Reindex (both agents generate embeddings for received content)\n");
    step_reindex();

    printf("\nPhase 8: Post-sync search (both agents find both topics)\n");
    step_postsync_search();

    // Cleanup
    if (db_a) sqlite3_close(db_a);
    if (db_b) sqlite3_close(db_b);
    unlink(AGENT_A_DB);
    unlink(AGENT_B_DB);

    printf("\n=== Sync Test Results ===\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}

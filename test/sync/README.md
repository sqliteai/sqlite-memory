# Agent Memory Sync — Integration Test

This test demonstrates **bidirectional knowledge sharing between AI agents** using [sqlite-memory](../../README.md) and [SQLite Sync](https://github.com/sqliteai/sqlite-sync).

Two independent agents, each with its own local SQLite database and its own area of expertise, sync their knowledge through a shared cloud database. After sync, both agents can answer questions about topics they never directly indexed.

## What the Test Does

**Agent A** knows about the James Webb Space Telescope (JWST).
**Agent B** knows about the Great Barrier Reef.

The test runs in eight phases:

| Phase | Description |
|-------|-------------|
| 1 | Open separate temporary file-backed databases for Agent A and Agent B |
| 2 | Enable CRDT sync on both agents before ingesting content |
| 3 | Each agent ingests its own knowledge using the remote `vectors.space` embedding service |
| 4 | Pre-sync isolation: each agent can answer its own topic, not the other's, and context-filtered FTS works locally |
| 5 | Connect both agents to the shared SQLiteCloud managed database |
| 6 | Bidirectional sync: each agent pushes its content and pulls the other's |
| 7 | Reindex: each agent generates embeddings for the newly received content |
| 8 | Post-sync: both agents can now answer questions about **both** topics |

Phases 4 and 8 together form the core assertion: knowledge was isolated before sync and shared after sync, with no manual data exchange between agents.

## Why This Matters

AI agents increasingly need to share knowledge without tight coupling. Each agent runs independently, maintains its own local memory, and may be offline or unreachable. The sync layer provides:

- **Offline-first operation** — agents ingest and search locally, sync when connectivity is available
- **No coordination required** — agents push independently; CRDT merge handles conflicts deterministically
- **Selective sharing** — content can be filtered by context so agents share only what is relevant
- **Scalable to many agents** — the same pattern extends to fleets of agents accumulating knowledge in parallel and merging it into a coherent shared corpus

This is the foundation for distributed agent memory: a growing body of knowledge that no single agent owns but all agents contribute to and benefit from.

The sync test uses the remote `vectors.space` embedding API via `provider='llama'` and `model='embeddinggemma-300m'`. That keeps setup lightweight because users do not need to download a local GGUF model just to run the sync scenario.

## Dependencies

### sqlite-vector

Required for vector similarity search. Download from:
[https://github.com/sqliteai/sqlite-vector/releases](https://github.com/sqliteai/sqlite-vector/releases)

Place the shared library (e.g. `sqlite-vector.dylib` / `sqlite-vector.so`) where the Makefile's `VECTOR_LIB` variable points, or set `VECTOR_LIB` at build time.

### SQLite Sync (cloudsync)

Required for CRDT-based synchronization. Download the pre-built binary for your platform from:
[https://github.com/sqliteai/sqlite-sync/releases](https://github.com/sqliteai/sqlite-sync/releases)

Place `cloudsync.dylib` (macOS) or `cloudsync.so` (Linux) in this directory (`test/sync/`).

The extension is loaded at runtime via `SELECT load_extension(...)`. No linking is required.

## SQLite Cloud Setup

The sync layer routes changes through a [SQLiteCloud](https://sqlitecloud.io/) managed database. Setup takes about five minutes:

1. **Create an account** at [https://sqlite.ai/](https://sqlite.ai/) and create a project.
2. **Create a database** in the [dashboard](https://dashboard.sqlitecloud.io/).
3. **Create the memory table** — connect to your database and run:
   ```sql
   CREATE TABLE IF NOT EXISTS dbmem_content (
       hash          TEXT    PRIMARY KEY NOT NULL,
       path          TEXT    NOT NULL DEFAULT '' UNIQUE,
       value         TEXT    DEFAULT NULL,
       length        INTEGER NOT NULL DEFAULT 0,
       context       TEXT    DEFAULT NULL,
       created_at    INTEGER DEFAULT 0,
       last_accessed INTEGER DEFAULT 0
   );
   ```
4. **Enable OffSync** for the database: open the database, click **OffSync**, and enable synchronization. This provisions the CloudSync microservice that routes changes between agents.
5. **Enable OffSync for the table** — initialize sync on `dbmem_content` and configure the `value` column to use block-level LWW so that concurrent agent edits to different lines of the same entry are preserved rather than overwritten. This can be done from the dashboard UI (Database → OffSync section) or via SQL:
   ```sql
   SELECT cloudsync_init('dbmem_content');
   SELECT cloudsync_set_column('dbmem_content', 'value', 'algo', 'block');
   ```
6. **Copy the managed database ID** — shown on the OffSync page (format: `db_xxxxxxxxxxxx`).
7. **Create API keys** — one per agent is recommended, so access can be revoked independently. API keys are created in the dashboard under **API Keys**.

### Credentials used by the test

| Variable | Description |
|----------|-------------|
| `APIKEY` | vectors.space API key used for `memory_set_model('llama', 'embeddinggemma-300m')` |
| `SYNC_DB_ID` | Managed database ID from the OffSync page |
| `SYNC_APIKEY_A` | API key for Agent A |
| `SYNC_APIKEY_B` | API key for Agent B |

The Makefile has defaults baked in for the project's own test database. To use your own SQLiteCloud database and keys:

```sh
make sync-test \
  DEFINES="-DTEST_SQLITE_EXTENSION" \
  APIKEY=your_vectors_space_key \
  SYNC_DB_ID=db_your_managed_db_id \
  SYNC_APIKEY_A=your_agent_a_key \
  SYNC_APIKEY_B=your_agent_b_key
```

## Running the Test

```sh
# From the project root
make sync-test DEFINES="-DTEST_SQLITE_EXTENSION" APIKEY=your_vectors_space_key
```

The test creates two temporary SQLite database files in `/tmp/`, runs the full eight-phase scenario, and cleans up on exit. Expected output:

```
Sync integration test: JWST (Agent A) + Great Barrier Reef (Agent B)
=======================================================================
...
=== Sync Test Results ===
Tests run:    28
Tests passed: 28
Tests failed: 0

Sync test passed!
```

## How Sync Works in the Test

The test uses `cloudsync_network_sync(500, 3)` called twice per agent in sequence. The first call pushes local changes to the cloud; the second call (after the other agent has pushed) pulls remote changes. No manual version resets or delays are needed; the two-round pattern is the standard approach for bidirectional peer exchange described in the [SQLite Sync documentation](https://github.com/sqliteai/sqlite-sync).

Sync is enabled on `dbmem_content`, and the `value` column (which stores the raw text) is configured with the `block` algorithm so that line-level changes from concurrent agents are preserved rather than replaced wholesale.

After receiving content via sync, each agent calls `memory_reindex()` to generate embeddings for the newly arrived rows. Only rows not yet in the local embedding vault are processed, so existing embeddings are never duplicated.

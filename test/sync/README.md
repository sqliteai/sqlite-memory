# Agent Memory Sync — Integration Test

This test demonstrates **bidirectional knowledge sharing between AI agents** using [sqlite-memory](../../README.md) and [SQLite Sync](https://github.com/sqliteai/sqlite-sync).

Two independent agents, each with its own local SQLite database and its own area of expertise, sync their knowledge through a shared cloud database. After sync, both agents can answer questions about topics they never directly indexed.

## What the Test Does

**Agent A** knows about the James Webb Space Telescope (JWST).
**Agent B** knows about the Great Barrier Reef.

The test runs in eight phases:

| Phase | Description |
|-------|-------------|
| 1 | Open separate in-memory databases for Agent A and Agent B |
| 2 | Enable CRDT sync on both agents before ingesting content |
| 3 | Each agent ingests its own knowledge (embeddings computed locally) |
| 4 | Pre-sync isolation: each agent can answer its own topic, not the other's |
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
3. **Enable OffSync** for the database: open the database, click **OffSync**, and enable synchronization. This provisions the CloudSync microservice that routes changes between agents.
4. **Copy the managed database ID** — shown on the OffSync page (format: `db_xxxxxxxxxxxx`).
5. **Create API keys** — one per agent is recommended, so access can be revoked independently. API keys are created in the dashboard under **API Keys**.

### Credentials used by the test

| Variable | Description |
|----------|-------------|
| `SYNC_DB_ID` | Managed database ID from the OffSync page |
| `SYNC_APIKEY_A` | API key for Agent A |
| `SYNC_APIKEY_B` | API key for Agent B |
| `APIKEY` | vectors.space API key for computing embeddings |

The Makefile has defaults baked in for the project's own test database. To use your own:

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

The test creates two temporary SQLite databases in `/tmp/`, runs the full eight-phase scenario, and cleans up on exit. Expected output:

```
Sync integration test: JWST (Agent A) + Great Barrier Reef (Agent B)
=======================================================================
...
=== Sync Test Results ===
Tests run:    26
Tests passed: 26
Tests failed: 0

Sync test passed!
```

## How Sync Works in the Test

The test uses `cloudsync_network_sync(500, 3)` called twice per agent in sequence. The first call pushes local changes to the cloud; the second call (after the other agent has pushed) pulls remote changes. No manual version resets or delays are needed; the two-round pattern is the standard approach for bidirectional peer exchange described in the [SQLite Sync documentation](https://github.com/sqliteai/sqlite-sync).

CRDT sync is enabled on `dbmem_content` with the `cls` algorithm. The `value` column (which stores the raw text) uses the `block` algorithm so that line-level changes from concurrent agents are preserved rather than replaced wholesale.

After receiving content via sync, each agent calls `memory_reindex()` to generate embeddings for the newly arrived rows. Only rows not yet in the local embedding vault are processed, so existing embeddings are never duplicated.

<div align="center">
  <a href="https://sqlite.ai">
    <img src="https://www.sqlite.ai/social/logo-ai.png" alt="SQLite AI" height="56">
  </a>

  <h1>SQLite-Memory</h1>
  <p><strong>Persistent, searchable memory for AI agents.</strong><br>
  Markdown-based memory with semantic search, hybrid retrieval, and offline-first sync between agents. Drop-in memory layer for any LLM workflow.</p>

  <p>
    <a href="https://dashboard.sqlitecloud.io/auth/sign-in"><strong>Free managed instance →</strong></a> ·
    <a href="https://docs.sqlitecloud.io/docs/ai-overview">Docs</a> ·
    <a href="https://sqlite.ai">Website</a> ·
    <a href="https://blog.sqlite.ai">Blog</a>
  </p>

  <p>
    <sub><strong>Data:</strong>
    <a href="https://github.com/sqliteai/sqlite-vector">Vector</a> ·
    <a href="https://github.com/sqliteai/sqlite-sync">Sync</a> ·
    <a href="https://github.com/sqliteai/sqlite-columnar">Columnar</a> ·
    <a href="https://github.com/sqliteai/sqlite-js">JS</a>
    <br>
    <strong>AI:</strong>
    <a href="https://github.com/sqliteai/sqlite-ai">AI</a> ·
    <a href="https://github.com/sqliteai/sqlite-agent">Agent</a> ·
    <a href="https://github.com/sqliteai/sqlite-memory">Memory</a> ·
    <a href="https://github.com/sqliteai/sqlite-mcp">MCP</a>
    </sub>
  </p>
</div>

<br>

> **Multiple agents need shared memory?** SQLite-Memory syncs locally via CRDTs; pair it with **[SQLite Cloud](https://dashboard.sqlitecloud.io/auth/sign-in)** (or your own Postgres/Supabase) to coordinate memory across machines, users, and workers. Free tier available.

---

# SQLite Memory

A SQLite extension that gives AI agents persistent, searchable memory, optimized for markdown content. Features hybrid semantic search (vector similarity + FTS5), markdown-aware chunking, and local embedding via llama.cpp.

Agent memory databases can be synchronized between agents using **offline-first technology** via [sqlite-sync](https://github.com/sqliteai/sqlite-sync). Each agent works independently and syncs when connected, making it ideal for distributed AI systems, edge deployments, and collaborative agent architectures.

## The Future of AI Agent Memory

Modern AI agents need persistent, searchable memory to maintain context across conversations and tasks. Inspired by [OpenClaw's memory architecture](https://docs.openclaw.ai/concepts/memory), sqlite-memory implements what we believe will become the de facto standard for AI agent memory systems: **markdown files as the source of truth**.

In this paradigm:
- **Markdown files** serve as human-readable, version-controllable knowledge bases
- **Embeddings** enable semantic understanding and retrieval
- **Hybrid search** combines the precision of full-text search with the intelligence of vector similarity

sqlite-memory bridges these concepts, allowing any SQLite-powered application to ingest, store, and semantically search over knowledge bases.

## Why sqlite-memory?

### For AI Agent Developers

- **Persistent Memory**: Give your agents long-term memory that survives restarts
- **Semantic Recall**: Retrieve relevant context based on meaning, not just keywords
- **Context Isolation**: Organize memories by context (projects, conversations, topics)
- **Local-First**: Run entirely on-device with local embedding models - no API costs, no latency, no data leaving your system

### For Application Developers

- **Zero Infrastructure**: No vector database servers to deploy - it's just SQLite
- **Single File**: Your entire knowledge base lives in one portable `.db` file
- **SQL Interface**: Query your semantic memory using familiar SQL
- **Embeddable**: Works anywhere SQLite works - mobile, desktop, edge, WASM

### Technical Advantages

- **Hybrid Search**: Combines vector similarity (cosine distance) with FTS5 full-text search for superior retrieval
- **Smart Chunking**: Markdown-aware parsing preserves semantic boundaries
- **Intelligent Sync**: Content-hash change detection skips unchanged files, atomically replaces modified ones, and cleans up deleted ones
- **Transactional Safety**: Text/file ingests run inside SAVEPOINT transactions, and directory sync uses transactional cleanup plus per-file transactional updates so failed files do not leave partial rows behind
- **Efficient Storage**: Binary embeddings with configurable dimensions
- **Embedding Cache**: Automatically caches computed embeddings, so re-indexing the same text skips redundant API calls and computation
- **Flexible Embedding**: Use local models (llama.cpp) or [vectors.space](https://vectors.space) remote API

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Your Application                        │
├─────────────────────────────────────────────────────────────┤
│                      sqlite-memory                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │   Parser    │  │  Embedding  │  │   Hybrid Search     │  │
│  │  (md4c)     │  │ (llama.cpp) │  │ (vector + FTS5)     │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                   sqlite-vector                             │
├─────────────────────────────────────────────────────────────┤
│                      SQLite                                 │
└─────────────────────────────────────────────────────────────┘
```

## Getting Started

> [!IMPORTANT]
> Databases created with sqlite-memory versions earlier than `1.0.0` must be rebuilt before use with `1.0.0+`, because the internal schema changed.

### Prerequisites

- SQLite
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) extension
- [sqlite-sync](https://github.com/sqliteai/sqlite-sync) extension (optional, only needed for agent sync)
- **For local embeddings**: A GGUF embedding model (e.g., [nomic-embed-text](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF))
- **For remote embeddings**: A free API key from [vectors.space](https://vectors.space)

### Quick Start

```sql
-- Load extensions (sync is optional)
.load ./vector
.load ./cloudsync
.load ./memory

-- Configure embedding model (choose one):

-- Option 1: Local embedding with llama.cpp (no internet required)
SELECT memory_set_model('local', '/path/to/nomic-embed-text-v1.5.Q8_0.gguf');

-- Option 2: Remote embedding via vectors.space (requires free API key from https://vectors.space)
-- The provider name 'openai' selects the vectors.space OpenAI-compatible endpoint.
-- SELECT memory_set_apikey('your-vectorspace-api-key');
-- SELECT memory_set_model('openai', 'text-embedding-3-small');

-- Add some knowledge
SELECT memory_add_text('SQLite is a C-language library that implements a small, fast,
self-contained, high-reliability, full-featured, SQL database engine. SQLite is the
most used database engine in the world.', 'sqlite-docs');

SELECT memory_add_text('Vector databases store data as high-dimensional vectors,
enabling similarity search. They are essential for semantic search, recommendation
systems, and AI applications.', 'concepts');

-- Add an entire documentation directory
SELECT memory_add_directory('/path/to/docs', 'project-docs');
-- Paths are stored relative to /path/to/docs, so the database can be materialized elsewhere.

-- Search your memory semantically
SELECT path, snippet, ranking
FROM memory_search
WHERE query = 'how do databases store information efficiently';

-- Results ranked by semantic similarity + keyword matching
-- ┌──────────────┬─────────────────────────────────────┬─────────┐
-- │     path     │               snippet               │ ranking │
-- ├──────────────┼─────────────────────────────────────┼─────────┤
-- │ (uuid)       │ SQLite is a C-language library...   │ 0.89    │
-- │ (uuid)       │ Vector databases store data as...   │ 0.82    │
-- └──────────────┴─────────────────────────────────────┴─────────┘
```

### Command Line: sqlmem

[`sqlmem`](cli/README.md) is the Go CLI for managing SQLite Memory projects from the terminal. It creates `.sqlmem.json`, manages the SQLite database, downloads and loads the SQLite extensions, configures embedding models, indexes Markdown sources, runs hybrid searches, watches files for changes, and exposes the memory tools over MCP.

Use it when you want a project-level workflow around sqlite-memory without writing SQL directly:

```bash
cd cli
make build
./sqlmem init --model /path/to/embedding-model.gguf
./sqlmem add ../docs
./sqlmem search -q "how do I configure memory?"
```

See the [`sqlmem` README](cli/README.md) for installation, configuration, extension cache paths, PDF support, MCP, and command examples.

### Example: Building an AI Agent with Memory

```python
import sqlite3

# Connect to your memory database
conn = sqlite3.connect('agent_memory.db')
conn.enable_load_extension(True)
conn.load_extension('./vector')
conn.load_extension('./memory')

# One-time setup
conn.execute("SELECT memory_set_model('local', './models/nomic-embed-text-v1.5.Q8_0.gguf')")

# Store conversation context
def remember(content, context="conversation"):
    conn.execute("SELECT memory_add_text(?, ?)", (content, context))
    conn.commit()

# Retrieve relevant memories
def recall(query, min_score=0.7):
    cursor = conn.execute("""
        SELECT snippet, ranking FROM memory_search
        WHERE query = ? AND ranking > ?
        ORDER BY ranking DESC
    """, (query, min_score))
    return cursor.fetchall()

# Use in your agent
remember("User prefers concise responses and uses Python primarily.")
remember("Project deadline is March 15th, focusing on API integration.")

# Later, when the user asks about the project...
memories = recall("what's the project timeline")
# Returns relevant context about March 15th deadline
```

## Intelligent Sync

By default, all `memory_add_*` functions use content-hash change detection to avoid redundant work:

- **`memory_add_text`**: Computes a hash of the content. If the same content was already indexed, it is skipped entirely. No duplicate embeddings are ever created.
- **`memory_add_file`**: Reads the file and hashes its content. If the file was previously indexed with different content, the old entry (chunks, embeddings, FTS) is atomically replaced. Unchanged files are skipped. Absolute file paths are stored as portable logical suffixes, while the original local path is retained only in local metadata.
- **`memory_add_content`**: Indexes caller-provided file content without reading from the filesystem, preserving the supplied logical file name/path and optional context.
- **`memory_add_directory`**: Performs a full two-phase sync:
  1. **Cleanup**: Removes database entries for files that no longer exist on disk
  2. **Scan**: Recursively processes all matching files - adding new ones, replacing modified ones, and skipping unchanged ones. Stored paths are relative to the scanned directory root, with local provenance retained only in local metadata.

For virtual-file or editor workflows that need separate logical paths even when content is identical or empty, enable path-preserving storage:

```sql
SELECT memory_set_option('preserve_duplicate_paths', 1);
```

In this mode, `dbmem_content.hash` identifies the stored entry and is scoped by path.

`memory_add_text()`, `memory_add_file()`, and `memory_add_content()` each run inside a SQLite SAVEPOINT transaction. `memory_add_directory()` performs its cleanup pass transactionally and then processes each file in its own transaction. If one file fails, that file rolls back cleanly and previously-committed files remain valid; there are no partially-indexed rows or orphaned chunk/FTS entries for the failed file.

This makes all sync functions safe to call repeatedly - for example, on a cron schedule or at agent startup - with minimal overhead.

## Agent Memory Sync

Multiple agents can share and merge knowledge without any coordination. Each agent works independently with its own local SQLite database, syncing through a shared [SQLiteCloud](https://sqlitecloud.io/) managed database when connectivity is available.

Enable sync on a database connection before ingesting content:

```sql
-- Load the sqlite-sync extension
SELECT load_extension('./cloudsync');

-- Enable CRDT sync (optionally scoped to a specific context)
SELECT memory_enable_sync();               -- sync all memory
SELECT memory_enable_sync('project-x');   -- sync only the 'project-x' context

-- Connect to the shared cloud database
SELECT cloudsync_network_init('your-managed-database-id');
SELECT cloudsync_network_set_apikey('your-api-key');

-- Ingest content normally — CRDT tracks every write
SELECT memory_add_text('Agent A findings...', 'research');

-- Push local changes and pull remote ones (call twice for full bidirectional exchange)
SELECT cloudsync_network_sync(500, 3);
SELECT cloudsync_network_sync(500, 3);

-- Refresh hashes and embeddings for any content received or merged from other agents
SELECT memory_reindex();
```

Each piece of text added to the database is parsed into chunks and tracked by a [block-level LWW CRDT algorithm](https://github.com/sqliteai/sqlite-sync?tab=readme-ov-file#block-level-lww), which merges line-level changes from concurrent agents without conflicts. Only the portable `dbmem_content` table is synced — embeddings and local filesystem provenance are always local. After a sync merge changes `dbmem_content.value`, `memory_reindex()` recomputes stale content hashes and refreshes local embeddings.

### Why This Matters for AI Systems

The combination of local-first memory and CRDT sync enables agent architectures that are not possible with centralized databases:

- **No single point of failure** — each agent has a complete, queryable copy of shared memory
- **Offline-capable** — agents ingest and search without network access; sync catches up when connectivity returns
- **Selective sharing** — `memory_enable_sync('context')` limits sync to a named context, so agents can keep private memory separate from shared memory
- **Scales to many agents** — agents running on different nodes accumulate knowledge in parallel and merge into a single consistent corpus without coordination

### Working Example

[`test/sync/`](test/sync/) contains a full integration test that walks through the entire flow:

- Agent A indexes knowledge about the James Webb Space Telescope
- Agent B indexes knowledge about the Great Barrier Reef
- After sync, **both agents can answer questions about both topics** — knowledge each agent never directly indexed

See [`test/sync/README.md`](test/sync/README.md) for setup instructions, SQLiteCloud account configuration, and how to run the test.

## Use Cases

- **AI Assistants**: Maintain conversation history and user preferences
- **Documentation Search**: Semantic search over markdown documentation
- **Knowledge Bases**: Build searchable knowledge repositories
- **Note-Taking Apps**: Find notes by meaning, not just keywords
- **Code Understanding**: Index and search code documentation
- **Personal Memory**: Store and retrieve personal knowledge

## Configuration

Tune the memory system for your needs:

```sql
-- Chunking parameters
SELECT memory_set_option('max_tokens', 512);      -- Tokens per chunk
SELECT memory_set_option('overlay_tokens', 100);  -- Overlap between chunks

-- Search behavior
SELECT memory_set_option('max_results', 30);      -- Max search results
SELECT memory_set_option('min_score', 0.75);      -- Score threshold
SELECT memory_set_option('vector_weight', 0.6);   -- Vector vs FTS balance
SELECT memory_set_option('text_weight', 0.4);
SELECT memory_set_option('search_oversample', 4); -- Fetch 4x candidates before merging

-- File processing
SELECT memory_set_option('extensions', 'md,txt,rst');  -- File types to index
SELECT memory_set_option('preserve_duplicate_paths', 1); -- Keep duplicate/empty virtual paths

-- Embedding cache (enabled by default)
SELECT memory_set_option('embedding_cache', 0);        -- Disable cache
SELECT memory_set_option('cache_max_entries', 10000);  -- Limit cache size (0 = no limit)
SELECT memory_cache_clear();                           -- Clear cached embeddings
```

## Memory Management

```sql
-- View all memories
SELECT hash, path, context, datetime(created_at, 'unixepoch', 'localtime') as created
FROM dbmem_content;

-- Delete by context
SELECT memory_delete_context('old-project');

-- Delete specific memory by hash
SELECT memory_delete('9e3779b97f4a7c15');

-- Clear all memories
SELECT memory_clear();
```

## Documentation

For complete API documentation, including all functions and configuration options, see **[API.md](API.md)**.

## Building

```bash
# Clone with submodules
git clone --recursive https://github.com/sqliteai/sqlite-memory.git
cd sqlite-memory

# Build (full build with local + remote engines)
make

# Run parser/core unit tests + extension loading smoke test
make test

# Run the full SQL extension unit suite
make test DEFINES="-DTEST_SQLITE_EXTENSION"
```

### Build Configurations

| Command | Local Engine | Remote Engine | File I/O |
|---------|:------------:|:-------------:|:--------:|
| `make` | ✓ | ✓ | ✓ |
| `make local` | ✓ | ✗ | ✓ |
| `make remote` | ✗ | ✓ | ✓ |
| `make wasm` | ✗ | ✓ | ✗ |

- **Local Engine**: Built-in llama.cpp for on-device embeddings (requires GGUF model)
- **Remote Engine**: [vectors.space](https://vectors.space) API for cloud embeddings (requires free API key)
- **File I/O**: `memory_add_file`, `memory_add_directory`, and `memory_materialize_files` functions

You can also combine options manually:

```bash
# Custom build with specific options
make OMIT_LOCAL_ENGINE=1 OMIT_REMOTE_ENGINE=0 OMIT_IO=0
```

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

## ☁️ Hosted version

Need to share agent memory across devices, users, or workers? **[SQLite Cloud](https://sqlite.ai)** is the managed backend for SQLite-Memory — sync memory across a fleet of agents with auth, ACL, and observability.

[**Start free →**](https://dashboard.sqlitecloud.io/auth/sign-in)

---

## Part of the SQLite AI stack

SQLite-Memory is one piece of a larger ecosystem that turns SQLite into a runtime for intelligent, distributed data:

**Data layer**
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) — ANN vector search inside SQLite
- [sqlite-sync](https://github.com/sqliteai/sqlite-sync) — Offline-first CRDT sync across devices
- [sqlite-columnar](https://github.com/sqliteai/sqlite-columnar) — Column-oriented analytics for OLAP queries
- [sqlite-js](https://github.com/sqliteai/sqlite-js) — Custom SQLite functions written in JavaScript

**AI layer**
- [sqlite-ai](https://github.com/sqliteai/sqlite-ai) — On-device LLM inference and embeddings
- [sqlite-agent](https://github.com/sqliteai/sqlite-agent) — Autonomous AI agents running inside SQLite
- [**sqlite-memory**](https://github.com/sqliteai/sqlite-memory) — Persistent, searchable memory for agents *(you are here)*
- [sqlite-mcp](https://github.com/sqliteai/sqlite-mcp) — Call MCP tools directly from SQL queries

**Managed platform**
- [SQLite Cloud](https://sqlite.ai) — Hosted SQLite with sync, auth, edge functions, and analytics. [Free tier →](https://dashboard.sqlitecloud.io/auth/sign-in)

Built by [SQLite AI](https://sqlite.ai). Questions? [Open a discussion](https://github.com/sqliteai/sqlite-memory/discussions) or [contact us](https://sqlite.ai/support).

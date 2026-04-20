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

All `memory_add_*` functions use content-hash change detection to avoid redundant work:

- **`memory_add_text`**: Computes a hash of the content. If the same content was already indexed, it is skipped entirely. No duplicate embeddings are ever created.
- **`memory_add_file`**: Reads the file and hashes its content. If the file was previously indexed with different content, the old entry (chunks, embeddings, FTS) is atomically replaced. Unchanged files are skipped.
- **`memory_add_directory`**: Performs a full two-phase sync:
  1. **Cleanup**: Removes database entries for files that no longer exist on disk
  2. **Scan**: Recursively processes all matching files - adding new ones, replacing modified ones, and skipping unchanged ones

`memory_add_text()` and `memory_add_file()` each run inside a SQLite SAVEPOINT transaction. `memory_add_directory()` performs its cleanup pass transactionally and then processes each file in its own transaction. If one file fails, that file rolls back cleanly and previously-committed files remain valid; there are no partially-indexed rows or orphaned chunk/FTS entries for the failed file.

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

-- Generate embeddings for any content received from other agents
SELECT memory_reindex();
```

Each piece of text added to the database is parsed into chunks and tracked by a [block-level LWW CRDT algorithm](https://github.com/sqliteai/sqlite-sync?tab=readme-ov-file#block-level-lww), which merges line-level changes from concurrent agents without conflicts. Only the `dbmem_content` table is synced — embeddings are always generated locally after receiving new content.

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
- **File I/O**: `memory_add_file` and `memory_add_directory` functions

You can also combine options manually:

```bash
# Custom build with specific options
make OMIT_LOCAL_ENGINE=1 OMIT_REMOTE_ENGINE=0 OMIT_IO=0
```

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

## Part of the SQLite AI Ecosystem

This project is part of the **SQLite AI** ecosystem, a collection of extensions that bring modern AI capabilities to the world's most widely deployed database. The goal is to make SQLite the default data and inference engine for Edge AI applications.

Other projects in the ecosystem include:

- **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** - On-device inference and embedding generation directly inside SQLite.
- **[SQLite-Memory](https://github.com/sqliteai/sqlite-memory)** - Markdown-based AI agent memory with semantic search.
- **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** - Ultra-efficient vector search for embeddings stored as BLOBs in standard SQLite tables.
- **[SQLite-Sync](https://github.com/sqliteai/sqlite-sync)** - Local-first CRDT-based synchronization for seamless, conflict-free data sync and real-time collaboration across devices.
- **[SQLite-Agent](https://github.com/sqliteai/sqlite-agent)** - Run autonomous AI agents directly from within SQLite databases.
- **[SQLite-MCP](https://github.com/sqliteai/sqlite-mcp)** - Connect SQLite databases to MCP servers and invoke their tools.
- **[SQLite-JS](https://github.com/sqliteai/sqlite-js)** - Create custom SQLite functions using JavaScript.
- **[Liteparser](https://github.com/sqliteai/liteparser)** - A highly efficient and fully compliant SQLite SQL parser.

Learn more at **[SQLite AI](https://sqlite.ai)**.

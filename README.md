# SQLite Memory

A SQLite extension that gives AI agents persistent, searchable memory. Features hybrid semantic search (vector similarity + FTS5), markdown-aware chunking, and local embedding via llama.cpp. Memory databases can be synced between agents using **offline first technology** each agent works independently and syncs when connected, making it ideal for distributed AI systems, edge deployments, and collaborative agent architectures.

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
- **Intelligent Sync**: Content-hash change detection — unchanged files are skipped, modified files are atomically replaced, deleted files are cleaned up
- **Transactional Safety**: Every sync operation runs inside a SAVEPOINT transaction — either fully succeeds or fully rolls back, no partially-indexed content
- **Efficient Storage**: Binary embeddings with configurable dimensions
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
- **For local embeddings**: A GGUF embedding model (e.g., [nomic-embed-text](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF))
- **For remote embeddings**: A free API key from [vectors.space](https://vectors.space)

### Quick Start

```sql
-- Load extensions
.load ./vector
.load ./memory

-- Configure embedding model (choose one):

-- Option 1: Local embedding with llama.cpp (no internet required)
SELECT memory_set_model('local', '/path/to/nomic-embed-text-v1.5.Q8_0.gguf');

-- Option 2: Remote embedding via vectors.space (requires free API key from https://vectors.space)
-- SELECT memory_set_model('openai', 'text-embedding-3-small');
-- SELECT memory_set_apikey('your-vectorspace-api-key');

-- Add some knowledge
SELECT memory_sync_text('SQLite is a C-language library that implements a small, fast,
self-contained, high-reliability, full-featured, SQL database engine. SQLite is the
most used database engine in the world.', 'sqlite-docs');

SELECT memory_sync_text('Vector databases store data as high-dimensional vectors,
enabling similarity search. They are essential for semantic search, recommendation
systems, and AI applications.', 'concepts');

-- Sync an entire documentation directory
SELECT memory_sync_directory('/path/to/docs', 'project-docs');

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
    conn.execute("SELECT memory_sync_text(?, ?)", (content, context))
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

All `memory_sync_*` functions use content-hash change detection to avoid redundant work:

- **`memory_sync_text`** — Computes a hash of the content. If the same content was already indexed, it is skipped entirely. No duplicate embeddings are ever created.
- **`memory_sync_file`** — Reads the file and hashes its content. If the file was previously indexed with different content, the old entry (chunks, embeddings, FTS) is atomically replaced. Unchanged files are skipped.
- **`memory_sync_directory`** — Performs a full two-phase sync:
  1. **Cleanup**: Removes database entries for files that no longer exist on disk
  2. **Scan**: Recursively processes all matching files — adding new ones, replacing modified ones, and skipping unchanged ones

Every sync operation is wrapped in a SQLite SAVEPOINT transaction. If anything fails mid-sync (embedding error, disk issue, etc.), the entire operation rolls back cleanly. There is no risk of partially-indexed files or orphaned entries.

This makes all sync functions safe to call repeatedly — for example, on a cron schedule or at agent startup — with minimal overhead.

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

-- File processing
SELECT memory_set_option('extensions', 'md,txt,rst');  -- File types to index
```

## Memory Management

```sql
-- View all memories
SELECT hash, path, context,
       datetime(created_at, 'unixepoch', 'localtime') as created
FROM dbmem_content;

-- Delete by context
SELECT memory_delete_context('old-project');

-- Delete specific memory
SELECT memory_delete(1234567890);

-- Clear all memories
SELECT memory_clear();
```

## Documentation

For complete API documentation, including all functions and configuration options, see **[API.md](API.md)**.

## Building

```bash
# Clone with submodules
git clone --recursive https://github.com/user/sqlite-memory.git
cd sqlite-memory

# Build (full build with local + remote engines)
make

# Run tests
make test
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
- **File I/O**: `memory_sync_file` and `memory_sync_directory` functions

You can also combine options manually:

```bash
# Custom build with specific options
make OMIT_LOCAL_ENGINE=1 OMIT_REMOTE_ENGINE=0 OMIT_IO=0
```

## License

MIT License - see [LICENSE](LICENSE) for details.

---

*sqlite-memory is part of the [SQLite AI](https://sqlite.ai) project, bringing AI capabilities to the world's most deployed database.*


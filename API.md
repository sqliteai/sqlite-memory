# SQLite Memory Extension – API Reference

A SQLite extension that provides semantic memory capabilities with hybrid search (vector similarity + full-text search).

## Table of Contents

- [Overview](#overview)
- [Loading the Extension](#loading-the-extension)
- [SQL Functions](#sql-functions)
  - [General Functions](#general-functions)
  - [Configuration Functions](#configuration-functions)
  - [Memory Management Functions](#memory-management-functions)
  - [Deletion Functions](#deletion-functions)
- [Virtual Table Module](#virtual-table-module)
- [Configuration Options](#configuration-options)
- [Timestamps](#timestamps)
- [Examples](#examples)

---

## Overview

sqlite-memory enables semantic search over text content stored in SQLite. It:

1. **Chunks** text content using semantic parsing (markdown-aware)
2. **Generates embeddings** for each chunk using the built-in llama.cpp engine (`"local"` provider) or the [vector.space](https://vector.space) remote service
3. **Stores** embeddings and full-text content for hybrid search
4. **Searches** using vector similarity combined with FTS5 full-text search

---

## Loading the Extension

### Dynamic Loading (Recommended)

```sql
.load ./memory
```

### With sqlite-vector (Required for Search)

The extension requires [sqlite-vector](https://github.com/sqliteai/sqlite-vector) for vector similarity search:

```sql
.load ./vector
.load ./memory
```

---

## SQL Functions

### General Functions

#### `memory_version()`

Returns the extension version string.

**Parameters:** None

**Returns:** TEXT - Version string (e.g., "0.5.0")

**Example:**
```sql
SELECT memory_version();
-- Returns: "0.5.0"
```

---

### Configuration Functions

#### `memory_set_model(provider TEXT, model TEXT)`

Configures the embedding model to use.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | TEXT | `"local"` for built-in llama.cpp engine, or any other name (e.g., `"openai"`) for [vector.space](https://vector.space) remote service |
| `model` | TEXT | For local: full path to GGUF model file. For remote: model identifier supported by vector.space |

**Returns:** INTEGER - 1 on success

**Notes:**
- When `provider` is `"local"`, the extension uses the built-in llama.cpp engine and verifies the model file exists
- When `provider` is anything other than `"local"`, the extension uses the [vector.space](https://vector.space) remote embedding service
- Remote embedding requires a free API key from [vector.space](https://vector.space) (set via `memory_set_apikey`)
- Settings are persisted in `dbmem_settings` table
- For local models, the embedding engine is initialized immediately

**Example:**
```sql
-- Local embedding model (uses built-in llama.cpp engine)
SELECT memory_set_model('local', '/path/to/nomic-embed-text-v1.5.Q8_0.gguf');

-- Remote embedding via vector.space (requires free API key)
SELECT memory_set_model('openai', 'text-embedding-3-small');
SELECT memory_set_apikey('your-vectorspace-api-key');
```

---

#### `memory_set_apikey(key TEXT)`

Sets the API key for the [vector.space](https://vector.space) remote embedding service.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `key` | TEXT | API key obtained from [vector.space](https://vector.space) (free account) |

**Returns:** INTEGER - 1 on success

**Notes:**
- API key is stored in memory only, not persisted to disk
- Required when using any provider other than `"local"`
- Get a free API key by creating an account at [vector.space](https://vector.space)

**Example:**
```sql
SELECT memory_set_apikey('your-vectorspace-api-key');
```

---

#### `memory_set_option(key TEXT, value ANY)`

Sets a configuration option.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `key` | TEXT | Option name (see [Configuration Options](#configuration-options)) |
| `value` | ANY | Option value (type depends on the option) |

**Returns:** INTEGER - 1 on success

**Example:**
```sql
-- Set maximum tokens per chunk
SELECT memory_set_option('max_tokens', 512);

-- Enable engine warmup
SELECT memory_set_option('engine_warmup', 1);

-- Set minimum score threshold
SELECT memory_set_option('min_score', 0.75);
```

---

#### `memory_get_option(key TEXT)`

Retrieves a configuration option value.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `key` | TEXT | Option name |

**Returns:** ANY - Option value, or NULL if not set

**Example:**
```sql
SELECT memory_get_option('max_tokens');
-- Returns: 400

SELECT memory_get_option('provider');
-- Returns: "local"
```

---

### Memory Management Functions

#### `memory_add_text(content TEXT [, context TEXT])`

Adds text content to memory.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `content` | TEXT | Yes | Text content to store and index |
| `context` | TEXT | No | Optional context label for grouping memories |

**Returns:** INTEGER - 1 on success

**Notes:**
- Content is chunked based on `max_tokens` and `overlay_tokens` settings
- Each chunk is embedded and stored in `dbmem_vault`
- Content hash prevents duplicate storage
- Sets `created_at` timestamp automatically

**Example:**
```sql
-- Add text without context
SELECT memory_add_text('SQLite is a C-language library that implements a small, fast, self-contained SQL database engine.');

-- Add text with context
SELECT memory_add_text('Important meeting notes from 2024-01-15...', 'meetings');
```

---

#### `memory_add_file(path TEXT [, context TEXT])`

Adds a file to memory.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | TEXT | Yes | Full path to the file |
| `context` | TEXT | No | Optional context label for grouping memories |

**Returns:** INTEGER - 1 on success

**Notes:**
- Only processes files matching configured extensions (default: `md,mdx`)
- File path is stored in `dbmem_content.path`
- Not available when compiled with `DBMEM_OMIT_IO`

**Example:**
```sql
SELECT memory_add_file('/docs/readme.md');
SELECT memory_add_file('/docs/api.md', 'documentation');
```

---

#### `memory_add_directory(path TEXT [, context TEXT])`

Recursively adds all matching files from a directory.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | TEXT | Yes | Full path to the directory |
| `context` | TEXT | No | Optional context label applied to all files |

**Returns:** INTEGER - Number of files processed

**Notes:**
- Recursively scans subdirectories
- Only processes files matching configured extensions
- Not available when compiled with `DBMEM_OMIT_IO`

**Example:**
```sql
SELECT memory_add_directory('/path/to/docs');
-- Returns: 42 (number of files added)

SELECT memory_add_directory('/project/notes', 'project-notes');
```

---

### Deletion Functions

#### `memory_delete(hash INTEGER)`

Deletes a specific memory by its hash.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `hash` | INTEGER | The hash identifier of the memory to delete |

**Returns:** INTEGER - Number of content entries deleted (0 or 1)

**Notes:**
- Atomically deletes from `dbmem_content`, `dbmem_vault`, and `dbmem_vault_fts`
- Uses SAVEPOINT transaction for atomicity
- Hash can be obtained from `dbmem_content` table or search results

**Example:**
```sql
-- Get hash from content table
SELECT hash FROM dbmem_content WHERE path LIKE '%readme%';

-- Delete by hash
SELECT memory_delete(1234567890);
```

---

#### `memory_delete_context(context TEXT)`

Deletes all memories with a specific context.

**Parameters:**
| Parameter | Type | Description |
|-----------|------|-------------|
| `context` | TEXT | The context label to match |

**Returns:** INTEGER - Number of content entries deleted

**Notes:**
- Deletes all entries where `context` matches exactly
- Cascades to chunks and FTS entries

**Example:**
```sql
-- Delete all memories with context 'meetings'
SELECT memory_delete_context('meetings');
-- Returns: 15
```

---

#### `memory_clear()`

Deletes all memories from the database.

**Parameters:** None

**Returns:** INTEGER - 1 on success

**Notes:**
- Clears `dbmem_content`, `dbmem_vault`, and `dbmem_vault_fts`
- Does not delete settings from `dbmem_settings`
- Uses SAVEPOINT transaction for atomicity

**Example:**
```sql
SELECT memory_clear();
```

---

### `memory_search`

A virtual table for performing hybrid semantic search.

**Query Format:**
```sql
SELECT * FROM memory_search WHERE query = 'search text';
```

**Columns:**
| Column | Type | Description |
|--------|------|-------------|
| `query` | TEXT (HIDDEN) | Search query (required in WHERE clause) |
| `hash` | INTEGER | Content hash identifier |
| `path` | TEXT | Source path or generated UUID |
| `context` | TEXT | Context label (NULL if not set) |
| `snippet` | TEXT | Text snippet from matching chunk |
| `score` | REAL | Combined similarity score (0.0 - 1.0) |

**Notes:**
- Requires sqlite-vector extension loaded first
- Performs hybrid search combining vector similarity and FTS5
- Results are ranked by combined score
- Limited by `max_items` setting (default: 20)
- Filtered by `min_score` setting (default: 0.7)
- Updates `last_accessed` timestamp if `update_access` is enabled

**Example:**
```sql
-- Basic search
SELECT * FROM memory_search WHERE query = 'database indexing strategies';

-- Search with score filter
SELECT path, snippet, score
FROM memory_search
WHERE query = 'how to optimize queries'
AND score > 0.8;

-- Search within a specific context
SELECT * FROM memory_search
WHERE query = 'meeting action items'
AND context = 'meetings';
```

---

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `provider` | TEXT | - | Embedding provider (`"local"` for llama.cpp, otherwise vector.space) |
| `model` | TEXT | - | Model path (local) or identifier (remote) |
| `dimension` | INTEGER | - | Embedding dimension (auto-detected) |
| `max_tokens` | INTEGER | 400 | Maximum tokens per chunk |
| `overlay_tokens` | INTEGER | 80 | Token overlap between consecutive chunks |
| `chars_per_tokens` | INTEGER | 4 | Estimated characters per token |
| `save_content` | INTEGER | 1 | Store original content (1=yes, 0=no) |
| `skip_semantic` | INTEGER | 0 | Skip markdown parsing, treat as raw text |
| `skip_html` | INTEGER | 1 | Strip HTML tags when parsing |
| `extensions` | TEXT | "md,mdx" | Comma-separated file extensions to process |
| `engine_warmup` | INTEGER | 0 | Warm up engine on model load (compiles GPU shaders) |
| `max_items` | INTEGER | 20 | Maximum search results |
| `fts_enabled` | INTEGER | 1 | Enable FTS5 in hybrid search |
| `vector_weight` | REAL | 0.5 | Weight for vector similarity in scoring |
| `text_weight` | REAL | 0.5 | Weight for FTS in scoring |
| `min_score` | REAL | 0.7 | Minimum score threshold for results |
| `update_access` | INTEGER | 1 | Update last_accessed on search |

---

## Timestamps

The extension tracks two timestamps for each memory:

### `created_at`

- Set automatically when content is added via `memory_add_text`, `memory_add_file`, or `memory_add_directory`
- Stored as Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
- Never updated after initial creation

### `last_accessed`

- Updated when content appears in search results (if `update_access=1`)
- Stored as Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
- Can be disabled by setting `update_access` to 0

**Displaying timestamps in local time:**
```sql
SELECT
    path,
    datetime(created_at, 'unixepoch', 'localtime') as created,
    datetime(last_accessed, 'unixepoch', 'localtime') as accessed
FROM dbmem_content;
```

---

## Examples

### Complete Setup and Usage

```sql
-- Load extensions
.load ./vector
.load ./memory

-- Check version
SELECT memory_version();

-- Configure local embedding model
SELECT memory_set_model('local', '/models/nomic-embed-text-v1.5.Q8_0.gguf');

-- Configure options
SELECT memory_set_option('max_tokens', 512);
SELECT memory_set_option('min_score', 0.75);

-- Add content
SELECT memory_add_text('SQLite is a C library that provides a lightweight disk-based database.', 'sqlite-docs');
SELECT memory_add_directory('/docs/sqlite', 'sqlite-docs');

-- Search
SELECT path, snippet, score
FROM memory_search
WHERE query = 'how does SQLite store data on disk';

-- View all memories with timestamps
SELECT
    hash,
    path,
    context,
    datetime(created_at, 'unixepoch', 'localtime') as created,
    datetime(last_accessed, 'unixepoch', 'localtime') as last_used
FROM dbmem_content
ORDER BY last_accessed DESC;

-- Delete by context
SELECT memory_delete_context('old-docs');

-- Clear all
SELECT memory_clear();
```

### Working with Contexts

```sql
-- Add memories with different contexts
SELECT memory_add_text('Meeting notes...', 'meetings');
SELECT memory_add_text('API documentation...', 'api-docs');
SELECT memory_add_text('Tutorial content...', 'tutorials');

-- Search within a context
SELECT * FROM memory_search
WHERE query = 'authentication'
AND context = 'api-docs';

-- List all contexts
SELECT context, COUNT(*) as count
FROM dbmem_content
GROUP BY context;

-- Delete a context
SELECT memory_delete_context('old-meetings');
```

### Memory Statistics

```sql
-- Total memories and chunks
SELECT
    (SELECT COUNT(*) FROM dbmem_content) as total_memories,
    (SELECT COUNT(*) FROM dbmem_vault) as total_chunks;

-- Storage usage
SELECT
    SUM(length(embedding)) as embedding_bytes,
    SUM(length) as content_bytes
FROM dbmem_vault;

-- Memories by context
SELECT
    COALESCE(context, '(none)') as context,
    COUNT(*) as count
FROM dbmem_content
GROUP BY context;

-- Recently accessed
SELECT path, datetime(last_accessed, 'unixepoch', 'localtime') as last_used
FROM dbmem_content
WHERE last_accessed > 0
ORDER BY last_accessed DESC
LIMIT 10;
```

---

## Compilation Options

| Option | Description |
|--------|-------------|
| `DBMEM_OMIT_IO` | Omit file/directory functions (for WASM) |
| `DBMEM_OMIT_LOCAL_ENGINE` | Omit llama.cpp local engine (for remote-only builds) |
| `DBMEM_OMIT_REMOTE_ENGINE` | Omit vector.space remote engine (for local-only builds) |
| `SQLITE_CORE` | Compile as part of SQLite core (not as loadable extension) |

---

## Error Handling

All functions return an error if:
- Required parameters are missing or of wrong type
- Database operations fail
- Model file not found (for local provider)
- Embedding dimension mismatch

Errors can be caught using standard SQLite error handling mechanisms.

```sql
-- Example error handling in application code
SELECT memory_add_text(123);  -- Error: expects TEXT parameter
SELECT memory_delete('abc');  -- Error: expects INTEGER parameter
```

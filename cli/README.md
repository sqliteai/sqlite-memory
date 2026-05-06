# sqlmem

`sqlmem` manages SQLite Memory databases backed by Markdown sources.
The CLI handles config, extension download/loading, optional PDF conversion cache, watch mode, and MCP. Markdown parsing, chunking, embedding, schema, FTS, and vector search stay inside the `sqlite-memory` extension.

## Build

```sh
make build
```

## Quick Start

```sh
sqlmem init --model /path/to/nomic-embed-text-v1.5.Q8_0.gguf
sqlmem add ./docs
sqlmem search -q "sqlite vector search" --limit 5
```

Remote embeddings use vectors.space when an API key is present:

```sh
sqlmem init --api-key "$sqlmem_API_KEY" --model text-embedding-3-small
```

API key precedence is: CLI flag, `sqlmem_API_KEY`, config.

## Config

`sqlmem init` creates `.sqlmem.json` in the project root. Edit it manually or use:

```sh
sqlmem config
sqlmem config set options.max_results 10
```

If no config is found, commands fail with:

```text
No .sqlmem.json found. Run `sqlmem init` first.
```

## Extensions

Extensions are cached globally:

- macOS: `~/Library/Application Support/sqlmem/extensions/`
- Linux: `~/.local/share/sqlmem/extensions/`
- Windows: `%APPDATA%/sqlmem/extensions/`

Override with `--extensions-dir` or `sqlmem_EXTENSIONS_DIR`.

```sh
sqlmem extensions path
sqlmem extensions install
sqlmem extensions install sync
sqlmem extensions list
sqlmem extensions update
```

The load order is `sqlite-vector`, `sqlite-memory`, then optional `sqlite-sync`.

## PDF

PDF indexing is disabled by default because it needs a separate conversion/OCR step. Enable it explicitly before adding PDF files:

```sh
sqlmem config set pdf.enabled true
```

PDFs are then converted to Markdown before indexing. The default converter shells out to `glm-ocr` and stores cache entries under the global PDF cache:

```text
<pdf-cache>/<sha256>/
  source.json
  content.md
```

Override with `--pdf-cache-dir` or `sqlmem_PDF_CACHE_DIR`.

## Commands

```sh
sqlmem add ./docs
sqlmem add ./file.pdf
sqlmem add -s ./docs -s ./file.pdf
sqlmem search -q "query" --json
sqlmem watch
sqlmem mcp --transport stdio
sqlmem mcp --transport http --addr 127.0.0.1:8765
sqlmem status
sqlmem clear
sqlmem reindex
sqlmem remove ./docs
sqlmem reset
```

Running `sqlmem` without arguments opens an interactive prompt with command history and arrow-key navigation.

## Test

```sh
make test
```

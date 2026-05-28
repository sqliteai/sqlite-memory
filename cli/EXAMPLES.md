# sqlmem Examples

Practical examples for common `sqlmem` workflows.

## Initialize A Project

Create `.sqlmem.json`, create the SQLite database, install required extensions, and configure the embedding model.

```sh
sqlmem init --model /models/nomic-embed-text-v1.5.Q8_0.gguf
```

Use a custom extension cache directory:

```sh
sqlmem init \
  --extensions-dir ~/.cache/sqlmem/extensions \
  --model /models/nomic-embed-text-v1.5.Q8_0.gguf
```

## Use Remote Embeddings

When an API key is present, `sqlmem` configures sqlite-memory for remote embeddings.

```sh
sqlmem init \
  --api-key "$sqlmem_API_KEY" \
  --model text-embedding-3-small
```

You can also set the API key through the environment:

```sh
export sqlmem_API_KEY="..."
sqlmem init --model text-embedding-3-small
```

Precedence is:

1. `--api-key`
2. `sqlmem_API_KEY`
3. `.sqlmem.json`

## Add Sources

Add a directory:

```sh
sqlmem add ./docs
```

Add one Markdown file:

```sh
sqlmem add ./README.md
```

Add multiple sources in one command:

```sh
sqlmem add ./docs ./notes/project.md
```

Use repeated `--source` flags:

```sh
sqlmem add -s ./docs -s ./notes/project.md
```

Attach a context label to added content:

```sh
sqlmem add ./docs --context product-docs
```

## Add PDFs

PDF indexing is disabled by default because it requires a separate conversion/OCR step. Enable it explicitly first:

```sh
sqlmem config set pdf.enabled true
```

PDF files are then converted to Markdown before indexing.

```sh
sqlmem add ./papers/sqlite-memory-overview.pdf
```

Use a custom PDF cache directory:

```sh
sqlmem --pdf-cache-dir ~/.cache/sqlmem/pdf add ./papers/report.pdf
```

Disable PDF support again:

```sh
sqlmem config set pdf.enabled false
```

## Search

Search with a positional query:

```sh
sqlmem search "hybrid search with sqlite"
```

Search with flags:

```sh
sqlmem search -q "embedding cache behavior" --limit 5
```

Return JSON for scripts:

```sh
sqlmem search -q "vector extension load order" --limit 10 --json
```

Pipe JSON to `jq`:

```sh
sqlmem search -q "pdf cache" --json | jq '.[].path'
```

## Watch Sources

Watch sources already stored in `.sqlmem.json`:

```sh
sqlmem watch
```

Watch explicit paths for the current session:

```sh
sqlmem watch ./docs ./notes/project.md
```

Use a shorter debounce window:

```sh
sqlmem watch --debounce 200ms
```

## Inspect Status

Show database path, source count, embedding selection, PDF cache, and indexed counts:

```sh
sqlmem status
```

Show the full configuration:

```sh
sqlmem config
```

## Edit Configuration

Set the default search limit:

```sh
sqlmem config set options.max_results 10
```

Lower the minimum score:

```sh
sqlmem config set options.min_score 0.65
```

Disable embedding cache:

```sh
sqlmem config set options.embedding_cache false
```

Set supported indexed file extensions:

```sh
sqlmem config set options.extensions "md,mdx,txt"
```

Opt into reStructuredText explicitly:

```sh
sqlmem config set options.extensions "md,mdx,txt,rst"
```

## Manage Extensions

Print the global extension cache path:

```sh
sqlmem extensions path
```

Install required extensions:

```sh
sqlmem extensions install
```

Install only sqlite-sync:

```sh
sqlmem extensions install sync
```

List installed extension files:

```sh
sqlmem extensions list
```

Update cached extensions:

```sh
sqlmem extensions update
```

Use a GitHub token for higher release API limits:

```sh
export GITHUB_TOKEN="..."
sqlmem extensions update
```

## MCP Server

Start the MCP server over stdio:

```sh
sqlmem mcp --transport stdio
```

Start the MCP server over HTTP:

```sh
sqlmem mcp --transport http --addr 127.0.0.1:8765
```

Available MCP tools:

```text
memory_search
memory_add_file
memory_add_content
memory_add_directory
memory_add_text
memory_clear
memory_delete
memory_delete_context
memory_reindex
memory_status
```

## Remove Sources

Remove a configured source from `.sqlmem.json`:

```sh
sqlmem remove ./docs
```

## Reindex Or Clear

Reindex all stored memory:

```sh
sqlmem reindex
```

Clear all memory content:

```sh
sqlmem clear
```

Reset the project by deleting the configured database and `.sqlmem.json`:

```sh
sqlmem reset
```

## Interactive Mode

Run without a subcommand to open the interactive prompt:

```sh
sqlmem
```

Inside the prompt:

```text
sqlmem> status
sqlmem> search "release notes"
sqlmem> add ./notes
sqlmem> quit
```

Command history is available with the up and down arrow keys.

## Script Examples

Fail if no results are returned:

```sh
results="$(sqlmem search -q "database migration" --json)"
count="$(printf '%s' "$results" | jq 'length')"
test "$count" -gt 0
```

Index all Markdown files changed in the current Git branch:

```sh
git diff --name-only main...HEAD -- '*.md' '*.mdx' |
while IFS= read -r file; do
  [ -f "$file" ] && sqlmem add "$file"
done
```

Create a project-local database name:

```sh
sqlmem init --model /models/nomic-embed-text-v1.5.Q8_0.gguf
sqlmem config set database ".cache/project-memory.sqlite"
```

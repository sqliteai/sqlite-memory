# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.3.5] - 2026-06-10

### Added

- **Deferred embedding generation** via the new `defer_embeddings` option. When enabled, content is stored immediately without computing embeddings or FTS entries, so ingestion needs no embedding model and returns instantly. Deferred content is invisible to `memory_search` until it is processed. Requires `save_content=1`.
- **`memory_embed_pending([limit])`** generates the embeddings and FTS entries for deferred content, either all at once or in batches of `limit` rows. Each row is processed in its own SAVEPOINT, so an interrupted call can simply be retried and other connections observe per-file progress while a batch runs.
- **`memory_pending_count()`** returns how many rows are still waiting for embedding generation, for progress reporting during a `memory_embed_pending()` loop.

### Changed

- File nodes returned by `memory_list_files()` now include an `indexed` boolean, which is `false` while content is waiting for embedding generation.

## [1.3.4] - 2026-06-09

### Fixed

- **`memory_rename_file()` with `preserve_duplicate_paths=1`** now recomputes the path-scoped hash and updates the related embedding and FTS rows in a single transaction. Previously a renamed entry kept its old hash and its embeddings and FTS rows were left orphaned, so the renamed content stopped matching correctly in search.

### Changed

- Renaming a `preserve_duplicate_paths` row that was created with `save_content=0` now returns a clear error instead of silently corrupting the entry, because the path-scoped hash cannot be recomputed without the stored content.

## [1.3.3] - 2026-06-05

### Added

- **Explicit empty directory markers.** With `preserve_duplicate_paths=1`, `memory_add_content('dirname/', '')` records a directory that contains no files. Markers appear as directories in `memory_list_files()`, are recreated as real directories by `memory_materialize_files()`, are excluded from search, and can be deleted with `memory_delete_file()` using either `dirname` or `dirname/` (which removes only the marker, never its children).

### Changed

- **Provider and model settings are now picked up automatically by new connections.** The embedding engine is initialized lazily on first use from the settings persisted in `dbmem_settings`, so `memory_set_model()` no longer has to be called on every connection. Calling it explicitly still initializes the engine immediately, which is useful to preload and validate a model. API keys remain connection-scoped: `memory_set_apikey()` must still be called per connection for remote providers.
- `memory_materialize_files()` now counts directory markers in its return value.

### Fixed

- Re-adding the same path with `preserve_duplicate_paths=1` is now idempotent instead of churning rows on every call.

## [1.3.2] - 2026-06-04

### Added

- **`preserve_duplicate_paths` option** keeps separate entries for distinct logical paths even when their content is identical or empty. With it enabled, `dbmem_content.hash` becomes path-scoped and identifies an entry rather than only its raw content. Intended for virtual-file and editor-style workflows where two files may legitimately hold the same text.

## [1.3.1] - 2026-06-04

### Fixed

- Error messages returned by many `memory_*` functions were truncated or garbled because of a wrong length argument. Errors now arrive intact and readable.

## [1.3.0] - 2026-05-28

### Added

- **`memory_add_content(path, content [, context])`** indexes content supplied by the caller without reading the filesystem, so applications that already hold the text (uploads, editors, generated documents) can index it directly. Available even in `DBMEM_OMIT_IO` builds.
- **`memory_rename_file(old_path, new_path)`** renames an indexed path without re-reading or re-embedding its content.
- **`memory_delete_file(path)`** deletes a single indexed entry by logical path or by exact local source path.
- **`memory_list_files()`** returns the indexed content as a JSON directory and file tree.
- **`memory_materialize_files([root_path])`** writes stored content back to disk, creating parent directories as needed. Paths containing `..` are rejected, and files that already match are left untouched.
- **`memory_is_enabled()`** reports whether the current database already has the sqlite-memory schema.

### Changed

- **File paths are now portable.** Absolute paths are stored as a logical suffix (`/Users/me/docs/readme.md` becomes `docs/readme.md`) and `memory_add_directory()` stores paths relative to the scanned root, so an indexed database can be synced between machines. The original local path moves to `dbmem_content_source`, a local-only table that is never synchronized.
- **Local path collisions are resolved automatically.** When a logical suffix would collide, it is extended until unique. Importing a local file whose logical path already exists without local provenance — for example after a sync — updates that entry and attaches provenance to it instead of creating a duplicate.
- `memory_add_directory()` now returns the number of files scanned successfully, rather than only the number of newly processed files.
- `memory_clear()` also clears the local `dbmem_content_source` table.

## [1.2.2] - 2026-05-22

### Fixed

- **Adding content before configuring a model no longer crashes.** `memory_add_text()`, `memory_add_file()` and `memory_add_directory()` now return "memory_set_model must be called before adding content".
- Searching an empty database now says to add content first, instead of reporting the misleading "embedding dimension is not specified".

## [1.2.1] - 2026-05-21

### Fixed

- **Local embedding crashes on certain content.** Whitespace-only chunks are no longer sent to the encoder, and the llama.cpp context is sized from the configured chunk window with batch limits kept in step, which removes encoder assertion failures during indexing.
- Changing `max_tokens`, `overlay_tokens` or `chars_per_tokens` now rebuilds the local engine and invalidates the cached local embeddings, so stale embeddings computed with the old token window are no longer reused.
- Local engine error messages are now per-thread, so errors from one connection are no longer reported on another.

## [1.2.0] - 2026-05-12

### Added

- **Token usage and truncation are now recorded** for every embedding, in the new `n_tokens` and `truncated` columns of `dbmem_vault` and `dbmem_cache`. Existing databases are migrated automatically on open. Cached embeddings restore this metadata instead of reporting zeros, making it possible to see which content was truncated by the model.

### Changed

- **C API (breaking):** in `dbmem_embedding_result_t`, the `n_tokens_truncated` integer is replaced by a `truncated` boolean, and `n_tokens` now means processed tokens (`0` when unknown). Custom providers registered with `sqlite3_memory_register_provider` must be updated.

## [1.1.0] - 2026-05-06

### Added

- **`sqlmem` command line tool** for managing sqlite-memory projects from the terminal. It creates and manages the database, downloads and loads the required extensions, configures the embedding model, indexes Markdown sources, runs searches, and can watch files for changes. `sqlmem mcp` exposes the memory tools to agents over MCP (stdio or HTTP), and optional PDF indexing can be enabled with `sqlmem config set pdf.enabled true`. See [`cli/README.md`](cli/README.md).
- **MDX support.** Files with an `.mdx` extension now have their `import`/`export` statements and `{...}` JSX expressions stripped before indexing, so MDX documents are indexed as clean prose instead of polluting search results with JavaScript scaffolding.

### Fixed

- Remote providers that omit `output_dimension` in their response no longer fail with "Missing embedding data in API response"; the dimension is inferred from the returned embedding.

## [1.0.0] - 2026-04-22

### Changed

- **Content hashes are now TEXT** (a 16-character hex string such as `'9e3779b97f4a7c15'`) across `dbmem_content`, `dbmem_vault` and `dbmem_cache`, and the `hash` column of `memory_search` returns that value. `memory_delete()` now takes the hash as TEXT. **Databases created with earlier versions must be rebuilt.**
- **`memory_set_model()` is now atomic.** The engine switch, settings update and reindex all happen in one transaction; if any step fails the previous provider, model and engine are restored, instead of leaving the connection half-configured.
- **`memory_set_apikey()` now takes effect immediately** on an already-initialized remote engine, so it no longer has to be called before `memory_set_model()`.
- Switching between provider classes (custom, local, remote) frees the previous engine right away instead of keeping it resident until the connection closes, which matters for large local models holding RAM or VRAM.
- `memory_set_option()` now accepts `0` for `max_results`, `vector_weight`, `text_weight` and `min_score`; previously zero was silently ignored and the old value kept.
- `memory_add_directory()` runs its cleanup pass transactionally and then processes each file in its own transaction, so one failing file rolls back only itself and previously indexed files remain valid.

### Fixed

- **`memory_add_directory()` could delete entries from unrelated directories** sharing a name prefix — syncing `/docs` could remove entries belonging to `/docs-old`. Paths are now matched on real directory boundaries.
- **Markdown headings did not start a new chunk**, so sections were merged into the preceding one and chunk boundaries did not follow the document structure as documented.
- `memory_reindex()` is now transactional, aborts on the first failing row instead of silently swallowing errors, and no longer fails when a leftover temporary table exists.
- Repeated `memory_search` queries on the same statement could reuse stale results; the cursor is now fully reset between queries.
- Fixed a potential crash when the local embedding engine reported a tokenize or encode failure.

## [0.9.0] - 2026-04-13

### Added

- **Memory synchronization between agents.** `memory_enable_sync([context, ...])` enables CRDT-based sync of `dbmem_content` through [sqlite-sync](https://github.com/sqliteai/sqlite-sync), using block-level LWW on the content so concurrent line-level edits from different agents merge without conflicts. Called with no arguments it syncs everything; with one or more context names it replicates only those contexts. `memory_disable_sync()` removes the sync infrastructure while preserving the data.
- **`memory_reindex()`** generates embeddings and FTS entries for content that has none — the situation after pulling content from other agents, since embeddings are always local and never synchronized. It also repairs hashes left stale by a CRDT merge.

### Fixed

- **Context-filtered searches returned wrong or empty results.** `memory_search` with `context = ...` referenced a nonexistent column in the vector branch ([#2](https://github.com/sqliteai/sqlite-memory/issues/2)).
- Combining `query` and `context` without `max_entries` assigned the values to the wrong search parameters.
- A large `max_results` combined with `search_oversample` could overflow and under-allocate; the query now fails cleanly instead.

## [0.8.5] - 2026-04-07

### Fixed

- macOS builds now resolve SQLite symbols from the host process at load time, so the extension loads correctly into arbitrary SQLite builds and alongside other extensions.
- Extension loading is now failure-safe: if any function fails to register, the extension cleans up instead of leaking.
- Hardened the remote embedding engine against over-long provider and model names and against invalid or empty embedding arrays in the API response.
- `memory_add_*` now reports oversized content as an error instead of silently truncating it, and error messages from the file, directory and search paths are correctly bounded and formatted.

### Changed

- `API.md` now documents `memory_search` correctly: `query`, `max_entries` and `context` are hidden filter columns used in `WHERE` (`context` was previously listed as an output column), and the output columns are `hash`, `seq`, `ranking`, `path` and `snippet`. It also gained a C API section for `sqlite3_memory_register_provider`.

## [0.8.3] - 2026-04-01

### Fixed

- The macOS slice of the shipped `memory.xcframework` now uses the versioned bundle layout required by Xcode 26, fixing framework validation and embedding failures for Apple developers. The xcframework release asset is also published correctly again.

## [0.8.2] - 2026-03-24

### Fixed

- Renamed internal exported symbols to a `dbmemory_*` prefix, resolving the symbol conflicts that prevented sqlite-memory from being built or statically linked alongside sqlite-sync.

## [0.8.1] - 2026-03-17

### Changed

- **C API (breaking):** `dbmem_provider_t` gained an `xdata` user pointer, and all three provider callbacks now receive it. Providers written against 0.8.0 must be updated.

## [0.8.0] - 2026-03-17

### Added

- **Custom embedding providers.** `sqlite3_memory_register_provider()` lets a host application plug in its own embedding engine, which is then selected from SQL with `memory_set_model('<provider_name>', '<model>')`. Custom providers work regardless of the `DBMEM_OMIT_LOCAL_ENGINE` and `DBMEM_OMIT_REMOTE_ENGINE` build options, and are used for both indexing and search.

## [0.7.5] - 2026-03-17

### Added

- **`OMIT_CURL=1` build option.** On Apple platforms the remote embedding engine is built on `NSURLSession` instead of bundling libcurl and mbedTLS, producing a considerably smaller binary with no third-party TLS dependency. Remote embeddings continue to work unchanged.

## [0.7.1] - 2026-03-04

### Added

- sqlite-memory is now bundled into `sqlite-wasm` releases.

## [0.7.0] - 2026-03-04

Initial public release.

### Added

- **Hybrid semantic search** through the `memory_search` virtual table, combining vector similarity with FTS5 full-text search. Results expose `hash`, `seq`, `ranking`, `path` and `snippet`, and queries can be filtered by `context` or capped per query with `max_entries`.
- **Markdown-aware indexing** with `memory_add_text()`, `memory_add_file()` and `memory_add_directory()`. Content is chunked along semantic boundaries, and content-hash change detection means unchanged files are skipped, modified files are atomically replaced, and deleted files are cleaned up — so sync functions are cheap to call repeatedly.
- **Local and remote embeddings**, selected with `memory_set_model()`: the built-in llama.cpp engine for local GGUF models, or the [vectors.space](https://vectors.space) service for remote providers (with `memory_set_apikey()`). Changing the provider or model automatically re-embeds existing content.
- **Embedding cache** so re-indexing the same text skips redundant computation and API calls, with optional size-based eviction (`embedding_cache`, `cache_max_entries`) and `memory_cache_clear()`.
- **Deletion functions**: `memory_delete()`, `memory_delete_context()` and `memory_clear()`.
- **Configuration** via `memory_set_option()` and `memory_get_option()`, covering chunking (`max_tokens`, `overlay_tokens`), parsing (`skip_semantic`, `skip_html`, `extensions`), search behaviour (`max_results`, `fts_enabled`, `vector_weight`, `text_weight`, `min_score`, `search_oversample`) and storage (`save_content`, `update_access`).
- **Transactional safety**: every ingest runs inside a SAVEPOINT, so content is never left partially indexed.
- **Prebuilt binaries** for macOS, iOS and iOS Simulator (XCFramework), Linux (including musl), Windows, Android and WASM, plus the `DBMEM_OMIT_IO`, `DBMEM_OMIT_LOCAL_ENGINE` and `DBMEM_OMIT_REMOTE_ENGINE` build options for trimming the extension.

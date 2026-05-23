package memory

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/sqliteai/sqlite-memory/cli/internal/config"
)

const defaultRemoteModel = "text-embedding-3-small"

type ModelOptions struct {
	Provider string
	Model    string
	APIKey   string
}

type SearchResult struct {
	Hash    string  `json:"hash"`
	Seq     int     `json:"seq"`
	Ranking float64 `json:"ranking"`
	Path    string  `json:"path"`
	Snippet string  `json:"snippet"`
}

func ResolveModel(cfg config.Config, opts ModelOptions) ModelOptions {
	apiKey := config.ResolveAPIKey(cfg, opts.APIKey)
	model := opts.Model
	if model == "" {
		model = cfg.Embedding.Model
	}
	provider := opts.Provider
	if provider == "" {
		provider = cfg.Embedding.Provider
	}
	if apiKey != "" {
		if provider == "" || provider == "local" {
			provider = "openai"
		}
		if model == "" {
			model = defaultRemoteModel
		}
	} else {
		provider = "local"
	}
	return ModelOptions{Provider: provider, Model: model, APIKey: apiKey}
}

func Configure(ctx context.Context, db *sql.DB, cfg config.Config, opts ModelOptions) error {
	resolved := ResolveModel(cfg, opts)
	for key, value := range optionMap(cfg.Options) {
		if _, err := db.ExecContext(ctx, "SELECT memory_set_option(?, ?)", key, value); err != nil {
			return err
		}
	}
	if resolved.APIKey != "" {
		if _, err := db.ExecContext(ctx, "SELECT memory_set_apikey(?)", resolved.APIKey); err != nil {
			return err
		}
	}
	if resolved.Model == "" {
		return nil
	}
	_, err := db.ExecContext(ctx, "SELECT memory_set_model(?, ?)", resolved.Provider, resolved.Model)
	return err
}

func AddFile(ctx context.Context, db *sql.DB, path, contextLabel string) error {
	if contextLabel == "" {
		_, err := db.ExecContext(ctx, "SELECT memory_add_file(?)", path)
		return err
	}
	_, err := db.ExecContext(ctx, "SELECT memory_add_file(?, ?)", path, contextLabel)
	return err
}

func AddDirectory(ctx context.Context, db *sql.DB, path, contextLabel string) error {
	if contextLabel == "" {
		_, err := db.ExecContext(ctx, "SELECT memory_add_directory(?)", path)
		return err
	}
	_, err := db.ExecContext(ctx, "SELECT memory_add_directory(?, ?)", path, contextLabel)
	return err
}

func AddText(ctx context.Context, db *sql.DB, text, contextLabel string) error {
	if contextLabel == "" {
		_, err := db.ExecContext(ctx, "SELECT memory_add_text(?)", text)
		return err
	}
	_, err := db.ExecContext(ctx, "SELECT memory_add_text(?, ?)", text, contextLabel)
	return err
}

func Search(ctx context.Context, db *sql.DB, query string, limit int) ([]SearchResult, error) {
	if limit > 0 {
		return scanSearch(db.QueryContext(ctx, "SELECT hash, seq, ranking, path, snippet FROM memory_search WHERE query = ? AND max_entries = ?", query, limit))
	}
	return scanSearch(db.QueryContext(ctx, "SELECT hash, seq, ranking, path, snippet FROM memory_search WHERE query = ?", query))
}

func Clear(ctx context.Context, db *sql.DB) error {
	_, err := db.ExecContext(ctx, "SELECT memory_clear()")
	return err
}

func Delete(ctx context.Context, db *sql.DB, hash string) error {
	_, err := db.ExecContext(ctx, "SELECT memory_delete(?)", hash)
	return err
}

func DeletePath(ctx context.Context, db *sql.DB, path string) error {
	rows, err := db.QueryContext(ctx, "SELECT hash FROM dbmem_content WHERE path = ?", path)
	if err != nil {
		return err
	}
	defer rows.Close()
	var hashes []string
	for rows.Next() {
		var hash string
		if err := rows.Scan(&hash); err != nil {
			return err
		}
		hashes = append(hashes, hash)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	for _, hash := range hashes {
		if err := Delete(ctx, db, hash); err != nil && !errors.Is(err, sql.ErrNoRows) {
			return err
		}
	}
	return nil
}

func DeleteContext(ctx context.Context, db *sql.DB, contextLabel string) error {
	_, err := db.ExecContext(ctx, "SELECT memory_delete_context(?)", contextLabel)
	return err
}

func Reindex(ctx context.Context, db *sql.DB) error {
	_, err := db.ExecContext(ctx, "SELECT memory_reindex()")
	return err
}

func Status(ctx context.Context, db *sql.DB) (map[string]any, error) {
	out := map[string]any{}
	var memories, chunks int
	_ = db.QueryRowContext(ctx, "SELECT COUNT(*) FROM dbmem_content").Scan(&memories)
	_ = db.QueryRowContext(ctx, "SELECT COUNT(*) FROM dbmem_vault").Scan(&chunks)
	out["memories"] = memories
	out["chunks"] = chunks
	return out, nil
}

type ContentResult struct {
	Hash         string  `json:"hash"`
	Path         string  `json:"path"`
	Context      *string `json:"context"`
	Value        *string `json:"value"`
	CreatedAt    int64   `json:"created_at"`
	LastAccessed int64   `json:"last_accessed"`
}

func Get(ctx context.Context, db *sql.DB, hash string) (string, error) {
	var r ContentResult
	err := db.QueryRowContext(ctx,
		"SELECT hash, path, context, value, created_at, last_accessed FROM dbmem_content WHERE hash = ?",
		hash,
	).Scan(&r.Hash, &r.Path, &r.Context, &r.Value, &r.CreatedAt, &r.LastAccessed)
	if errors.Is(err, sql.ErrNoRows) {
		return "", fmt.Errorf("hash not found: %s", hash)
	}
	if err != nil {
		return "", err
	}
	data, _ := json.MarshalIndent(r, "", "  ")
	return string(data), nil
}

func Query(ctx context.Context, db *sql.DB, query string) (string, error) {
	conn, err := db.Conn(ctx)
	if err != nil {
		return "", err
	}
	defer conn.Close()
	if _, err := conn.ExecContext(ctx, "PRAGMA query_only = ON"); err != nil {
		return "", err
	}
	rows, err := conn.QueryContext(ctx, query)
	if err != nil {
		return "", err
	}
	defer rows.Close()
	cols, err := rows.Columns()
	if err != nil {
		return "", err
	}
	var results []map[string]any
	for rows.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return "", err
		}
		row := make(map[string]any, len(cols))
		for i, col := range cols {
			row[col] = vals[i]
		}
		results = append(results, row)
	}
	if err := rows.Err(); err != nil {
		return "", err
	}
	data, _ := json.MarshalIndent(results, "", "  ")
	return string(data), nil
}

func ResultsJSON(results []SearchResult) string {
	data, _ := json.MarshalIndent(results, "", "  ")
	return string(data)
}

func scanSearch(rows *sql.Rows, err error) ([]SearchResult, error) {
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var results []SearchResult
	for rows.Next() {
		var r SearchResult
		if err := rows.Scan(&r.Hash, &r.Seq, &r.Ranking, &r.Path, &r.Snippet); err != nil {
			return nil, err
		}
		results = append(results, r)
	}
	return results, rows.Err()
}

func optionMap(opts config.Options) map[string]any {
	return map[string]any{
		"max_tokens":        opts.MaxTokens,
		"overlay_tokens":    opts.OverlayTokens,
		"max_results":       opts.MaxResults,
		"min_score":         opts.MinScore,
		"vector_weight":     opts.VectorWeight,
		"text_weight":       opts.TextWeight,
		"search_oversample": opts.SearchOversample,
		"extensions":        opts.Extensions,
		"embedding_cache":   boolInt(opts.EmbeddingCache),
		"cache_max_entries": opts.CacheMaxEntries,
	}
}

func boolInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func FormatResult(r SearchResult) string {
	return fmt.Sprintf("%.3f  %s\n%s", r.Ranking, r.Path, r.Snippet)
}

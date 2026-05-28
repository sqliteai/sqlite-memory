package memory

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"path/filepath"

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

func AddContent(ctx context.Context, db *sql.DB, path, content, contextLabel string) error {
	if contextLabel == "" {
		_, err := db.ExecContext(ctx, "SELECT memory_add_content(?, ?)", path, content)
		return err
	}
	_, err := db.ExecContext(ctx, "SELECT memory_add_content(?, ?, ?)", path, content, contextLabel)
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
	for _, candidate := range pathCandidates(path) {
		var deleted int
		if err := db.QueryRowContext(ctx, "SELECT memory_delete_file(?)", candidate).Scan(&deleted); err != nil {
			return err
		}
		if deleted > 0 {
			return nil
		}
	}
	return nil
}

func pathCandidates(path string) []string {
	candidates := []string{filepath.ToSlash(filepath.Clean(path))}
	if filepath.IsAbs(path) {
		candidates = append(candidates, filepath.ToSlash(filepath.Base(path)))
	}

	out := candidates[:0]
	seen := map[string]struct{}{}
	for _, candidate := range candidates {
		if candidate == "." || candidate == "" {
			continue
		}
		if _, ok := seen[candidate]; ok {
			continue
		}
		seen[candidate] = struct{}{}
		out = append(out, candidate)
	}
	return out
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

package config

import (
	"os"
	"path/filepath"
	"testing"
)

func TestConfigRoundTrip(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, FileName)
	cfg := Default()
	cfg.Database = "test.sqlite"
	cfg.Sources = []string{"docs"}
	if err := Save(path, cfg); err != nil {
		t.Fatal(err)
	}
	got, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if got.Database != "test.sqlite" {
		t.Fatalf("database = %q", got.Database)
	}
	if len(got.Sources) != 1 || got.Sources[0] != "docs" {
		t.Fatalf("sources = %#v", got.Sources)
	}
}

func TestDefaultExtensionsAreConservative(t *testing.T) {
	if got := Default().Options.Extensions; got != "md,mdx,txt" {
		t.Fatalf("extensions = %q", got)
	}
	if Default().PDF.Enabled {
		t.Fatal("pdf should be disabled by default")
	}
}

func TestSetDot(t *testing.T) {
	cfg := Default()
	if err := SetDot(&cfg, "options.max_results", "7"); err != nil {
		t.Fatal(err)
	}
	if cfg.Options.MaxResults != 7 {
		t.Fatalf("max_results = %d", cfg.Options.MaxResults)
	}
	if err := SetDot(&cfg, "embedding.model", "abc"); err != nil {
		t.Fatal(err)
	}
	if cfg.Embedding.Model != "abc" {
		t.Fatalf("model = %q", cfg.Embedding.Model)
	}
	if err := SetDot(&cfg, "options.cache_max_entries", "0"); err != nil {
		t.Fatal(err)
	}
	if cfg.Options.CacheMaxEntries != 0 {
		t.Fatalf("cache_max_entries = %d", cfg.Options.CacheMaxEntries)
	}
}

func TestSourceNormalizationAndDedup(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "docs")
	if err := os.Mkdir(path, 0755); err != nil {
		t.Fatal(err)
	}
	cfgPath := filepath.Join(dir, FileName)
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.Chdir(wd); err != nil {
			t.Fatal(err)
		}
	})
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}

	stored, resolved, err := NormalizeSource(cfgPath, "./docs")
	if err != nil {
		t.Fatal(err)
	}
	expected, err := filepath.EvalSymlinks(path)
	if err != nil {
		t.Fatal(err)
	}
	if stored != "docs" {
		t.Fatalf("stored = %q", stored)
	}
	if resolved != expected {
		t.Fatalf("resolved = %q", resolved)
	}
	cfg := Default()
	if !AddSource(&cfg, cfgPath, stored) {
		t.Fatal("first source was not added")
	}
	if AddSource(&cfg, cfgPath, filepath.Join(".", "docs")) {
		t.Fatal("duplicate source was added")
	}
	if len(cfg.Sources) != 1 {
		t.Fatalf("sources = %#v", cfg.Sources)
	}
	if cfg.Sources[0] != "docs" {
		t.Fatalf("source was stored as %q", cfg.Sources[0])
	}
	if !RemoveSource(&cfg, cfgPath, filepath.Join(".", "docs")) {
		t.Fatal("source was not removed")
	}
}

func TestAddSourceRewritesEquivalentAbsoluteSource(t *testing.T) {
	dir := t.TempDir()
	cfgPath := filepath.Join(dir, FileName)
	docs := filepath.Join(dir, "docs")
	if err := os.Mkdir(docs, 0755); err != nil {
		t.Fatal(err)
	}
	cfg := Default()
	cfg.Sources = []string{docs, "docs"}

	if !AddSource(&cfg, cfgPath, "docs") {
		t.Fatal("equivalent absolute source was not rewritten")
	}
	if len(cfg.Sources) != 1 || cfg.Sources[0] != "docs" {
		t.Fatalf("sources = %#v", cfg.Sources)
	}
}

func TestSourceNormalizationFromSubdirectory(t *testing.T) {
	dir := t.TempDir()
	cfgPath := filepath.Join(dir, FileName)
	subdir := filepath.Join(dir, "subdir")
	docs := filepath.Join(dir, "docs")
	if err := os.Mkdir(subdir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(docs, 0755); err != nil {
		t.Fatal(err)
	}
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.Chdir(wd); err != nil {
			t.Fatal(err)
		}
	})
	if err := os.Chdir(subdir); err != nil {
		t.Fatal(err)
	}

	stored, resolved, err := NormalizeSource(cfgPath, "../docs")
	if err != nil {
		t.Fatal(err)
	}
	if stored != "docs" {
		t.Fatalf("stored = %q", stored)
	}
	if resolved != normalizeExistingPath(docs) {
		t.Fatalf("resolved = %q", resolved)
	}
}

func TestAPIKeyPrecedence(t *testing.T) {
	t.Setenv("sqlmem_API_KEY", "env")
	cfg := Default()
	cfg.Embedding.APIKey = "config"
	if got := ResolveAPIKey(cfg, "cli"); got != "cli" {
		t.Fatalf("cli key not preferred: %q", got)
	}
	if got := ResolveAPIKey(cfg, ""); got != "env" {
		t.Fatalf("env key not preferred: %q", got)
	}
	os.Unsetenv("sqlmem_API_KEY")
	if got := ResolveAPIKey(cfg, ""); got != "config" {
		t.Fatalf("config key not used: %q", got)
	}
}

func TestDefaultCacheDirs(t *testing.T) {
	t.Setenv("sqlmem_EXTENSIONS_DIR", "/tmp/sqlmem-ext")
	t.Setenv("sqlmem_PDF_CACHE_DIR", "/tmp/sqlmem-pdf")
	if got := DefaultExtensionsDir(); got != "/tmp/sqlmem-ext" {
		t.Fatalf("extensions dir = %q", got)
	}
	if got := DefaultPDFCacheDir(); got != "/tmp/sqlmem-pdf" {
		t.Fatalf("pdf cache dir = %q", got)
	}
}

func TestLoadPreservesExplicitZeroAndFalse(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, FileName)
	raw := `{
  "database": "memory.sqlite",
  "pdf": {"enabled": false},
  "options": {
    "cache_max_entries": 0,
    "min_score": 0,
    "embedding_cache": false
  }
}`
	if err := os.WriteFile(path, []byte(raw), 0644); err != nil {
		t.Fatal(err)
	}
	cfg, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.PDF.Enabled {
		t.Fatal("pdf.enabled was defaulted over explicit false")
	}
	if cfg.Options.CacheMaxEntries != 0 {
		t.Fatalf("cache_max_entries = %d", cfg.Options.CacheMaxEntries)
	}
	if cfg.Options.MinScore != 0 {
		t.Fatalf("min_score = %f", cfg.Options.MinScore)
	}
	if cfg.Options.EmbeddingCache {
		t.Fatal("embedding_cache was defaulted over explicit false")
	}
	if cfg.Options.MaxResults != Default().Options.MaxResults {
		t.Fatalf("missing max_results did not default: %d", cfg.Options.MaxResults)
	}
}

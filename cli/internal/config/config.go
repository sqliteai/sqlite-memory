package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const FileName = ".sqlmem.json"

var ErrNotFound = errors.New("No .sqlmem.json found. Run `sqlmem init` first.")

type Config struct {
	Database          string            `json:"database"`
	ExtensionsDir     string            `json:"extensions_dir"`
	Extensions        map[string]string `json:"extensions"`
	ExtensionVersions map[string]string `json:"extension_versions"`
	Sources           []string          `json:"sources"`
	Embedding         EmbeddingConfig   `json:"embedding"`
	PDF               PDFConfig         `json:"pdf"`
	Options           Options           `json:"options"`
	CreatedAt         string            `json:"created_at"`
	UpdatedAt         string            `json:"updated_at"`
}

type EmbeddingConfig struct {
	Provider string `json:"provider"`
	Model    string `json:"model"`
	APIKey   string `json:"api_key"`
}

type PDFConfig struct {
	Enabled  bool   `json:"enabled"`
	Model    string `json:"model"`
	Provider string `json:"provider"`
	APIKey   string `json:"api_key"`
	CacheDir string `json:"cache_dir"`
	Force    bool   `json:"force"`
}

type Options struct {
	MaxTokens        int     `json:"max_tokens"`
	OverlayTokens    int     `json:"overlay_tokens"`
	MaxResults       int     `json:"max_results"`
	MinScore         float64 `json:"min_score"`
	VectorWeight     float64 `json:"vector_weight"`
	TextWeight       float64 `json:"text_weight"`
	SearchOversample int     `json:"search_oversample"`
	Extensions       string  `json:"extensions"`
	EmbeddingCache   bool    `json:"embedding_cache"`
	CacheMaxEntries  int     `json:"cache_max_entries"`
}

func Default() Config {
	now := time.Now().UTC().Format(time.RFC3339)
	return Config{
		Database:      "memory.sqlite",
		ExtensionsDir: "",
		Extensions: map[string]string{
			"vector": "latest",
			"memory": "latest",
			"sync":   "",
		},
		ExtensionVersions: map[string]string{
			"vector": "latest",
			"memory": "latest",
			"sync":   "latest",
		},
		Sources: []string{},
		Embedding: EmbeddingConfig{
			Provider: "local",
			Model:    "",
			APIKey:   "",
		},
		PDF: PDFConfig{
			Enabled:  false,
			Provider: "local",
		},
		Options: Options{
			MaxTokens:        512,
			OverlayTokens:    100,
			MaxResults:       30,
			MinScore:         0.75,
			VectorWeight:     0.6,
			TextWeight:       0.4,
			SearchOversample: 4,
			Extensions:       "md,mdx,txt",
			EmbeddingCache:   true,
			CacheMaxEntries:  10000,
		},
		CreatedAt: now,
		UpdatedAt: now,
	}
}

func Load(path string) (Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return Config{}, ErrNotFound
		}
		return Config{}, err
	}
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return Config{}, err
	}
	applyDefaults(data, &cfg)
	return cfg, nil
}

func Save(path string, cfg Config) error {
	if cfg.CreatedAt == "" {
		cfg.CreatedAt = time.Now().UTC().Format(time.RFC3339)
	}
	cfg.UpdatedAt = time.Now().UTC().Format(time.RFC3339)
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return os.WriteFile(path, data, 0644)
}

func Find(start string) (string, error) {
	dir, err := filepath.Abs(start)
	if err != nil {
		return "", err
	}
	for {
		path := filepath.Join(dir, FileName)
		if _, err := os.Stat(path); err == nil {
			return path, nil
		}
		next := filepath.Dir(dir)
		if next == dir {
			return "", ErrNotFound
		}
		dir = next
	}
}

func LoadFrom(start string) (Config, string, error) {
	path, err := Find(start)
	if err != nil {
		return Config{}, "", err
	}
	cfg, err := Load(path)
	return cfg, path, err
}

func DefaultExtensionsDir() string {
	if v := firstEnv("sqlmem_EXTENSIONS_DIR", "SQLMEM_EXTENSIONS_DIR"); v != "" {
		return v
	}
	base := dataRoot()
	return filepath.Join(base, "extensions")
}

func DefaultPDFCacheDir() string {
	if v := firstEnv("sqlmem_PDF_CACHE_DIR", "SQLMEM_PDF_CACHE_DIR"); v != "" {
		return v
	}
	base := dataRoot()
	return filepath.Join(base, "pdf-cache")
}

func ResolveExtensionsDir(cfg Config, override string) string {
	if override != "" {
		return override
	}
	if v := firstEnv("sqlmem_EXTENSIONS_DIR", "SQLMEM_EXTENSIONS_DIR"); v != "" {
		return v
	}
	if cfg.ExtensionsDir != "" {
		return cfg.ExtensionsDir
	}
	return DefaultExtensionsDir()
}

func ResolvePDFCacheDir(cfg Config, override string) string {
	if override != "" {
		return override
	}
	if v := firstEnv("sqlmem_PDF_CACHE_DIR", "SQLMEM_PDF_CACHE_DIR"); v != "" {
		return v
	}
	if cfg.PDF.CacheDir != "" {
		return cfg.PDF.CacheDir
	}
	return DefaultPDFCacheDir()
}

func ResolveAPIKey(cfg Config, cliValue string) string {
	if cliValue != "" {
		return cliValue
	}
	if v := firstEnv("sqlmem_API_KEY", "SQLMEM_API_KEY"); v != "" {
		return v
	}
	return cfg.Embedding.APIKey
}

func SetDot(cfg *Config, key string, raw string) error {
	if key == "" {
		return fmt.Errorf("empty config key")
	}
	data, err := json.Marshal(cfg)
	if err != nil {
		return err
	}
	var m map[string]any
	if err := json.Unmarshal(data, &m); err != nil {
		return err
	}
	parts := strings.Split(key, ".")
	cur := m
	for _, part := range parts[:len(parts)-1] {
		next, ok := cur[part].(map[string]any)
		if !ok {
			return fmt.Errorf("unknown config key %q", key)
		}
		cur = next
	}
	leaf := parts[len(parts)-1]
	if _, ok := cur[leaf]; !ok {
		return fmt.Errorf("unknown config key %q", key)
	}
	cur[leaf] = parseValue(raw, cur[leaf])
	data, err = json.Marshal(m)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(data, cfg); err != nil {
		return err
	}
	applyDefaults(data, cfg)
	return nil
}

func NormalizeSource(configPath, source string) (string, string, error) {
	resolved, err := normalizeUserSource(source)
	if err != nil {
		return "", "", err
	}
	cfgDir, err := configDir(configPath)
	if err != nil {
		return "", "", err
	}
	stored, err := filepath.Rel(cfgDir, resolved)
	if err != nil {
		return "", "", err
	}
	return filepath.Clean(stored), resolved, nil
}

func ResolveSource(configPath, source string) (string, error) {
	if filepath.IsAbs(source) {
		return normalizeExistingPath(source), nil
	}
	cfgDir, err := configDir(configPath)
	if err != nil {
		return "", err
	}
	return normalizeExistingPath(filepath.Join(cfgDir, source)), nil
}

func HasSource(cfg Config, configPath, source string) bool {
	source = normalizeConfigSource(configPath, source)
	for _, existing := range cfg.Sources {
		if normalizeConfigSource(configPath, existing) == source {
			return true
		}
	}
	return false
}

func AddSource(cfg *Config, configPath, source string) bool {
	source = filepath.Clean(source)
	normalized := normalizeConfigSource(configPath, source)
	next := cfg.Sources[:0]
	changed := false
	found := false
	for _, existing := range cfg.Sources {
		if normalizeConfigSource(configPath, existing) == normalized {
			if !found {
				next = append(next, source)
				found = true
				changed = changed || existing != source
			} else {
				changed = true
			}
			continue
		}
		next = append(next, existing)
	}
	if !found {
		next = append(next, source)
		changed = true
	}
	cfg.Sources = next
	return changed
}

func RemoveSource(cfg *Config, configPath, source string) bool {
	source = normalizeConfigSource(configPath, source)
	next := cfg.Sources[:0]
	removed := false
	for _, existing := range cfg.Sources {
		if normalizeConfigSource(configPath, existing) == source {
			removed = true
			continue
		}
		next = append(next, existing)
	}
	cfg.Sources = next
	return removed
}

func configDir(configPath string) (string, error) {
	dir, err := filepath.Abs(filepath.Dir(configPath))
	if err != nil {
		return "", err
	}
	return normalizeExistingPath(dir), nil
}

func normalizeUserSource(source string) (string, error) {
	if abs, err := filepath.Abs(source); err == nil {
		return normalizeExistingPath(abs), nil
	} else {
		return "", err
	}
}

func normalizeConfigSource(configPath, source string) string {
	resolved, err := ResolveSource(configPath, source)
	if err != nil {
		return filepath.Clean(source)
	}
	return resolved
}

func normalizeExistingPath(source string) string {
	if resolved, err := filepath.EvalSymlinks(source); err == nil {
		source = resolved
	}
	return filepath.Clean(source)
}

func DatabasePath(configPath string, cfg Config) string {
	if filepath.IsAbs(cfg.Database) {
		return cfg.Database
	}
	return filepath.Join(filepath.Dir(configPath), cfg.Database)
}

func firstEnv(names ...string) string {
	for _, name := range names {
		if v := os.Getenv(name); v != "" {
			return v
		}
	}
	return ""
}

func dataRoot() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return "."
	}
	switch runtime.GOOS {
	case "darwin":
		return filepath.Join(home, "Library", "Application Support", "sqlmem")
	case "windows":
		if appData := os.Getenv("APPDATA"); appData != "" {
			return filepath.Join(appData, "sqlmem")
		}
		return filepath.Join(home, "AppData", "Roaming", "sqlmem")
	default:
		if xdg := os.Getenv("XDG_DATA_HOME"); xdg != "" {
			return filepath.Join(xdg, "sqlmem")
		}
		return filepath.Join(home, ".local", "share", "sqlmem")
	}
}

func applyDefaults(data []byte, cfg *Config) {
	def := Default()
	presence := fieldPresence(data)
	if cfg.Database == "" {
		cfg.Database = def.Database
	}
	if cfg.Extensions == nil {
		cfg.Extensions = def.Extensions
	}
	if cfg.ExtensionVersions == nil {
		cfg.ExtensionVersions = def.ExtensionVersions
	}
	if cfg.Sources == nil {
		cfg.Sources = []string{}
	}
	if cfg.Embedding.Provider == "" {
		cfg.Embedding.Provider = "local"
	}
	if !presence.PDF["enabled"] {
		cfg.PDF.Enabled = def.PDF.Enabled
	}
	if cfg.PDF.Provider == "" {
		cfg.PDF.Provider = "local"
	}
	if !presence.Options["max_tokens"] {
		cfg.Options.MaxTokens = def.Options.MaxTokens
	}
	if !presence.Options["overlay_tokens"] {
		cfg.Options.OverlayTokens = def.Options.OverlayTokens
	}
	if !presence.Options["max_results"] {
		cfg.Options.MaxResults = def.Options.MaxResults
	}
	if !presence.Options["min_score"] {
		cfg.Options.MinScore = def.Options.MinScore
	}
	if !presence.Options["vector_weight"] {
		cfg.Options.VectorWeight = def.Options.VectorWeight
	}
	if !presence.Options["text_weight"] {
		cfg.Options.TextWeight = def.Options.TextWeight
	}
	if !presence.Options["search_oversample"] {
		cfg.Options.SearchOversample = def.Options.SearchOversample
	}
	if !presence.Options["extensions"] {
		cfg.Options.Extensions = def.Options.Extensions
	}
	if !presence.Options["embedding_cache"] {
		cfg.Options.EmbeddingCache = def.Options.EmbeddingCache
	}
	if !presence.Options["cache_max_entries"] {
		cfg.Options.CacheMaxEntries = def.Options.CacheMaxEntries
	}
	if cfg.CreatedAt == "" {
		cfg.CreatedAt = def.CreatedAt
	}
	if cfg.UpdatedAt == "" {
		cfg.UpdatedAt = def.UpdatedAt
	}
}

type jsonPresence struct {
	Options map[string]bool
	PDF     map[string]bool
}

func fieldPresence(data []byte) jsonPresence {
	presence := jsonPresence{
		Options: map[string]bool{},
		PDF:     map[string]bool{},
	}
	var raw struct {
		Options map[string]json.RawMessage `json:"options"`
		PDF     map[string]json.RawMessage `json:"pdf"`
	}
	if err := json.Unmarshal(data, &raw); err != nil {
		return presence
	}
	for key := range raw.Options {
		presence.Options[key] = true
	}
	for key := range raw.PDF {
		presence.PDF[key] = true
	}
	return presence
}

func parseValue(raw string, existing any) any {
	var parsed any
	if err := json.Unmarshal([]byte(raw), &parsed); err == nil {
		return parsed
	}
	switch reflect.TypeOf(existing).Kind() {
	case reflect.Bool:
		if v, err := strconv.ParseBool(raw); err == nil {
			return v
		}
	case reflect.Float64:
		if v, err := strconv.ParseFloat(raw, 64); err == nil {
			return v
		}
	case reflect.Int:
		if v, err := strconv.Atoi(raw); err == nil {
			return v
		}
	}
	return raw
}

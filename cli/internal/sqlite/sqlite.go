package sqlite

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"

	sqlite3 "github.com/mattn/go-sqlite3"
	"github.com/sqliteai/sqlite-memory/cli/internal/config"
	"github.com/sqliteai/sqlite-memory/cli/internal/download"
)

var driverSeq atomic.Uint64

type ExtensionPaths struct {
	Vector string
	Memory string
	Sync   string
}

type OpenOptions struct {
	Config        config.Config
	ConfigPath    string
	ExtensionsDir string
	SkipLoad      bool
}

func Open(ctx context.Context, opts OpenOptions) (*sql.DB, error) {
	dbPath := config.DatabasePath(opts.ConfigPath, opts.Config)
	if err := os.MkdirAll(filepath.Dir(dbPath), 0755); err != nil {
		return nil, err
	}
	driverName := "sqlite3"
	if !opts.SkipLoad {
		paths, err := ResolveExtensions(opts.Config, opts.ExtensionsDir)
		if err != nil {
			return nil, err
		}
		driverName = registerExtensionDriver(paths)
	}
	db, err := sql.Open(driverName, dbPath)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	if err := db.PingContext(ctx); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

func ResolveExtensions(cfg config.Config, dirOverride string) (ExtensionPaths, error) {
	base := config.ResolveExtensionsDir(cfg, dirOverride)
	vector, err := resolveOne(base, download.RepoVector, "vector", cfg)
	if err != nil {
		return ExtensionPaths{}, err
	}
	memory, err := resolveOne(base, download.RepoMemory, "memory", cfg)
	if err != nil {
		return ExtensionPaths{}, err
	}
	var syncPath string
	if cfg.Extensions["sync"] != "" {
		syncPath, err = resolveOne(base, download.RepoSync, "sync", cfg)
		if err != nil {
			return ExtensionPaths{}, err
		}
	}
	return ExtensionPaths{Vector: vector, Memory: memory, Sync: syncPath}, nil
}

func LoadExtensions(ctx context.Context, db *sql.DB, paths ExtensionPaths) error {
	return fmt.Errorf("extensions must be loaded while opening the database")
}

func registerExtensionDriver(paths ExtensionPaths) string {
	name := fmt.Sprintf("sqlite3_sqlmem_%d", driverSeq.Add(1))
	sql.Register(name, &sqlite3.SQLiteDriver{Extensions: extensionOrder(paths)})
	return name
}

func extensionOrder(paths ExtensionPaths) []string {
	order := []string{paths.Vector, paths.Memory}
	if paths.Sync != "" {
		order = append(order, paths.Sync)
	}
	return order
}

func resolveOne(base, repo, key string, cfg config.Config) (string, error) {
	if v := cfg.Extensions[key]; v != "" && v != "latest" {
		if filepath.IsAbs(v) || fileExists(v) {
			return v, nil
		}
	}
	version := cfg.ExtensionVersions[key]
	if version == "" {
		version = cfg.Extensions[key]
	}
	if version == "" {
		version = "latest"
	}
	lib, ok := download.FindSharedLibrary(filepath.Join(base, repo, version), repo, download.CurrentPlatform())
	if !ok && version == "latest" {
		lib, ok = findAnyInstalled(base, repo)
	}
	if !ok {
		return "", fmt.Errorf("%s extension not installed. Run `sqlmem extensions install %s`.", key, key)
	}
	return lib, nil
}

func findAnyInstalled(base, repo string) (string, bool) {
	root := filepath.Join(base, repo)
	entries, err := os.ReadDir(root)
	if err != nil {
		return "", false
	}
	sort.Slice(entries, func(i, j int) bool {
		return compareVersions(entries[i].Name(), entries[j].Name()) > 0
	})
	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		if lib, ok := download.FindSharedLibrary(filepath.Join(root, entry.Name()), repo, download.CurrentPlatform()); ok {
			return lib, true
		}
	}
	return "", false
}

func compareVersions(a, b string) int {
	an := versionNumbers(a)
	bn := versionNumbers(b)
	if len(an) == 0 || len(bn) == 0 {
		return strings.Compare(a, b)
	}
	for i := 0; i < len(an) || i < len(bn); i++ {
		av, bv := 0, 0
		if i < len(an) {
			av = an[i]
		}
		if i < len(bn) {
			bv = bn[i]
		}
		if av < bv {
			return -1
		}
		if av > bv {
			return 1
		}
	}
	return strings.Compare(a, b)
}

func versionNumbers(version string) []int {
	version = strings.TrimLeft(version, "vV")
	parts := strings.FieldsFunc(version, func(r rune) bool {
		return r < '0' || r > '9'
	})
	nums := make([]int, 0, len(parts))
	for _, part := range parts {
		if part == "" {
			continue
		}
		n, err := strconv.Atoi(part)
		if err != nil {
			return nil
		}
		nums = append(nums, n)
	}
	return nums
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

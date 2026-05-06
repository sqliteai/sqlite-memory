package sqlite

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/sqliteai/sqlite-memory/cli/internal/config"
	"github.com/sqliteai/sqlite-memory/cli/internal/download"
)

func TestResolveExtensionsLatestFindsInstalledTag(t *testing.T) {
	if download.CurrentPlatform().SharedLibraryExt() != ".dylib" && download.CurrentPlatform().SharedLibraryExt() != ".so" && download.CurrentPlatform().SharedLibraryExt() != ".dll" {
		t.Skip("unsupported platform")
	}
	dir := t.TempDir()
	ext := download.CurrentPlatform().SharedLibraryExt()
	vector := filepath.Join(dir, download.RepoVector, "v1.2.3", "vector"+ext)
	memory := filepath.Join(dir, download.RepoMemory, "v1.2.3", "memory"+ext)
	for _, path := range []string{vector, memory} {
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("x"), 0644); err != nil {
			t.Fatal(err)
		}
	}
	cfg := config.Default()
	paths, err := ResolveExtensions(cfg, dir)
	if err != nil {
		t.Fatal(err)
	}
	if paths.Vector != vector || paths.Memory != memory {
		t.Fatalf("paths = %#v", paths)
	}
}

func TestFindAnyInstalledPrefersNewestSemverTag(t *testing.T) {
	dir := t.TempDir()
	ext := download.CurrentPlatform().SharedLibraryExt()
	oldPath := filepath.Join(dir, download.RepoMemory, "v1.9.0", "memory"+ext)
	newPath := filepath.Join(dir, download.RepoMemory, "v1.10.0", "memory"+ext)
	for _, path := range []string{oldPath, newPath} {
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("x"), 0644); err != nil {
			t.Fatal(err)
		}
	}

	got, ok := findAnyInstalled(dir, download.RepoMemory)
	if !ok {
		t.Fatal("no installed extension found")
	}
	if got != newPath {
		t.Fatalf("installed extension = %q, want %q", got, newPath)
	}
}

package memory

import (
	"path/filepath"
	"reflect"
	"testing"

	"github.com/sqliteai/sqlite-memory/cli/internal/config"
)

func TestResolveModelLocalWithoutAPIKey(t *testing.T) {
	cfg := config.Default()
	cfg.Embedding.Model = "/models/local.gguf"
	got := ResolveModel(cfg, ModelOptions{})
	if got.Provider != "local" || got.Model != "/models/local.gguf" {
		t.Fatalf("model = %#v", got)
	}
}

func TestResolveModelRemoteWithAPIKey(t *testing.T) {
	cfg := config.Default()
	got := ResolveModel(cfg, ModelOptions{APIKey: "key"})
	if got.Provider != "openai" {
		t.Fatalf("provider = %q", got.Provider)
	}
	if got.Model != defaultRemoteModel {
		t.Fatalf("model = %q", got.Model)
	}
}

func TestPathCandidatesAbsolutePathIncludesStoredBasename(t *testing.T) {
	path := filepath.Join(t.TempDir(), "docs", "readme.md")
	want := []string{filepath.ToSlash(filepath.Clean(path)), "readme.md"}
	if got := pathCandidates(path); !reflect.DeepEqual(got, want) {
		t.Fatalf("pathCandidates() = %#v, want %#v", got, want)
	}
}

func TestPathCandidatesRelativePathPreservesDirectory(t *testing.T) {
	want := []string{"docs/readme.md"}
	if got := pathCandidates(filepath.Join("docs", "readme.md")); !reflect.DeepEqual(got, want) {
		t.Fatalf("pathCandidates() = %#v, want %#v", got, want)
	}
}

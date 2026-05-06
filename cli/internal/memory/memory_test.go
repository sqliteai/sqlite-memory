package memory

import (
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

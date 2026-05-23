package memory

import (
	"context"
	"database/sql"
	"testing"

	_ "github.com/mattn/go-sqlite3"

	"github.com/sqliteai/sqlite-memory/cli/internal/config"
)

func openTestDB(t *testing.T) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite3", ":memory:")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { db.Close() })
	_, err = db.Exec(`CREATE TABLE dbmem_content (
		hash TEXT PRIMARY KEY,
		path TEXT NOT NULL,
		context TEXT,
		value TEXT,
		created_at INTEGER NOT NULL DEFAULT 0,
		last_accessed INTEGER NOT NULL DEFAULT 0
	)`)
	if err != nil {
		t.Fatal(err)
	}
	return db
}

func TestGetReturnsFullContent(t *testing.T) {
	db := openTestDB(t)
	ctx := context.Background()
	_, err := db.ExecContext(ctx, `INSERT INTO dbmem_content (hash, path, context, value, created_at, last_accessed)
		VALUES ('abc123', '/docs/test.md', 'test-ctx', 'hello world', 1000, 2000)`)
	if err != nil {
		t.Fatal(err)
	}
	out, err := Get(ctx, db, "abc123")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"abc123", "/docs/test.md", "test-ctx", "hello world"} {
		if !contains(out, want) {
			t.Errorf("output missing %q:\n%s", want, out)
		}
	}
}

func TestGetNotFound(t *testing.T) {
	db := openTestDB(t)
	_, err := Get(context.Background(), db, "nope")
	if err == nil {
		t.Fatal("expected error for missing hash")
	}
}

func TestQuerySelectWorks(t *testing.T) {
	db := openTestDB(t)
	ctx := context.Background()
	_, err := db.ExecContext(ctx, `INSERT INTO dbmem_content (hash, path, value, created_at, last_accessed)
		VALUES ('h1', '/a.md', 'content', 0, 0)`)
	if err != nil {
		t.Fatal(err)
	}
	out, err := Query(ctx, db, "SELECT hash, path FROM dbmem_content")
	if err != nil {
		t.Fatal(err)
	}
	if !contains(out, "h1") || !contains(out, "/a.md") {
		t.Errorf("unexpected output: %s", out)
	}
}

func TestQueryRejectsWrites(t *testing.T) {
	db := openTestDB(t)
	_, err := Query(context.Background(), db, "INSERT INTO dbmem_content (hash, path, created_at, last_accessed) VALUES ('x', '/x', 0, 0)")
	if err == nil {
		t.Fatal("expected error for write statement under query_only")
	}
}

func contains(s, sub string) bool {
	return len(s) >= len(sub) && (s == sub || len(sub) == 0 ||
		func() bool {
			for i := 0; i <= len(s)-len(sub); i++ {
				if s[i:i+len(sub)] == sub {
					return true
				}
			}
			return false
		}())
}

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

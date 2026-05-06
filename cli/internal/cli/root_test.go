package cli

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/sqliteai/sqlite-memory/cli/internal/config"
)

func TestStatusReturnsOpenError(t *testing.T) {
	dir := t.TempDir()
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
	cfg := config.Default()
	cfg.Extensions["vector"] = filepath.Join(dir, "missing-vector")
	cfg.Extensions["memory"] = filepath.Join(dir, "missing-memory")
	if err := config.Save(config.FileName, cfg); err != nil {
		t.Fatal(err)
	}

	cmd := NewRootCommand()
	cmd.SetArgs([]string{"status"})
	cmd.SetOut(&bytes.Buffer{})
	if err := cmd.Execute(); err == nil {
		t.Fatal("status returned nil error for missing extensions")
	}
}

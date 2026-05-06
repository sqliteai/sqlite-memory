package cli

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/chzyer/readline"
	"github.com/spf13/cobra"
	"github.com/sqliteai/sqlite-memory/cli/internal/config"
	"github.com/sqliteai/sqlite-memory/cli/internal/download"
	"github.com/sqliteai/sqlite-memory/cli/internal/mcp"
	"github.com/sqliteai/sqlite-memory/cli/internal/memory"
	"github.com/sqliteai/sqlite-memory/cli/internal/output"
	"github.com/sqliteai/sqlite-memory/cli/internal/pdf"
	sqlitemem "github.com/sqliteai/sqlite-memory/cli/internal/sqlite"
	watchpkg "github.com/sqliteai/sqlite-memory/cli/internal/watch"
)

type globalFlags struct {
	extensionsDir string
	pdfCacheDir   string
	apiKey        string
	provider      string
	model         string
}

func NewRootCommand() *cobra.Command {
	flags := &globalFlags{}
	root := &cobra.Command{
		Use:           "sqlmem",
		Short:         "Manage SQLite Memory databases backed by Markdown documents",
		SilenceUsage:  true,
		SilenceErrors: true,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runInteractive(flags)
		},
	}
	root.PersistentFlags().StringVar(&flags.extensionsDir, "extensions-dir", "", "SQLite extension cache directory")
	root.PersistentFlags().StringVar(&flags.pdfCacheDir, "pdf-cache-dir", "", "PDF markdown cache directory")
	root.PersistentFlags().StringVar(&flags.apiKey, "api-key", "", "vectors.space API key")
	root.PersistentFlags().StringVar(&flags.provider, "provider", "", "embedding provider")
	root.PersistentFlags().StringVar(&flags.model, "model", "", "embedding model")

	root.AddCommand(
		initCmd(flags),
		addCmd(flags),
		searchCmd(flags),
		watchCmd(flags),
		mcpCmd(flags),
		configCmd(),
		removeCmd(),
		clearCmd(flags),
		reindexCmd(flags),
		statusCmd(flags),
		resetCmd(),
		extensionsCmd(flags),
	)
	return root
}

func initCmd(flags *globalFlags) *cobra.Command {
	return &cobra.Command{
		Use:   "init",
		Short: "Initialize a sqlmem project",
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfgPath := filepath.Join(".", config.FileName)
			if _, err := os.Stat(cfgPath); err == nil {
				return fmt.Errorf("%s already exists", config.FileName)
			}
			cfg := config.Default()
			if flags.extensionsDir != "" {
				cfg.ExtensionsDir = flags.extensionsDir
			}
			if flags.apiKey != "" {
				cfg.Embedding.APIKey = flags.apiKey
			}
			if flags.provider != "" {
				cfg.Embedding.Provider = flags.provider
			}
			if flags.model != "" {
				cfg.Embedding.Model = flags.model
			}
			ctx := cmd.Context()
			if err := installRequiredExtensions(ctx, cfg, flags.extensionsDir, []string{"vector", "memory"}); err != nil {
				return err
			}
			db, err := sqlitemem.Open(ctx, sqlitemem.OpenOptions{Config: cfg, ConfigPath: cfgPath, ExtensionsDir: flags.extensionsDir})
			if err != nil {
				return err
			}
			defer db.Close()
			if err := memory.Configure(ctx, db, cfg, memory.ModelOptions{Provider: flags.provider, Model: flags.model, APIKey: flags.apiKey}); err != nil {
				return err
			}
			if err := config.Save(cfgPath, cfg); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Initialized sqlmem project in %d ms\n", elapsedMS(start))
			return nil
		},
	}
}

func addCmd(flags *globalFlags) *cobra.Command {
	var sources []string
	var contextLabel string
	cmd := &cobra.Command{
		Use:   "add [path...]",
		Short: "Add files or directories",
		RunE: func(cmd *cobra.Command, args []string) error {
			all := append([]string{}, args...)
			all = append(all, sources...)
			if len(all) == 0 {
				return fmt.Errorf("no sources provided")
			}
			ctx := cmd.Context()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			db, err := openConfigured(ctx, cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			for _, source := range all {
				start := time.Now()
				storedSource, indexSource, err := config.NormalizeSource(cfgPath, source)
				if err != nil {
					return err
				}
				if config.HasSource(cfg, cfgPath, storedSource) {
					if config.AddSource(&cfg, cfgPath, storedSource) {
						if err := config.Save(cfgPath, cfg); err != nil {
							return err
						}
					}
					fmt.Fprintf(cmd.OutOrStdout(), "Source already added: %s in %d ms\n", storedSource, elapsedMS(start))
					continue
				}
				spin := output.NewSpinner("indexing")
				spin.Start()
				err = addSource(ctx, db, cfg, flags, indexSource, contextLabel)
				spin.Stop()
				if err != nil {
					return err
				}
				config.AddSource(&cfg, cfgPath, storedSource)
				if err := config.Save(cfgPath, cfg); err != nil {
					return err
				}
				fmt.Fprintf(cmd.OutOrStdout(), "Added source: %s in %d ms\n", storedSource, elapsedMS(start))
			}
			return nil
		},
	}
	cmd.Flags().StringArrayVarP(&sources, "source", "s", nil, "source file or directory")
	cmd.Flags().StringVar(&contextLabel, "context", "", "context label")
	return cmd
}

func searchCmd(flags *globalFlags) *cobra.Command {
	var query string
	var limit int
	var jsonOut bool
	cmd := &cobra.Command{
		Use:   "search [query]",
		Short: "Search memory",
		RunE: func(cmd *cobra.Command, args []string) error {
			if query == "" {
				query = strings.Join(args, " ")
			}
			if query == "" {
				return fmt.Errorf("query required")
			}
			ctx := cmd.Context()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			db, err := openConfigured(ctx, cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			start := time.Now()
			spin := output.NewSpinner("searching")
			spin.Start()
			results, err := memory.Search(ctx, db, query, limit)
			spin.Stop()
			elapsed := elapsedMS(start)
			if err != nil {
				return err
			}
			if jsonOut {
				fmt.Fprintln(cmd.OutOrStdout(), memory.ResultsJSON(results))
				fmt.Fprintf(cmd.ErrOrStderr(), "Search returned %d results in %d ms\n", len(results), elapsed)
			} else {
				for _, r := range results {
					fmt.Fprintln(cmd.OutOrStdout(), memory.FormatResult(r))
					fmt.Fprintln(cmd.OutOrStdout())
				}
				fmt.Fprintf(cmd.OutOrStdout(), "Search returned %d results in %d ms\n", len(results), elapsed)
			}
			return nil
		},
	}
	cmd.Flags().StringVarP(&query, "query", "q", "", "search query")
	cmd.Flags().IntVar(&limit, "limit", 0, "result limit")
	cmd.Flags().BoolVar(&jsonOut, "json", false, "emit JSON")
	return cmd
}

func watchCmd(flags *globalFlags) *cobra.Command {
	var debounce time.Duration
	cmd := &cobra.Command{
		Use:   "watch",
		Short: "Watch configured sources",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			sources := []string{}
			for _, source := range cfg.Sources {
				resolved, err := config.ResolveSource(cfgPath, source)
				if err != nil {
					return err
				}
				sources = append(sources, resolved)
			}
			if len(args) > 0 {
				sources = []string{}
				for _, source := range args {
					_, resolved, err := config.NormalizeSource(cfgPath, source)
					if err != nil {
						return err
					}
					sources = append(sources, resolved)
				}
			}
			if len(sources) == 0 {
				return fmt.Errorf("no sources configured")
			}
			ctx := cmd.Context()
			db, err := openConfigured(ctx, cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			fmt.Fprintf(cmd.OutOrStdout(), "Watching %d sources\n", len(sources))
			return watchpkg.Run(ctx, sources, debounce, func(ctx context.Context, path string, removed bool) error {
				if removed {
					return removeIndexedSource(ctx, db, cfg, flags, path)
				}
				return addSource(ctx, db, cfg, flags, path, "")
			})
		},
	}
	cmd.Flags().DurationVar(&debounce, "debounce", 500*time.Millisecond, "debounce duration")
	return cmd
}

func mcpCmd(flags *globalFlags) *cobra.Command {
	var transport string
	var addr string
	cmd := &cobra.Command{
		Use:   "mcp",
		Short: "Start MCP server",
		RunE: func(cmd *cobra.Command, args []string) error {
			ctx := cmd.Context()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			db, err := openConfigured(ctx, cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			server := mcp.Server{DB: db}
			fmt.Fprintln(cmd.ErrOrStderr(), "MCP server started")
			if transport == "http" {
				return server.ServeHTTP(ctx, addr)
			}
			return server.ServeStdio(ctx, os.Stdin, os.Stdout)
		},
	}
	cmd.Flags().StringVar(&transport, "transport", "stdio", "stdio or http")
	cmd.Flags().StringVar(&addr, "addr", "127.0.0.1:8765", "HTTP listen address")
	return cmd
}

func configCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "config",
		Short: "Show config",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, _, err := loadConfig()
			if err != nil {
				return err
			}
			data, _ := json.MarshalIndent(cfg, "", "  ")
			fmt.Fprintln(cmd.OutOrStdout(), string(data))
			return nil
		},
	}
	cmd.AddCommand(&cobra.Command{
		Use:   "set KEY VALUE",
		Short: "Set config value",
		Args:  cobra.ExactArgs(2),
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			if err := config.SetDot(&cfg, args[0], args[1]); err != nil {
				return err
			}
			if err := config.Save(cfgPath, cfg); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Updated config: %s in %d ms\n", args[0], elapsedMS(start))
			return nil
		},
	})
	return cmd
}

func removeCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "remove SOURCE",
		Short: "Remove a configured source",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			source, _, err := config.NormalizeSource(cfgPath, args[0])
			if err != nil {
				return err
			}
			if !config.RemoveSource(&cfg, cfgPath, source) {
				return fmt.Errorf("source not found: %s", source)
			}
			if err := config.Save(cfgPath, cfg); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Removed source: %s in %d ms\n", source, elapsedMS(start))
			return nil
		},
	}
}

func clearCmd(flags *globalFlags) *cobra.Command {
	return &cobra.Command{
		Use:   "clear",
		Short: "Clear memory",
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			db, err := openConfigured(cmd.Context(), cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			if err := memory.Clear(cmd.Context(), db); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Cleared memory in %d ms\n", elapsedMS(start))
			return nil
		},
	}
}

func reindexCmd(flags *globalFlags) *cobra.Command {
	return &cobra.Command{
		Use:   "reindex",
		Short: "Reindex memory",
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			db, err := openConfigured(cmd.Context(), cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			if err := memory.Reindex(cmd.Context(), db); err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "Reindexed memory in %d ms\n", elapsedMS(start))
			return nil
		},
	}
}

func statusCmd(flags *globalFlags) *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "Show project status",
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			fmt.Fprintf(cmd.OutOrStdout(), "db: %s\n", config.DatabasePath(cfgPath, cfg))
			fmt.Fprintf(cmd.OutOrStdout(), "sources: %d\n", len(cfg.Sources))
			model := memory.ResolveModel(cfg, memory.ModelOptions{Provider: flags.provider, Model: flags.model, APIKey: flags.apiKey})
			fmt.Fprintf(cmd.OutOrStdout(), "embedding: %s %s\n", model.Provider, model.Model)
			fmt.Fprintf(cmd.OutOrStdout(), "pdf: enabled=%v cache=%s\n", cfg.PDF.Enabled, config.ResolvePDFCacheDir(cfg, flags.pdfCacheDir))
			db, err := openConfigured(cmd.Context(), cfg, cfgPath, flags)
			if err != nil {
				return err
			}
			defer db.Close()
			st, _ := memory.Status(cmd.Context(), db)
			fmt.Fprintf(cmd.OutOrStdout(), "memories: %v\nchunks: %v\n", st["memories"], st["chunks"])
			fmt.Fprintf(cmd.OutOrStdout(), "Status collected in %d ms\n", elapsedMS(start))
			return nil
		},
	}
}

func resetCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "reset",
		Short: "Reset project",
		RunE: func(cmd *cobra.Command, args []string) error {
			start := time.Now()
			cfg, cfgPath, err := loadConfig()
			if err != nil {
				return err
			}
			_ = os.Remove(config.DatabasePath(cfgPath, cfg))
			_ = os.Remove(cfgPath)
			fmt.Fprintf(cmd.OutOrStdout(), "Reset sqlmem project in %d ms\n", elapsedMS(start))
			return nil
		},
	}
}

func extensionsCmd(flags *globalFlags) *cobra.Command {
	cmd := &cobra.Command{Use: "extensions", Short: "Manage SQLite extensions"}
	cmd.AddCommand(&cobra.Command{
		Use:   "path",
		Short: "Show extension cache path",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, _, _ := loadConfig()
			fmt.Fprintln(cmd.OutOrStdout(), config.ResolveExtensionsDir(cfg, flags.extensionsDir))
			return nil
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "list",
		Short: "List installed extensions",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, _, _ := loadConfig()
			base := config.ResolveExtensionsDir(cfg, flags.extensionsDir)
			return filepath.WalkDir(base, func(path string, d os.DirEntry, err error) error {
				if err != nil || d.IsDir() {
					return nil
				}
				fmt.Fprintln(cmd.OutOrStdout(), path)
				return nil
			})
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "install [vector|memory|sync]",
		Short: "Install extensions",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, _, _ := loadConfig()
			names := args
			if len(names) == 0 {
				names = []string{"vector", "memory"}
			}
			return installRequiredExtensions(cmd.Context(), cfg, flags.extensionsDir, names)
		},
	})
	cmd.AddCommand(&cobra.Command{
		Use:   "update [vector|memory|sync]",
		Short: "Update extensions",
		RunE: func(cmd *cobra.Command, args []string) error {
			cfg, _, _ := loadConfig()
			names := args
			if len(names) == 0 {
				names = []string{"vector", "memory"}
			}
			return installRequiredExtensions(cmd.Context(), cfg, flags.extensionsDir, names)
		},
	})
	return cmd
}

func loadConfig() (config.Config, string, error) {
	cfg, path, err := config.LoadFrom(".")
	if err != nil {
		if errors.Is(err, config.ErrNotFound) {
			return config.Config{}, "", config.ErrNotFound
		}
		return config.Config{}, "", err
	}
	return cfg, path, nil
}

func openConfigured(ctx context.Context, cfg config.Config, cfgPath string, flags *globalFlags) (*sql.DB, error) {
	db, err := sqlitemem.Open(ctx, sqlitemem.OpenOptions{Config: cfg, ConfigPath: cfgPath, ExtensionsDir: flags.extensionsDir})
	if err != nil {
		return nil, err
	}
	if err := memory.Configure(ctx, db, cfg, memory.ModelOptions{Provider: flags.provider, Model: flags.model, APIKey: flags.apiKey}); err != nil {
		db.Close()
		return nil, err
	}
	return db, nil
}

func addSource(ctx context.Context, db *sql.DB, cfg config.Config, flags *globalFlags, source, contextLabel string) error {
	info, err := os.Stat(source)
	if err != nil {
		return err
	}
	if info.IsDir() {
		return memory.AddDirectory(ctx, db, source, contextLabel)
	}
	if strings.EqualFold(filepath.Ext(source), ".pdf") {
		if !cfg.PDF.Enabled {
			return fmt.Errorf("PDF support disabled")
		}
		cache := pdf.Cache{
			Dir:   config.ResolvePDFCacheDir(cfg, flags.pdfCacheDir),
			Force: cfg.PDF.Force,
		}
		result, err := cache.Process(ctx, source)
		if err != nil {
			return err
		}
		if contextLabel == "" {
			contextLabel = source
		}
		return memory.AddFile(ctx, db, result.IndexPath, contextLabel)
	}
	return memory.AddFile(ctx, db, source, contextLabel)
}

func removeIndexedSource(ctx context.Context, db *sql.DB, cfg config.Config, flags *globalFlags, source string) error {
	path := source
	if strings.EqualFold(filepath.Ext(source), ".pdf") {
		path = pdf.IndexPathForSource(config.ResolvePDFCacheDir(cfg, flags.pdfCacheDir), source)
	}
	return memory.DeletePath(ctx, db, path)
}

func installRequiredExtensions(ctx context.Context, cfg config.Config, override string, names []string) error {
	base := config.ResolveExtensionsDir(cfg, override)
	client := download.Client{}
	for _, name := range names {
		start := time.Now()
		repo, ok := map[string]string{"vector": download.RepoVector, "memory": download.RepoMemory, "sync": download.RepoSync}[name]
		if !ok {
			return fmt.Errorf("unknown extension %s", name)
		}
		version := cfg.ExtensionVersions[name]
		if version == "" {
			version = cfg.Extensions[name]
		}
		if version == "" {
			version = "latest"
		}
		path, err := client.Install(ctx, "sqliteai", repo, version, base)
		if err != nil {
			return err
		}
		fmt.Printf("Installed %s: %s in %d ms\n", name, path, elapsedMS(start))
	}
	return nil
}

func elapsedMS(start time.Time) int64 {
	return time.Since(start).Milliseconds()
}

func runInteractive(flags *globalFlags) error {
	history := filepath.Join(config.DefaultExtensionsDir(), "..", "history")
	if err := os.MkdirAll(filepath.Dir(history), 0755); err != nil {
		return err
	}
	rl, err := readline.NewEx(&readline.Config{
		Prompt:      "sqlmem> ",
		HistoryFile: history,
	})
	if err != nil {
		return err
	}
	defer rl.Close()
	for {
		line, err := rl.Readline()
		if errors.Is(err, readline.ErrInterrupt) {
			continue
		}
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if line == "exit" || line == "quit" {
			return nil
		}
		args := strings.Fields(line)
		cmd := NewRootCommand()
		cmd.SetArgs(args)
		if err := cmd.Execute(); err != nil {
			fmt.Fprintln(os.Stderr, err)
		}
	}
}

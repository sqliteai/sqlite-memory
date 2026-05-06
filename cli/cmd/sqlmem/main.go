package main

import (
	"fmt"
	"os"

	sqlmemcli "github.com/sqliteai/sqlite-memory/cli/internal/cli"
)

func main() {
	if err := sqlmemcli.NewRootCommand().Execute(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

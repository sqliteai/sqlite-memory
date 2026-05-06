package watch

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDebouncer(t *testing.T) {
	done := make(chan []event, 1)
	d := NewDebouncer(20 * time.Millisecond)
	d.Trigger("b.md", false, func(events []event) { done <- events })
	d.Trigger("a.md", true, func(events []event) { done <- events })
	select {
	case events := <-done:
		if len(events) != 2 {
			t.Fatalf("events = %#v", events)
		}
		if events[0].Path != "b.md" || events[0].Remove {
			t.Fatalf("first event = %#v", events[0])
		}
		if events[1].Path != "a.md" || !events[1].Remove {
			t.Fatalf("second event = %#v", events[1])
		}
	case <-time.After(100 * time.Millisecond):
		t.Fatal("debouncer did not fire")
	}
}

func TestRunReturnsHandlerError(t *testing.T) {
	dir := t.TempDir()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	want := errors.New("handler failed")
	done := make(chan error, 1)
	go func() {
		done <- Run(ctx, []string{dir}, 5*time.Millisecond, func(context.Context, string, bool) error {
			return want
		})
	}()

	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	deadline := time.After(2 * time.Second)
	for {
		select {
		case err := <-done:
			if !errors.Is(err, want) {
				t.Fatalf("Run error = %v, want %v", err, want)
			}
			return
		case <-ticker.C:
			path := filepath.Join(dir, "file.md")
			if err := os.WriteFile(path, []byte(time.Now().String()), 0644); err != nil {
				t.Fatal(err)
			}
		case <-deadline:
			cancel()
			t.Fatal("Run did not return handler error")
		}
	}
}

func TestSourceFilterAllowsOnlyConfiguredFile(t *testing.T) {
	dir := t.TempDir()
	source := filepath.Join(dir, "source.md")
	sibling := filepath.Join(dir, "sibling.md")
	if err := os.WriteFile(source, []byte("source"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sibling, []byte("sibling"), 0644); err != nil {
		t.Fatal(err)
	}
	filter, err := newSourceFilter([]string{source})
	if err != nil {
		t.Fatal(err)
	}
	if !filter.Allows(source) {
		t.Fatal("configured file was not allowed")
	}
	if filter.Allows(sibling) {
		t.Fatal("sibling file was allowed")
	}
}

func TestSourceFilterAllowsDirectoryChildren(t *testing.T) {
	dir := t.TempDir()
	child := filepath.Join(dir, "child.md")
	if err := os.WriteFile(child, []byte("child"), 0644); err != nil {
		t.Fatal(err)
	}
	filter, err := newSourceFilter([]string{dir})
	if err != nil {
		t.Fatal(err)
	}
	if !filter.Allows(child) {
		t.Fatal("directory child was not allowed")
	}
}

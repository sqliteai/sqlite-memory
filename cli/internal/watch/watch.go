package watch

import (
	"context"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/fsnotify/fsnotify"
)

type Handler func(context.Context, string, bool) error

type event struct {
	Path    string
	Remove  bool
	Ordinal int
}

type Debouncer struct {
	Delay   time.Duration
	mu      sync.Mutex
	timer   *time.Timer
	next    int
	pending map[string]event
}

func NewDebouncer(delay time.Duration) *Debouncer {
	return &Debouncer{Delay: delay, pending: map[string]event{}}
}

func (d *Debouncer) Trigger(path string, remove bool, fn func([]event)) {
	d.mu.Lock()
	d.next++
	d.pending[path] = event{Path: path, Remove: remove, Ordinal: d.next}
	if d.timer != nil {
		d.timer.Stop()
	}
	d.timer = time.AfterFunc(d.Delay, func() {
		d.mu.Lock()
		events := make([]event, 0, len(d.pending))
		for _, ev := range d.pending {
			events = append(events, ev)
		}
		d.pending = map[string]event{}
		d.mu.Unlock()
		sort.Slice(events, func(i, j int) bool {
			return events[i].Ordinal < events[j].Ordinal
		})
		fn(events)
	})
	d.mu.Unlock()
}

func Run(ctx context.Context, sources []string, delay time.Duration, handler Handler) error {
	w, err := fsnotify.NewWatcher()
	if err != nil {
		return err
	}
	defer w.Close()
	filter, err := newSourceFilter(sources)
	if err != nil {
		return err
	}
	for _, source := range sources {
		if err := addSource(w, source); err != nil {
			return err
		}
	}
	debounce := NewDebouncer(delay)
	errs := make(chan error, 1)
	reportErr := func(err error) {
		if err == nil {
			return
		}
		select {
		case errs <- err:
		default:
		}
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case err := <-errs:
			return err
		case err := <-w.Errors:
			if err != nil {
				return err
			}
		case ev := <-w.Events:
			if ev.Op&(fsnotify.Write|fsnotify.Create|fsnotify.Rename|fsnotify.Remove) == 0 {
				continue
			}
			if !filter.Allows(ev.Name) {
				continue
			}
			path := ev.Name
			remove := ev.Op&(fsnotify.Rename|fsnotify.Remove) != 0
			debounce.Trigger(path, remove, func(events []event) {
				for _, ev := range events {
					if err := handler(ctx, ev.Path, ev.Remove); err != nil {
						reportErr(err)
						continue
					}
					if !ev.Remove {
						if info, err := os.Stat(ev.Path); err == nil && info.IsDir() {
							reportErr(addSource(w, ev.Path))
						}
					}
				}
			})
		}
	}
}

type sourceFilter struct {
	files map[string]struct{}
	dirs  []string
}

func newSourceFilter(sources []string) (sourceFilter, error) {
	filter := sourceFilter{files: map[string]struct{}{}}
	for _, source := range sources {
		info, err := os.Stat(source)
		if err != nil {
			return sourceFilter{}, err
		}
		path, err := filepath.Abs(source)
		if err != nil {
			return sourceFilter{}, err
		}
		path = filepath.Clean(path)
		if info.IsDir() {
			filter.dirs = append(filter.dirs, path)
		} else {
			filter.files[path] = struct{}{}
		}
	}
	return filter, nil
}

func (f sourceFilter) Allows(path string) bool {
	path, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	path = filepath.Clean(path)
	if _, ok := f.files[path]; ok {
		return true
	}
	for _, dir := range f.dirs {
		if path == dir {
			return true
		}
		rel, err := filepath.Rel(dir, path)
		if err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(os.PathSeparator)) && !filepath.IsAbs(rel) {
			return true
		}
	}
	return false
}

func addSource(w *fsnotify.Watcher, source string) error {
	info, err := os.Stat(source)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return w.Add(filepath.Dir(source))
	}
	return filepath.WalkDir(source, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return w.Add(path)
		}
		return nil
	})
}

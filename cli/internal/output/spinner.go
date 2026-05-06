package output

import (
	"fmt"
	"io"
	"os"
	"sync"
	"time"
)

type Spinner struct {
	w       io.Writer
	label   string
	done    chan struct{}
	stopped chan struct{}
	once    sync.Once
	active  bool
}

func NewSpinner(label string) *Spinner {
	return &Spinner{
		w:       os.Stderr,
		label:   label,
		done:    make(chan struct{}),
		stopped: make(chan struct{}),
		active:  isTerminal(os.Stderr),
	}
}

func (s *Spinner) Start() {
	if !s.active {
		return
	}
	go func() {
		defer close(s.stopped)
		frames := []rune{'-', '\\', '|', '/'}
		t := time.NewTicker(120 * time.Millisecond)
		defer t.Stop()
		i := 0
		for {
			select {
			case <-s.done:
				fmt.Fprint(s.w, "\r\033[K")
				return
			case <-t.C:
				fmt.Fprintf(s.w, "\r%c %s", frames[i%len(frames)], s.label)
				i++
			}
		}
	}()
}

func (s *Spinner) Stop() {
	s.once.Do(func() {
		if !s.active {
			return
		}
		close(s.done)
		<-s.stopped
	})
}

func isTerminal(f *os.File) bool {
	info, err := f.Stat()
	return err == nil && (info.Mode()&os.ModeCharDevice) != 0
}

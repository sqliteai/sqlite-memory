package pdf

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

type fakeConverter struct {
	calls int
}

func (f *fakeConverter) Convert(ctx context.Context, pdfPath string) (string, error) {
	f.calls++
	return "# converted\n", nil
}

func TestPDFCacheSkipUnchanged(t *testing.T) {
	dir := t.TempDir()
	pdfPath := filepath.Join(dir, "doc.pdf")
	if err := os.WriteFile(pdfPath, []byte("pdf bytes"), 0644); err != nil {
		t.Fatal(err)
	}
	converter := &fakeConverter{}
	cache := Cache{Dir: filepath.Join(dir, "cache"), Converter: converter}
	first, err := cache.Process(context.Background(), pdfPath)
	if err != nil {
		t.Fatal(err)
	}
	if first.Skipped {
		t.Fatal("first conversion skipped")
	}
	if first.IndexPath == "" {
		t.Fatal("index path not set")
	}
	if first.IndexPath != IndexPathForSource(cache.Dir, pdfPath) {
		t.Fatalf("index path mismatch: %q", first.IndexPath)
	}
	if _, err := os.Stat(first.IndexPath); err != nil {
		t.Fatalf("index markdown not written: %v", err)
	}
	second, err := cache.Process(context.Background(), pdfPath)
	if err != nil {
		t.Fatal(err)
	}
	if !second.Skipped {
		t.Fatal("unchanged PDF was not skipped")
	}
	if converter.calls != 1 {
		t.Fatalf("converter calls = %d", converter.calls)
	}
	if second.IndexPath != first.IndexPath {
		t.Fatalf("index path changed: %q != %q", second.IndexPath, first.IndexPath)
	}
}

package pdf

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

type Metadata struct {
	Path  string `json:"path"`
	MTime int64  `json:"mtime"`
	Size  int64  `json:"size"`
	Hash  string `json:"hash"`
}

type Result struct {
	MarkdownPath string
	IndexPath    string
	Skipped      bool
	Metadata     Metadata
}

type Converter interface {
	Convert(ctx context.Context, pdfPath string) (string, error)
}

type GLMOCRConverter struct{}

func (GLMOCRConverter) Convert(ctx context.Context, pdfPath string) (string, error) {
	cmd := exec.CommandContext(ctx, "glm-ocr", pdfPath)
	out, err := cmd.Output()
	if err != nil {
		return "", fmt.Errorf("convert PDF with glm-ocr: %w", err)
	}
	return string(out), nil
}

type Cache struct {
	Dir       string
	Force     bool
	Converter Converter
}

func (c Cache) Process(ctx context.Context, path string) (Result, error) {
	meta, err := ReadMetadata(path)
	if err != nil {
		return Result{}, err
	}
	dir := filepath.Join(c.Dir, meta.Hash)
	sourcePath := filepath.Join(dir, "source.json")
	contentPath := filepath.Join(dir, "content.md")
	indexPath := IndexPathForSource(c.Dir, meta.Path)
	if !c.Force && unchanged(sourcePath, contentPath, indexPath, meta) {
		return Result{MarkdownPath: contentPath, IndexPath: indexPath, Skipped: true, Metadata: meta}, nil
	}
	converter := c.Converter
	if converter == nil {
		converter = GLMOCRConverter{}
	}
	markdown, err := converter.Convert(ctx, path)
	if err != nil {
		return Result{}, err
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return Result{}, err
	}
	if err := os.WriteFile(contentPath, []byte(markdown), 0644); err != nil {
		return Result{}, err
	}
	if err := os.MkdirAll(filepath.Dir(indexPath), 0755); err != nil {
		return Result{}, err
	}
	if err := os.WriteFile(indexPath, []byte(markdown), 0644); err != nil {
		return Result{}, err
	}
	data, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return Result{}, err
	}
	data = append(data, '\n')
	if err := os.WriteFile(sourcePath, data, 0644); err != nil {
		return Result{}, err
	}
	return Result{MarkdownPath: contentPath, IndexPath: indexPath, Metadata: meta}, nil
}

func ReadMetadata(path string) (Metadata, error) {
	info, err := os.Stat(path)
	if err != nil {
		return Metadata{}, err
	}
	f, err := os.Open(path)
	if err != nil {
		return Metadata{}, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return Metadata{}, err
	}
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	return Metadata{
		Path:  abs,
		MTime: info.ModTime().UnixNano(),
		Size:  info.Size(),
		Hash:  hex.EncodeToString(h.Sum(nil)),
	}, nil
}

func unchanged(sourcePath, contentPath, indexPath string, meta Metadata) bool {
	if _, err := os.Stat(contentPath); err != nil {
		return false
	}
	if _, err := os.Stat(indexPath); err != nil {
		return false
	}
	data, err := os.ReadFile(sourcePath)
	if err != nil {
		return false
	}
	var old Metadata
	if err := json.Unmarshal(data, &old); err != nil {
		return false
	}
	return old.Hash == meta.Hash && old.Size == meta.Size && old.MTime == meta.MTime
}

func IndexPathForSource(cacheDir string, path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	sum := sha256.Sum256([]byte(abs))
	name := hex.EncodeToString(sum[:]) + ".md"
	return filepath.Join(cacheDir, "index", name)
}

func Touch(path string, t time.Time) error {
	return os.Chtimes(path, t, t)
}

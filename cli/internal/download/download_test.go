package download

import (
	"archive/tar"
	"compress/gzip"
	"os"
	"path/filepath"
	"testing"
)

func TestPlatformTokens(t *testing.T) {
	p := Platform{OS: "darwin", Arch: "arm64"}
	if p.SharedLibraryExt() != ".dylib" {
		t.Fatalf("ext = %q", p.SharedLibraryExt())
	}
	if !containsAny("sqlite-memory-macos-aarch64.tar.gz", p.OSTokens()) {
		t.Fatal("macOS token not detected")
	}
	if !containsAny("sqlite-memory-macos-aarch64.tar.gz", p.ArchTokens()) {
		t.Fatal("arm token not detected")
	}
}

func TestSelectAsset(t *testing.T) {
	asset, err := SelectAsset("sqlite-memory", []Asset{
		{Name: "sqlite-memory-linux-x86_64.tar.gz"},
		{Name: "sqlite-memory-darwin-arm64.tar.gz"},
		{Name: "checksums.txt"},
	}, Platform{OS: "darwin", Arch: "arm64"})
	if err != nil {
		t.Fatal(err)
	}
	if asset.Name != "sqlite-memory-darwin-arm64.tar.gz" {
		t.Fatalf("asset = %q", asset.Name)
	}
}

func TestSelectAssetWindowsDoesNotMatchDarwin(t *testing.T) {
	asset, err := SelectAsset("sqlite-memory", []Asset{
		{Name: "sqlite-memory-darwin-x86_64.tar.gz"},
		{Name: "sqlite-memory-windows-x86_64.tar.gz"},
	}, Platform{OS: "windows", Arch: "amd64"})
	if err != nil {
		t.Fatal(err)
	}
	if asset.Name != "sqlite-memory-windows-x86_64.tar.gz" {
		t.Fatalf("asset = %q", asset.Name)
	}
}

func TestFindSharedLibrary(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "libmemory.dylib")
	if err := os.WriteFile(path, []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}
	got, ok := FindSharedLibrary(dir, "sqlite-memory", Platform{OS: "darwin", Arch: "arm64"})
	if !ok || got != path {
		t.Fatalf("lib = %q %v", got, ok)
	}
}

func TestExtractTarGzAllowsRootDirectoryEntry(t *testing.T) {
	dir := t.TempDir()
	archivePath := filepath.Join(dir, "archive.tar.gz")
	if err := writeTarGz(archivePath, []tar.Header{
		{Name: "./", Typeflag: tar.TypeDir, Mode: 0755},
		{Name: "libmemory.dylib", Typeflag: tar.TypeReg, Mode: 0644, Size: 1},
	}, []byte("x")); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(dir, "target")
	if err := os.MkdirAll(target, 0755); err != nil {
		t.Fatal(err)
	}
	if err := extractTarGz(archivePath, target); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(target, "libmemory.dylib")); err != nil {
		t.Fatal(err)
	}
}

func writeTarGz(path string, headers []tar.Header, fileData []byte) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	gz := gzip.NewWriter(f)
	defer gz.Close()
	tw := tar.NewWriter(gz)
	defer tw.Close()
	for _, header := range headers {
		h := header
		if err := tw.WriteHeader(&h); err != nil {
			return err
		}
		if h.Typeflag == tar.TypeReg {
			if _, err := tw.Write(fileData); err != nil {
				return err
			}
		}
	}
	return nil
}

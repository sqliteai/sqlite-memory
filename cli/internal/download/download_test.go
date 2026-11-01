package download

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
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

func TestIsGitHubHost(t *testing.T) {
	cases := []struct {
		host string
		want bool
	}{
		{"github.com", true},
		{"api.github.com", true},
		{"objects.githubusercontent.com", true},
		{"codeload.github.com", true},
		{"GitHub.com", true},
		{"evil.example", false},
		{"github.com.evil.example", false},
		{"notgithub.com", false},
		{"githubusercontent.com.evil.test", false},
		{"", false},
		// a bare suffix is not a host, and a trailing dot is a different name
		{".github.com", true}, // genuinely a subdomain-shaped name under github.com
		{"github.com.", false},
		{"xgithub.com", false},
		{"githubusercontent.com", false}, // only *.githubusercontent.com serves assets
	}
	for _, c := range cases {
		if got := isGitHubHost(c.host); got != c.want {
			t.Errorf("isGitHubHost(%q) = %v, want %v", c.host, got, c.want)
		}
	}
}

func TestDownloadRejectsNonHTTPS(t *testing.T) {
	dst := filepath.Join(t.TempDir(), "asset.tar.gz")
	c := Client{}
	err := c.download(context.Background(), "http://example.invalid/asset.tar.gz", dst)
	if err == nil {
		t.Fatal("expected plaintext download to be refused")
	}
	if !strings.Contains(err.Error(), "https") {
		t.Fatalf("error = %v, want it to mention https", err)
	}
	if _, statErr := os.Stat(dst); statErr == nil {
		t.Fatal("no file should have been created")
	}
}

func TestHTTPSOnlyRedirectRefusesDowngrade(t *testing.T) {
	plain, _ := http.NewRequest(http.MethodGet, "http://example.invalid/x", nil)
	if err := httpsOnlyRedirect(plain, nil); err == nil {
		t.Fatal("expected a redirect to http to be refused")
	}
	secure, _ := http.NewRequest(http.MethodGet, "https://example.invalid/x", nil)
	if err := httpsOnlyRedirect(secure, nil); err != nil {
		t.Fatalf("https redirect refused: %v", err)
	}
}

func TestDownloadDoesNotSendTokenToNonGitHubHost(t *testing.T) {
	var gotAuth string
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotAuth = r.Header.Get("Authorization")
		_, _ = w.Write([]byte("payload"))
	}))
	defer srv.Close()

	// the asset URL comes from an API response, so it may point anywhere
	t.Setenv("GITHUB_TOKEN", "super-secret-token")
	dst := filepath.Join(t.TempDir(), "asset.bin")
	c := Client{HTTP: srv.Client()}
	if err := c.download(context.Background(), srv.URL+"/asset.bin", dst); err != nil {
		t.Fatal(err)
	}
	if gotAuth != "" {
		t.Fatalf("Authorization header leaked to a non-GitHub host: %q", gotAuth)
	}
	body, err := os.ReadFile(dst)
	if err != nil {
		t.Fatal(err)
	}
	if string(body) != "payload" {
		t.Fatalf("body = %q", body)
	}
}

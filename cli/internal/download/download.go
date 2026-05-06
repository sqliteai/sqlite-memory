package download

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const (
	RepoVector = "sqlite-vector"
	RepoMemory = "sqlite-memory"
	RepoSync   = "sqlite-sync"
)

type Client struct {
	HTTP     *http.Client
	Platform Platform
}

type Asset struct {
	Name               string `json:"name"`
	BrowserDownloadURL string `json:"browser_download_url"`
}

type release struct {
	TagName string  `json:"tag_name"`
	Assets  []Asset `json:"assets"`
}

func (c Client) Install(ctx context.Context, owner, repo, version, baseDir string) (string, error) {
	if version == "" {
		version = "latest"
	}
	if c.HTTP == nil {
		c.HTTP = http.DefaultClient
	}
	if c.Platform.OS == "" {
		c.Platform = CurrentPlatform()
	}
	rel, err := c.release(ctx, owner, repo, version)
	if err != nil {
		return "", err
	}
	cacheVersion := rel.TagName
	if cacheVersion == "" {
		cacheVersion = version
	}
	targetDir := filepath.Join(baseDir, repo, cacheVersion)
	if lib, ok := FindSharedLibrary(targetDir, repo, c.Platform); ok {
		return lib, nil
	}
	asset, err := SelectAsset(repo, rel.Assets, c.Platform)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(targetDir, 0755); err != nil {
		return "", err
	}
	downloadPath := filepath.Join(targetDir, asset.Name)
	if err := c.download(ctx, asset.BrowserDownloadURL, downloadPath); err != nil {
		return "", err
	}
	if isArchive(downloadPath) {
		if err := extract(downloadPath, targetDir); err != nil {
			return "", err
		}
	}
	if lib, ok := FindSharedLibrary(targetDir, repo, c.Platform); ok {
		return lib, nil
	}
	if strings.HasSuffix(strings.ToLower(downloadPath), c.Platform.SharedLibraryExt()) {
		return downloadPath, nil
	}
	return "", fmt.Errorf("no shared library found in %s", targetDir)
}

func (c Client) release(ctx context.Context, owner, repo, version string) (release, error) {
	url := fmt.Sprintf("https://api.github.com/repos/%s/%s/releases/latest", owner, repo)
	if version != "latest" {
		url = fmt.Sprintf("https://api.github.com/repos/%s/%s/releases/tags/%s", owner, repo, version)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return release{}, err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	if token := os.Getenv("GITHUB_TOKEN"); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	res, err := c.HTTP.Do(req)
	if err != nil {
		return release{}, err
	}
	defer res.Body.Close()
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(res.Body, 512))
		return release{}, fmt.Errorf("github release request failed: %s %s", res.Status, strings.TrimSpace(string(body)))
	}
	var rel release
	if err := json.NewDecoder(res.Body).Decode(&rel); err != nil {
		return release{}, err
	}
	return rel, nil
}

func (c Client) download(ctx context.Context, url, path string) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	if token := os.Getenv("GITHUB_TOKEN"); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	res, err := c.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	if res.StatusCode < 200 || res.StatusCode >= 300 {
		return fmt.Errorf("download failed: %s", res.Status)
	}
	out, err := os.Create(path)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, res.Body)
	return err
}

func SelectAsset(repo string, assets []Asset, platform Platform) (Asset, error) {
	type scored struct {
		asset Asset
		score int
	}
	var matches []scored
	for _, asset := range assets {
		name := strings.ToLower(asset.Name)
		if strings.Contains(name, "sha256") || strings.Contains(name, "checksum") {
			continue
		}
		score := 0
		if containsAny(name, platform.OSTokens()) {
			score += 10
		}
		if containsAny(name, platform.ArchTokens()) {
			score += 10
		}
		if strings.Contains(name, repo) || strings.Contains(name, strings.TrimPrefix(repo, "sqlite-")) {
			score += 2
		}
		if strings.HasSuffix(name, ".zip") || strings.HasSuffix(name, ".tar.gz") || strings.HasSuffix(name, ".tgz") {
			score++
		}
		if strings.HasSuffix(name, platform.SharedLibraryExt()) {
			score += 3
		}
		if score >= 20 {
			matches = append(matches, scored{asset: asset, score: score})
		}
	}
	if len(matches) == 0 {
		return Asset{}, fmt.Errorf("no %s asset for %s/%s", repo, platform.OS, platform.Arch)
	}
	sort.Slice(matches, func(i, j int) bool {
		if matches[i].score == matches[j].score {
			return matches[i].asset.Name < matches[j].asset.Name
		}
		return matches[i].score > matches[j].score
	})
	return matches[0].asset, nil
}

func FindSharedLibrary(root, repo string, platform Platform) (string, bool) {
	ext := platform.SharedLibraryExt()
	key := strings.TrimPrefix(repo, "sqlite-")
	var found string
	filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() || found != "" {
			return nil
		}
		name := strings.ToLower(d.Name())
		if strings.HasSuffix(name, ext) && (strings.Contains(name, key) || strings.Contains(name, repo)) {
			found = path
		}
		return nil
	})
	return found, found != ""
}

func containsAny(s string, tokens []string) bool {
	for _, token := range tokens {
		if containsToken(s, token) {
			return true
		}
	}
	return false
}

func containsToken(s, token string) bool {
	start := 0
	for {
		i := strings.Index(s[start:], token)
		if i < 0 {
			return false
		}
		i += start
		before := i == 0 || !isTokenChar(rune(s[i-1]))
		afterIndex := i + len(token)
		after := afterIndex == len(s) || !isTokenChar(rune(s[afterIndex]))
		if before && after {
			return true
		}
		start = i + 1
	}
}

func isTokenChar(r rune) bool {
	return r >= 'a' && r <= 'z' || r >= '0' && r <= '9'
}

func isArchive(path string) bool {
	lower := strings.ToLower(path)
	return strings.HasSuffix(lower, ".zip") || strings.HasSuffix(lower, ".tar.gz") || strings.HasSuffix(lower, ".tgz")
}

func extract(path, targetDir string) error {
	lower := strings.ToLower(path)
	if strings.HasSuffix(lower, ".zip") {
		return extractZip(path, targetDir)
	}
	return extractTarGz(path, targetDir)
}

func extractZip(path, targetDir string) error {
	zr, err := zip.OpenReader(path)
	if err != nil {
		return err
	}
	defer zr.Close()
	for _, f := range zr.File {
		outPath, ok := safeArchivePath(targetDir, f.Name)
		if !ok {
			return fmt.Errorf("unsafe archive path %s", f.Name)
		}
		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(outPath, 0755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(outPath), 0755); err != nil {
			return err
		}
		in, err := f.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(outPath)
		if err != nil {
			in.Close()
			return err
		}
		_, copyErr := io.Copy(out, in)
		closeErr := out.Close()
		in.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		_ = os.Chmod(outPath, f.FileInfo().Mode())
	}
	return nil
}

func extractTarGz(path, targetDir string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	gz, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	defer gz.Close()
	tr := tar.NewReader(gz)
	for {
		h, err := tr.Next()
		if errorsIsEOF(err) {
			return nil
		}
		if err != nil {
			return err
		}
		outPath, ok := safeArchivePath(targetDir, h.Name)
		if !ok {
			return fmt.Errorf("unsafe archive path %s", h.Name)
		}
		switch h.Typeflag {
		case tar.TypeDir:
			if err := os.MkdirAll(outPath, 0755); err != nil {
				return err
			}
		case tar.TypeReg:
			if err := os.MkdirAll(filepath.Dir(outPath), 0755); err != nil {
				return err
			}
			out, err := os.OpenFile(outPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, os.FileMode(h.Mode))
			if err != nil {
				return err
			}
			_, copyErr := io.Copy(out, tr)
			closeErr := out.Close()
			if copyErr != nil {
				return copyErr
			}
			if closeErr != nil {
				return closeErr
			}
		}
	}
}

func safeArchivePath(targetDir, name string) (string, bool) {
	cleanTarget := filepath.Clean(targetDir)
	outPath := filepath.Join(cleanTarget, name)
	cleanOut := filepath.Clean(outPath)
	if cleanOut == cleanTarget {
		return cleanOut, true
	}
	return cleanOut, strings.HasPrefix(cleanOut, cleanTarget+string(os.PathSeparator))
}

func errorsIsEOF(err error) bool {
	return err == io.EOF
}

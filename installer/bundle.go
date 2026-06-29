package main

import (
	"archive/zip"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// repoSlug is the GitHub "owner/repo" the installer downloads releases from.
// TODO: set this when the origin repo is chosen (deferred sub-decision in the distribution spec).
// Until then, the GitHub path is disabled and you install with --local <dist>.
const repoSlug = "OWNER/open-snaphak"

// releaseAsset is the stable asset name CI publishes on every release (so latest/download works).
const releaseAsset = "snaphak-bundle.zip"

// bundle is a ready-to-deploy overlay tree (a dist/ dir) plus its MANIFEST.sha256 file list.
type bundle struct {
	root  string
	files []manifestEntry
}

type manifestEntry struct {
	rel    string // overlay-relative path, e.g. "snaphak\snaphakui.dll"
	sha256 string
}

// acquireBundle returns a verified bundle (from --local, else a downloaded release) plus a cleanup func.
func acquireBundle(f flags) (*bundle, func(), error) {
	noop := func() {}
	if f.local != "" {
		b, err := loadBundle(f.local)
		return b, noop, err
	}
	dir, cleanup, err := downloadRelease(f.release)
	if err != nil {
		return nil, noop, err
	}
	b, err := loadBundle(dir)
	if err != nil {
		cleanup()
		return nil, noop, err
	}
	return b, cleanup, nil
}

// loadBundle reads dist/MANIFEST.sha256 and verifies every listed file is present and hash-correct,
// so we never start writing into a DOOM install from a partial or tampered bundle.
func loadBundle(root string) (*bundle, error) {
	f, err := os.Open(filepath.Join(root, "MANIFEST.sha256"))
	if err != nil {
		return nil, fmt.Errorf("no MANIFEST.sha256 in %q (not a dist/ bundle): %w", root, err)
	}
	defer f.Close()

	var entries []manifestEntry
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return nil, fmt.Errorf("malformed MANIFEST line: %q", line)
		}
		entries = append(entries, manifestEntry{
			rel:    filepath.FromSlash(fields[1]),
			sha256: strings.ToLower(fields[0]),
		})
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("empty MANIFEST.sha256 in %q", root)
	}
	for _, e := range entries {
		got, err := fileSHA256(filepath.Join(root, e.rel))
		if err != nil {
			return nil, fmt.Errorf("bundle file missing: %s (%w)", e.rel, err)
		}
		if got != e.sha256 {
			return nil, fmt.Errorf("bundle file hash mismatch: %s", e.rel)
		}
	}
	return &bundle{root: root, files: entries}, nil
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// downloadRelease fetches + extracts the release zip into a temp dir, returning that dir and a cleanup func.
func downloadRelease(tag string) (dir string, cleanup func(), err error) {
	noop := func() {}
	if strings.HasPrefix(repoSlug, "OWNER/") {
		return "", noop, fmt.Errorf("release download is not configured yet (repoSlug placeholder) -- use --local <dist> for now")
	}
	var url string
	if tag == "" || tag == "latest" {
		url = fmt.Sprintf("https://github.com/%s/releases/latest/download/%s", repoSlug, releaseAsset)
	} else {
		url = fmt.Sprintf("https://github.com/%s/releases/download/%s/%s", repoSlug, tag, releaseAsset)
	}
	tmp, err := os.MkdirTemp("", "snaphak-bundle-")
	if err != nil {
		return "", noop, err
	}
	cleanup = func() { os.RemoveAll(tmp) }

	zipPath := filepath.Join(tmp, releaseAsset)
	if err := httpDownload(url, zipPath); err != nil {
		cleanup()
		return "", noop, fmt.Errorf("download %s: %w", url, err)
	}
	if err := unzip(zipPath, tmp); err != nil {
		cleanup()
		return "", noop, err
	}
	return tmp, cleanup, nil
}

func httpDownload(url, dest string) error {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snaphak-installer")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	out, err := os.Create(dest)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, resp.Body)
	return err
}

// unzip extracts src into dest, guarding against zip-slip path traversal.
func unzip(src, dest string) error {
	r, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer r.Close()
	prefix := filepath.Clean(dest) + string(os.PathSeparator)
	for _, zf := range r.File {
		target := filepath.Join(dest, filepath.FromSlash(zf.Name))
		if !strings.HasPrefix(target, prefix) {
			return fmt.Errorf("unsafe zip entry: %s", zf.Name)
		}
		if zf.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		rc, err := zf.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(target)
		if err != nil {
			rc.Close()
			return err
		}
		_, copyErr := io.Copy(out, rc)
		out.Close()
		rc.Close()
		if copyErr != nil {
			return copyErr
		}
	}
	return nil
}

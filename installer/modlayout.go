package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// The verified backend carries this same manifest path when it supports the split layout.
const modBuildManifest = `.build\manifest.json`

var modSalt = regexp.MustCompile(`^[a-z][a-z0-9]{7}$`)
var modWorkFolder = regexp.MustCompile(`\.(old|staging)-[0-9]+$`)

func upgradeModLayout(data string) error {
	mods := filepath.Join(data, "mods")
	cache := filepath.Join(data, "cache")
	recovery := &modRecovery{cache: cache}
	for _, name := range []string{"content", "served"} {
		if err := moveModCache(filepath.Join(mods, "."+name), filepath.Join(cache, name), name, recovery); err != nil {
			return err
		}
	}
	if err := splitModFolders(mods, mods, false, recovery); err != nil {
		return err
	}
	served := filepath.Join(cache, "content", "served")
	return splitModFolders(served, served, true, recovery)
}

type modRecovery struct {
	cache, dir string
}

func (r *modRecovery) keep(path, rel string) error {
	if r.dir == "" {
		if !plainPath(r.cache) {
			return fmt.Errorf("the cache folder is linked: %s", r.cache)
		}
		if err := os.MkdirAll(r.cache, 0o755); err != nil {
			return err
		}
		var err error
		r.dir, err = os.MkdirTemp(r.cache, "recovered-mod-files-")
		if err != nil {
			return err
		}
		ui.Note("Kept differing copies of mod files in %s.", r.dir)
	}
	dst := filepath.Join(r.dir, rel)
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	return os.Rename(path, dst)
}

func moveModCache(src, dst, rel string, recovery *modRecovery) error {
	old, err := os.Lstat(src)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !plainPath(src) || !plainPath(dst) {
		return fmt.Errorf("a mod cache path is linked: %s", rel)
	}
	current, err := os.Lstat(dst)
	if errors.Is(err, os.ErrNotExist) {
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		// Renaming the directory also preserves hard links, read-only flags and partial downloads.
		return os.Rename(src, dst)
	}
	if err != nil {
		return err
	}
	if old.IsDir() && current.IsDir() {
		children, err := os.ReadDir(src)
		if err != nil {
			return err
		}
		for _, e := range children {
			if err := moveModCache(filepath.Join(src, e.Name()), filepath.Join(dst, e.Name()), filepath.Join(rel, e.Name()), recovery); err != nil {
				return err
			}
		}
		return os.Remove(src)
	}
	if old.Mode().IsRegular() && current.Mode().IsRegular() {
		if os.SameFile(old, current) || identicalFiles(src, dst) {
			// Windows clears the read-only bit when removing a file, including on its other hard links.
			if old.Mode().Perm()&0o200 != 0 {
				if err := os.Remove(src); err == nil {
					return nil
				}
			}
		} else {
			name := strings.TrimSuffix(filepath.Base(src), ".json")
			better := false
			if len(name) == sha256.Size*2 {
				sum, err := fileSHA256(src)
				if err != nil {
					return err
				}
				better = sum == name
			} else if strings.EqualFold(filepath.Ext(src), ".part") && old.Size() > current.Size() {
				better, err = isFilePrefix(dst, src, current.Size())
				if err != nil {
					return err
				}
			}
			if better {
				if err := recovery.keep(dst, rel); err != nil {
					return err
				}
				return os.Rename(src, dst)
			}
		}
	}
	return recovery.keep(src, rel)
}

func isFilePrefix(short, long string, size int64) (bool, error) {
	a, err := fileSHA256(short)
	if err != nil {
		return false, err
	}
	f, err := os.Open(long)
	if err != nil {
		return false, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.CopyN(h, f, size); err != nil {
		return false, err
	}
	return fmt.Sprintf("%x", h.Sum(nil)) == a, nil
}

func splitModFolders(root, parent string, compositions bool, recovery *modRecovery) error {
	if !plainPath(parent) {
		return fmt.Errorf("the mod folder is linked: %s", parent)
	}
	entries, err := os.ReadDir(parent)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, e := range entries {
		if !e.IsDir() || strings.HasPrefix(e.Name(), ".") || modWorkFolder.MatchString(e.Name()) {
			continue
		}
		if !compositions && parent == root && strings.EqualFold(e.Name(), "maps") {
			continue
		}
		folder := filepath.Join(parent, e.Name())
		if !plainPath(folder) {
			continue
		}
		found, err := splitModFile(folder, recovery)
		if err != nil {
			return err
		}
		if !compositions && !found && !isRegular(filepath.Join(folder, "package.json")) {
			if err := splitModFolders(root, folder, false, recovery); err != nil {
				return err
			}
		}
	}
	return nil
}

func splitModFile(folder string, recovery *modRecovery) (bool, error) {
	path := filepath.Join(folder, "mod.json")
	if !plainPath(path) {
		return true, nil
	}
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return true, err
	}
	start, manifest, identity := inlineModBuild(data)
	if manifest == nil {
		return true, nil
	}
	sidecar := filepath.Join(folder, ".build", "manifest.json")
	if !plainPath(sidecar) {
		return true, fmt.Errorf("the build manifest is linked: %s", sidecar)
	}
	existing, err := os.ReadFile(sidecar)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return true, err
	}
	if !bytes.Equal(existing, manifest) {
		if err == nil {
			rel, err := filepath.Rel(filepath.Dir(recovery.cache), sidecar)
			if err != nil {
				return true, err
			}
			if err := recovery.keep(sidecar, rel); err != nil {
				return true, err
			}
		}
		if err := os.MkdirAll(filepath.Dir(sidecar), 0o755); err != nil {
			return true, err
		}
		if err := writeAtomicFile(sidecar, manifest); err != nil {
			return true, err
		}
	}

	// Write the complete sidecar first so an interrupted update can repeat this step.
	out := make([]byte, 0, len(data)-len(manifest)+len(identity))
	out = append(out, data[:start]...)
	out = append(out, identity...)
	out = append(out, data[start+len(manifest):]...)
	return true, writeAtomicFile(path, out)
}

// Decoder offsets let the installer replace only the builder's value, preserving every author byte.
func inlineModBuild(data []byte) (int, []byte, []byte) {
	text := bytes.TrimPrefix(data, []byte("\xef\xbb\xbf"))
	if !json.Valid(text) {
		return 0, nil, nil
	}
	d := json.NewDecoder(bytes.NewReader(text))
	if tok, _ := d.Token(); tok != json.Delim('{') {
		return 0, nil, nil
	}
	for d.More() {
		key, _ := d.Token()
		var raw json.RawMessage
		if err := d.Decode(&raw); err != nil {
			return 0, nil, nil
		}
		if key != "snapmap-plus" {
			continue
		}
		var block struct {
			Format int             `json:"format"`
			Salt   string          `json:"salt"`
			Rules  json.RawMessage `json:"rules"`
		}
		if json.Unmarshal(raw, &block) != nil || block.Format != 2 || !modSalt.MatchString(block.Salt) || len(block.Rules) == 0 {
			return 0, nil, nil
		}
		end := len(data) - len(text) + int(d.InputOffset())
		identity := []byte(fmt.Sprintf(`{"format":2,"salt":"%s"}`, block.Salt))
		return end - len(raw), raw, identity
	}
	return 0, nil, nil
}

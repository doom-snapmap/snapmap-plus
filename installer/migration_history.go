package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// The original snapshot remains an input until every recognized map and prefab has been inspected.
func (p *migrationPlan) retireBuildHistory() error {
	if err := p.retireRecovery(); err != nil {
		return err
	}
	for _, home := range p.legacyHomes() {
		dirs := []string{filepath.Join(home, "mods", ".builds")}
		if samePath(home, p.data) {
			dirs = append(dirs, filepath.Join(home, "cache", "legacy-builds"))
		}
		for _, dir := range dirs {
			if !plainPath(dir) {
				continue
			}
			entries, err := os.ReadDir(dir)
			if errors.Is(err, fs.ErrNotExist) {
				continue
			}
			if err != nil {
				return err
			}
			for _, child := range entries {
				folder := filepath.Join(dir, child.Name())
				if !child.IsDir() || !plainPath(folder) || p.needed[folder] || p.held[folder] {
					continue
				}
				if err = p.retireSnapshot(folder, filepath.Base(dir) == "legacy-builds"); err != nil {
					return err
				}
			}
			p.empty(dir)
		}
	}
	return nil
}

func (p *migrationPlan) retireSnapshot(folder string, cached bool) error {
	metadata := map[string]string{}
	for _, rel := range []string{"mod.json", ".build/manifest.json", "bundle.json", "bundle.canonical.json"} {
		path := filepath.Join(folder, filepath.FromSlash(rel))
		if !plainPath(path) {
			return nil
		}
		if earlierPathExists(path) && !isRegular(path) {
			return nil
		}
		hash, err := migrationFileHash(path)
		if err != nil {
			return err
		}
		metadata[rel] = hash
	}
	b, err := p.pins.inspect(folder, false)
	if err != nil {
		return nil
	}
	leaf := filepath.Base(folder)
	recordOnly := cached && strings.EqualFold(leaf, b.hash)
	if !recordOnly && !strings.EqualFold(leaf, b.id+"@"+b.hash[:16]) {
		return nil
	}
	if b.inline {
		if err = p.pins.verifyInline(b, p.work); err != nil {
			return nil
		}
	}
	expected := map[string]string{}
	if b.inline {
		expected["mod.json"] = ""
		if isRegular(filepath.Join(folder, ".build", "manifest.json")) {
			var doc earlierObject
			var header struct{ Build string }
			if err = json.Unmarshal(b.descriptor, &doc); err == nil {
				err = json.Unmarshal(doc["snapmap-plus"], &header)
			}
			if err != nil {
				return err
			}
			if header.Build == "" {
				expected[".build/manifest.json"] = ""
			}
		}
	} else if b.originalFile != "" {
		expected[b.originalFile] = ""
	} else {
		expected["bundle.canonical.json"] = ""
		if isRegular(filepath.Join(folder, "bundle.json")) {
			expected["bundle.json"] = ""
		}
	}
	for _, file := range b.files {
		want := file.hash()
		if file.Stock != nil {
			want = file.Stock.SHA256
		}
		path := filepath.Join(folder, filepath.FromSlash(file.Path))
		if !plainPath(path) {
			return nil
		}
		actual, err := migrationFileHash(path)
		if err != nil {
			return err
		}
		if actual == "" && (file.Stock != nil || recordOnly) {
			continue
		}
		if actual != want {
			p.Deferred = append(p.Deferred, fmt.Sprintf("%s: historical build file %s is missing or edited; kept the folder", folder, file.Path))
			return nil
		}
		expected[file.Path] = want
	}
	for rel, hash := range expected {
		if hash == "" {
			hash = metadata[rel]
			if hash == "" {
				return fmt.Errorf("historical descriptor changed: %s", folder)
			}
		}
		path := filepath.Join(folder, filepath.FromSlash(rel))
		if err = p.record(path, hash, "", "", "Mods"); err != nil {
			return err
		}
		for parent := filepath.Dir(path); under(folder, parent); parent = filepath.Dir(parent) {
			p.empty(parent)
		}
	}
	p.empty(folder)
	return nil
}

// An edited retry archive or an incomplete library scan may still contain the user's only copy.
func (p *migrationPlan) retireRecovery() error {
	dir := filepath.Join(p.data, "cache", "migration-units")
	if !plainPath(dir) {
		return nil
	}
	files, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range files {
		name := entry.Name()
		file := filepath.Join(dir, name)
		if p.keepRecovery[file] || entry.IsDir() || !strings.HasSuffix(name, ".zip") || !plainPath(file) || !isRegular(file) {
			continue
		}
		key, hash, ok := strings.Cut(strings.TrimSuffix(name, ".zip"), "-")
		if !ok || !pinHash(key) || !pinHash(hash) {
			continue
		}
		if !p.usedRecovery[file] {
			if len(p.Deferred) > 0 {
				continue
			}
			actual, err := migrationFileHash(file)
			if err != nil {
				return err
			}
			if actual != hash {
				continue
			}
			tmp, err := os.MkdirTemp(p.work, "unused-retry-*")
			if err != nil {
				return err
			}
			inspect := newEarlierPins(p.data, p.pins.doom, nil)
			inspect.output = tmp
			var names earlierGlobalNames
			_, verified := inspect.readRetry(file, key, &names)
			if err = os.RemoveAll(tmp); err != nil {
				return err
			}
			if verified != nil {
				continue
			}
		}
		if err = p.record(file, hash, "", "", "Mods"); err != nil {
			return err
		}
	}
	p.empty(dir)
	return nil
}

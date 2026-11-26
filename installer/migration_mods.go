package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func walkLibrary(root string, fn fs.WalkDirFunc) error {
	return filepath.WalkDir(root, func(path string, e fs.DirEntry, err error) error {
		if path == root && errors.Is(err, fs.ErrNotExist) {
			return nil
		}
		if err != nil {
			return err
		}
		if !plainPath(path) {
			if e.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		return fn(path, e, nil)
	})
}

func (p *migrationPlan) putBytes(path string, body []byte, stage string, replace bool) error {
	out, err := os.CreateTemp(p.work, "body-*")
	if err != nil {
		return err
	}
	defer os.Remove(out.Name())
	_, writeErr := out.Write(body)
	err = errors.Join(writeErr, out.Close())
	if err != nil {
		return err
	}
	return p.put(path, out.Name(), stage, replace)
}

func (p *migrationPlan) transferTree(source, dest, stage string, replace bool) error {
	return filepath.WalkDir(source, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(path) {
			return fmt.Errorf("linked content at %s", path)
		}
		if e.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		return p.put(filepath.Join(dest, rel), path, stage, replace)
	})
}

// Keep the exact build record for older external exports. Its verified payloads share the ordinary blob store.
func (p *migrationPlan) retainBuild(src, work string, consumed map[string]bool) (*earlierBundle, error) {
	var doc, block earlierObject
	for _, name := range []string{"mod.json", "bundle.json"} {
		if b, err := os.ReadFile(filepath.Join(src, name)); err == nil && json.Unmarshal(b, &doc) == nil {
			json.Unmarshal(doc["snapmap-plus"], &block)
			break
		}
	}
	if !isRegular(filepath.Join(src, "bundle.canonical.json")) && !pinHash(pinString(block, "bundle")) && !pinHash(pinString(block, "build")) {
		return nil, nil
	}
	b, err := p.pins.inspect(src, false)
	if err != nil {
		return nil, err
	}
	if !b.assets {
		return nil, nil
	}
	dir := filepath.Join(p.data, "cache", "legacy-builds", b.hash)
	if b.inline {
		if err = p.pins.ready(); err != nil {
			return nil, err
		}
		var original earlierObject
		if err = json.Unmarshal(b.descriptor, &original); err != nil {
			return nil, err
		}
		original["snapmap-plus"] = b.block
		data := pinJSON(original)
		if err = os.WriteFile(filepath.Join(work, "original.json"), data, 0600); err != nil {
			return nil, err
		}
		if _, err = p.pins.run(work, "verify="+b.hash); err != nil {
			return nil, fmt.Errorf("the author and build records disagree: %w", err)
		}
		if err = p.putBytes(filepath.Join(dir, "mod.json"), data, "Mods", false); err != nil {
			return nil, err
		}
	} else {
		if err = p.putBytes(filepath.Join(dir, "bundle.canonical.json"), b.descriptor, "Mods", false); err != nil {
			return nil, err
		}
		for _, name := range []string{"bundle.canonical.json", "bundle.json"} {
			consumed[filepath.Join(src, name)] = true
		}
		var old struct{ Extra earlierObject }
		json.Unmarshal(b.descriptor, &old)
		if old.Extra["legacy"] != nil {
			if err = p.pins.ready(); err != nil {
				return nil, err
			}
			b.added, err = p.pins.globalNames(b, work)
			if err != nil {
				return nil, err
			}
		}
	}
	for _, f := range b.files {
		if f.Stock != nil {
			continue
		}
		hash := f.hash()
		target := filepath.Join(p.data, "cache", "content", "files", hash[:2], hash)
		current, err := migrationFileHash(target)
		if err != nil {
			return nil, err
		}
		if current == hash {
			continue
		}
		file := filepath.Join(src, filepath.FromSlash(f.Path))
		if h, err := migrationFileHash(file); err != nil {
			return nil, err
		} else if h != hash {
			// An author may have edited this asset since publishing. Never record those bytes under the old identity.
			continue
		}
		if err = p.put(target, file, "Mods", false); err != nil {
			return nil, err
		}
	}
	return b, nil
}

func (p *migrationPlan) convertMod(src, dst string, pruned prunedStock) error {
	if !samePath(src, dst) && earlierPathExists(dst) {
		return fmt.Errorf("an existing mod already uses %s", dst)
	}
	tmp, err := os.MkdirTemp(p.work, "author-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	sourceHash, err := earlierTreeHash(src)
	if err != nil {
		return err
	}
	target := filepath.Join(tmp, "converted")
	consumed := map[string]bool{}
	bundle, err := p.retainBuild(src, tmp, consumed)
	if err != nil {
		return err
	}
	recovered, err := p.pins.prunedAuthorFiles(src, tmp, bundle, pruned)
	if err != nil {
		return err
	}
	if _, err = p.translateLocalAuthor(src, target, dst, bundle, consumed, recovered); err != nil {
		return err
	}
	if err = earlierExtraFiles(src, target, consumed); err != nil {
		return err
	}
	if err = p.transferTree(target, dst, "Mods", samePath(src, dst)); err != nil {
		return err
	}
	if h, e := earlierTreeHash(src); e != nil || h != sourceHash {
		return fmt.Errorf("the mod changed during preflight: %s", src)
	}
	return filepath.WalkDir(src, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(path) {
			return fmt.Errorf("linked mod path %s", path)
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if e.IsDir() {
			p.empty(path)
			return nil
		}
		if samePath(src, dst) && isRegular(filepath.Join(target, rel)) {
			return nil
		}
		return p.remove(path, "Mods")
	})
}

func (p *migrationPlan) splitAuthor(dir string) error {
	path := filepath.Join(dir, "mod.json")
	if !plainPath(path) {
		return fmt.Errorf("linked author manifest %s", path)
	}
	raw, err := os.ReadFile(path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	start, block, identity := inlineModBuild(raw)
	if block == nil {
		return nil
	}
	side := filepath.Join(dir, ".build", "manifest.json")
	// A matching sidecar is an interrupted split. A different one can be another author's build.
	if err = p.putBytes(side, block, "Mods", false); err != nil {
		return err
	}
	out := append([]byte{}, raw[:start]...)
	out = append(out, identity...)
	out = append(out, raw[start+len(block):]...)
	return p.putBytes(path, out, "Mods", true)
}

func (p *migrationPlan) authored(dir string) error {
	materialRoot := filepath.Join(dir, "materials")
	invalidated := false
	if err := walkLibrary(materialRoot, func(path string, e fs.DirEntry, _ error) error {
		if e.IsDir() || !strings.EqualFold(filepath.Ext(path), ".decl") {
			return nil
		}
		raw, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		next, err := upgradeEarlierMaterial(raw)
		if err != nil {
			return fmt.Errorf("%s: %w", path, err)
		}
		if !bytes.Equal(raw, next) {
			if !invalidated {
				if err := p.invalidateBuild(dir); err != nil {
					return err
				}
				invalidated = true
			}
			return p.putBytes(path, next, "Mods", true)
		}
		return nil
	}); err != nil {
		return err
	}
	if !invalidated {
		return p.splitAuthor(dir)
	}
	return nil
}

func (p *migrationPlan) invalidateBuild(dir string) error {

	// Invalidate the generated build before applying edited graphics, including an inline build.
	side := filepath.Join(dir, ".build", "manifest.json")
	if current, err := os.ReadFile(side); err == nil {
		_, block, _ := inlineModBuild(append(append([]byte(`{"snapmap-plus":`), current...), '}'))
		if block == nil {
			return fmt.Errorf("cannot identify the generated manifest at %s", side)
		}
		if err = p.remove(side, "Mods"); err != nil {
			return err
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	path := filepath.Join(dir, "mod.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	start, block, identity := inlineModBuild(raw)
	if block != nil {
		result := append([]byte{}, raw[:start]...)
		result = append(result, identity...)
		result = append(result, raw[start+len(block):]...)
		return p.putBytes(path, result, "Mods", true)
	}
	return nil
}

func (p *migrationPlan) modLibrary(root, destination string, overrides bool) error {
	return walkLibrary(root, func(dir string, e fs.DirEntry, _ error) error {
		if !e.IsDir() || dir == root {
			return nil
		}
		if strings.HasPrefix(e.Name(), ".") || modWorkFolder.MatchString(e.Name()) ||
			(filepath.Dir(dir) == root && strings.EqualFold(e.Name(), "maps")) {
			return filepath.SkipDir
		}
		doc, descriptor, err := earlierDescriptor(dir)
		if err != nil {
			if !earlierPathExists(filepath.Join(dir, "assets")) {
				return filepath.SkipDir
			}
			return err
		}
		if doc == nil {
			return nil
		}
		_, hasID := doc["id"].(string)
		_, build := doc["snapmap-plus"].(map[string]any)
		assets := earlierPathExists(filepath.Join(dir, "assets"))
		pruned := prunedAuthorPaths(doc)
		previous := (assets || len(pruned.paths) != 0) && (hasID || build)
		if descriptor == "package.json" {
			recognized := false
			for _, leaf := range []string{"decls", "images", "resources", "strings", "shaders", "hud", "requirements", "assets"} {
				if earlierPathExists(filepath.Join(dir, leaf)) {
					recognized = true
					break
				}
			}
			previous = recognized && (hasID || overrides)
		}
		if previous {
			rel, err := filepath.Rel(root, dir)
			if err != nil {
				return err
			}
			if err = p.convertMod(dir, filepath.Join(destination, rel), pruned); err != nil {
				return fmt.Errorf("%s: %w", dir, err)
			}
		} else if descriptor == "mod.json" && !overrides && samePath(root, destination) {
			if err = p.authored(dir); err != nil {
				return err
			}
		}
		return filepath.SkipDir
	})
}

func (p *migrationPlan) moveCache(src, dst string) error {
	if !plainPath(src) || !plainPath(dst) {
		return fmt.Errorf("linked cache path %s", src)
	}
	if _, err := os.Lstat(src); errors.Is(err, fs.ErrNotExist) {
		return nil
	} else if err != nil {
		return err
	}
	return filepath.WalkDir(src, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(path) {
			return fmt.Errorf("linked cache entry %s", path)
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)
		if e.IsDir() {
			if stat, e := os.Lstat(target); e == nil && !stat.IsDir() {
				return fmt.Errorf("a file occupies the cache directory %s", target)
			} else if e != nil && !os.IsNotExist(e) {
				return e
			}
			p.empty(path)
			return nil
		}
		if filepath.Base(path) == "mod.json" && strings.EqualFold(filepath.Base(filepath.Dir(dst)), "cache") && strings.EqualFold(filepath.Base(dst), "content") && strings.HasPrefix(strings.ToLower(filepath.ToSlash(rel)), "served/") {
			data, err := os.ReadFile(path)
			if err != nil {
				return err
			}
			start, block, identity := inlineModBuild(data)
			if block != nil {
				if err = p.putBytes(filepath.Join(filepath.Dir(target), ".build", "manifest.json"), block, "Mods", false); err != nil {
					return err
				}
				out := append([]byte{}, data[:start]...)
				out = append(out, identity...)
				out = append(out, data[start+len(block):]...)
				if err = p.putBytes(target, out, "Mods", false); err != nil {
					return err
				}
				return p.remove(path, "Mods")
			}
		}
		existing, err := migrationFileHash(target)
		if err != nil {
			return err
		}
		h, err := migrationFileHash(path)
		if err != nil {
			return err
		}
		if existing == "" {
			key := strings.ToLower(target)
			if previous, present := p.destinations[key]; present {
				if p.Changes[previous].After == h {
					return p.remove(path, "Mods")
				}
				return fmt.Errorf("two cache entries would use %s", target)
			}
			p.destinations[key] = len(p.Changes)
			p.Changes = append(p.Changes, migrationChange{Path: target, After: h, Stage: "Mods", Move: path})
			return nil
		}
		if existing != h {
			if strings.HasSuffix(strings.ToLower(path), ".part") && strings.HasPrefix(strings.ToLower(filepath.ToSlash(rel)), "partial/") {
				old, err := os.Stat(path)
				if err != nil {
					return err
				}
				cur, err := os.Stat(target)
				if err != nil {
					return err
				}
				shorter, longer := target, path
				if old.Size() < cur.Size() {
					shorter, longer = path, target
				}
				compatible, err := isFilePrefix(shorter, longer, min(old.Size(), cur.Size()))
				if err != nil {
					return err
				}
				if compatible {
					if longer == path {
						if err = p.put(target, path, "Mods", true); err != nil {
							return err
						}
					}
					return p.remove(path, "Mods")
				}
			}
			return fmt.Errorf("different cache entries at %s and %s; choose the one to keep before migrating", path, target)
		}
		return p.remove(path, "Mods")
	})
}

func (p *migrationPlan) mods() error {
	if err := p.previousTransactions(); err != nil {
		return err
	}
	mods := filepath.Join(p.data, "mods")
	if err := p.modLibrary(mods, mods, false); err != nil {
		return err
	}
	for _, root := range p.legacyHomes() {
		if !p.keepOverrides {
			if err := p.modLibrary(filepath.Join(root, "overrides"), mods, true); err != nil {
				return err
			}
			if err := p.loose(root); err != nil {
				return err
			}
		}
		if !samePath(root, p.data) {
			if err := p.modLibrary(filepath.Join(root, "mods"), mods, false); err != nil {
				return err
			}
		}
	}
	for _, name := range []string{"served", "content"} {
		if err := p.moveCache(filepath.Join(mods, "."+name), filepath.Join(p.data, "cache", name)); err != nil {
			return err
		}
	}
	served := filepath.Join(p.data, "cache", "content", "served")
	entries, err := os.ReadDir(served)
	if err != nil && !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	for _, child := range entries {
		if child.IsDir() {
			if err = p.splitAuthor(filepath.Join(served, child.Name())); err != nil {
				return err
			}
		}
	}
	history := filepath.Join(mods, ".builds")
	entries, err = os.ReadDir(history)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, child := range entries {
		dir := filepath.Join(history, child.Name())
		if !child.IsDir() || !plainPath(dir) {
			continue
		}
		known := false
		if file := filepath.Join(dir, "mod.json"); isRegular(file) && plainPath(file) {
			b, e := os.ReadFile(file)
			if e != nil {
				return e
			}
			_, block, _ := inlineModBuild(b)
			known = block != nil
		}
		if !known {
			_, e := p.pins.inspect(dir, false)
			known = e == nil
		}
		if !known {
			continue
		}
		if err = p.moveCache(dir, filepath.Join(p.data, "cache", "legacy-builds", child.Name())); err != nil {
			return err
		}
	}
	p.empty(history)
	return nil
}

func isFilePrefix(short, long string, size int64) (bool, error) {
	expected, err := fileSHA256(short)
	if err != nil {
		return false, err
	}
	f, err := os.Open(long)
	if err != nil {
		return false, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err = io.CopyN(h, f, size); err != nil {
		return false, err
	}
	return fmt.Sprintf("%x", h.Sum(nil)) == expected, nil
}

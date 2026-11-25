package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func (p *earlierPins) ready() error {
	if p.temp == "" {
		var err error
		p.temp, err = os.MkdirTemp("", "snapmap-plus-units-*")
		if err != nil {
			return err
		}
	}
	if p.tool == "" {
		if len(earlierUnitBinary) == 0 {
			return fmt.Errorf("the installer must be built with its mod unit helper")
		}
		tool := filepath.Join(p.temp, "units.exe")
		if err := os.WriteFile(tool, earlierUnitBinary, 0700); err != nil {
			return err
		}
		p.tool = tool
	}
	if p.doom == "" {
		p.doom, _ = resolveDoom("")
	}
	return nil
}
func (p *earlierPins) run(work string, args ...string) (string, error) {
	cmd := exec.Command(p.tool, append(args, "doom="+p.doom)...)
	cmd.Dir = work
	out, err := cmd.CombinedOutput()
	return string(out), err
}
func (p *earlierPins) stage(b *earlierBundle, work string) (string, error) {
	if b.assets && b.inline {
		var doc earlierObject
		if err := json.Unmarshal(b.descriptor, &doc); err != nil {
			return "", err
		}
		doc["snapmap-plus"] = b.block
		if err := os.WriteFile(filepath.Join(work, "original.json"), pinJSON(doc), 0600); err != nil {
			return "", err
		}
		if _, err := p.run(work, "verify="+b.hash); err != nil {
			return "", fmt.Errorf("earlier author and build data do not match %s", b.hash)
		}
	}
	dir := "mod"
	if b.assets {
		dir = "old"
	}
	root := filepath.Join(work, dir)
	if e := os.MkdirAll(root, 0755); e != nil {
		return "", e
	}
	if b.assets {
		if e := os.Mkdir(filepath.Join(root, "assets"), 0755); e != nil {
			return "", e
		}
	}
	for _, f := range b.files {
		dest := filepath.Join(root, filepath.FromSlash(f.Path))
		if f.Stock != nil {
			if f.Closure || !b.assets {
				continue
			}
			stock := filepath.Join(work, "stock.bin")
			if _, e := p.run(work, "stock="+f.Stock.SHA256, "source="+f.Stock.Source); e != nil {
				return "", fmt.Errorf("installed game file %s does not match this build", f.Stock.Source)
			}
			r, e := os.Open(stock)
			if e != nil {
				return "", e
			}
			if e = copyPinFile(r, dest, f.Stock.SHA256, -1); e != nil {
				return "", e
			}
		} else if e := p.extract(b, f, dest); e != nil {
			return "", e
		}
	}
	filename := "mod.json"
	descriptor := b.descriptor
	if !b.inline {
		filename = "bundle.json"
		var doc earlierObject
		if err := json.Unmarshal(descriptor, &doc); err != nil {
			return "", err
		}
		var extra earlierObject
		if raw := doc["extra"]; raw != nil {
			if err := json.Unmarshal(raw, &extra); err != nil {
				return "", err
			}
			delete(doc, "extra")
			for k, v := range extra {
				if doc[k] != nil {
					return "", fmt.Errorf("ambiguous earlier author field %s", k)
				}
				doc[k] = v
			}
		}
		descriptor = pinJSON(doc)
	}
	if e := os.WriteFile(filepath.Join(root, filename), descriptor, 0644); e != nil {
		return "", e
	}
	if b.assets {
		salt, e := translateEarlierMod(root, filepath.Join(work, "mod"))
		if e != nil {
			return "", e
		}
		return salt, nil
	}
	side := filepath.Join(root, ".build")
	if e := os.Mkdir(side, 0755); e != nil {
		return "", e
	}
	if e := os.WriteFile(filepath.Join(side, "manifest.json"), b.block, 0644); e != nil {
		return "", e
	}
	return b.id, nil
}

func (p *earlierPins) row(row earlierObject) (earlierObject, error) {
	if err := p.index(); err != nil {
		return nil, err
	}
	id, hash, salted := strings.Cut(pinString(row, "mod"), "@")
	version := pinString(row, "version")
	if !salted {
		id, hash = pinString(row, "id"), pinString(row, "bundle")
	}
	if !pinHash(hash) || !earlierID.MatchString(id) || (salted && !modSalt.MatchString(id)) {
		return nil, fmt.Errorf("invalid earlier mod identity %q", id)
	}
	var requestedBuild int64
	if row["build"] != nil && json.Unmarshal(row["build"], &requestedBuild) != nil {
		return nil, fmt.Errorf("invalid earlier build number")
	}
	var names []string
	var attached bool
	if row["names"] != nil {
		if e := json.Unmarshal(row["names"], &names); e != nil {
			return nil, e
		}
	}
	if row["attached"] != nil {
		if e := json.Unmarshal(row["attached"], &attached); e != nil {
			return nil, e
		}
	}
	var last error
	for _, b := range p.candidates[hash] {
		if !strings.EqualFold(b.id, id) || salted != b.inline || (!salted && version != b.version) || (row["build"] != nil && requestedBuild != b.build) {
			continue
		}
		if err := p.ready(); err != nil {
			return nil, err
		}
		if b.staged == "" {
			work, err := os.MkdirTemp(p.temp, "build-*")
			if err != nil {
				return nil, err
			}
			salt, err := p.stage(b, work)
			if err != nil {
				last = err
				continue
			}
			b.staged, b.salt = work, salt
			if b.inline && !b.assets {
				b.built = b.hash
			}
		}
		work, salt := b.staged, b.salt
		display := pinString(row, "name")
		if display == "" {
			var doc earlierObject
			_ = json.Unmarshal(b.descriptor, &doc)
			display = pinString(doc, "name")
		}
		translated := make([]string, len(names))
		for i, s := range names {
			translated[i] = strings.ReplaceAll(s, "mods/"+id+"/", "mods/"+salt+"/")
		}
		next, err := p.slice(work, salt, display, translated, attached, b.built)
		if err != nil {
			last = err
			continue
		}
		if b.built == "" {
			raw, err := os.ReadFile(filepath.Join(work, "mod", ".build", "manifest.json"))
			if err != nil {
				return nil, err
			}
			var side earlierObject
			if json.Unmarshal(raw, &side) != nil || !pinHash(pinString(side, "build")) {
				return nil, fmt.Errorf("unreadable generated build")
			}
			b.built = pinString(side, "build")
		}
		for _, k := range []string{"choices", "labels", "modules"} {
			if row[k] != nil {
				next[k] = row[k]
			}
		}
		if b.inZIP {
			p.consumed[b.folder] = true
			expectedPaths := []string{"bundle.canonical.json", "bundle.json"}
			if b.originalFile != "" {
				expectedPaths = append(expectedPaths, b.originalFile)
			}
			if b.inline {
				expectedPaths = []string{"mod.json", ".build/manifest.json"}
			}
			for _, f := range b.files {
				if f.Stock == nil {
					expectedPaths = append(expectedPaths, f.Path)
				}
			}
			for _, rel := range expectedPaths {
				p.observed[strings.ToLower(b.folder+"/"+rel)] = true
			}
		}
		return next, nil
	}
	if last == nil {
		last = fmt.Errorf("the original build files are unavailable")
	}
	return nil, fmt.Errorf("%s @ %s: %w", id, hash, last)
}

// The caller has verified and translated this embedded package in a private folder.
func (p *earlierPins) pinCarried(folder, id, display string, names []string) (earlierObject, error) {
	if err := p.ready(); err != nil {
		return nil, err
	}
	work, err := os.MkdirTemp(p.temp, "package-*")
	if err != nil {
		return nil, err
	}
	if err = copyEarlierPinTree(folder, filepath.Join(work, "mod")); err != nil {
		return nil, err
	}
	doc, _, err := earlierDescriptor(filepath.Join(work, "mod"))
	if err != nil {
		return nil, err
	}
	_, salt, err := earlierModIdentity(doc)
	if err != nil {
		return nil, err
	}
	if id != salt && earlierSalt(id) != salt {
		return nil, fmt.Errorf("embedded package has a different namespace")
	}
	roots := make([]string, len(names))
	for i, s := range names {
		roots[i] = strings.ReplaceAll(s, "mods/"+id+"/", "mods/"+salt+"/")
	}
	return p.slice(work, salt, display, roots, false, "")
}
func copyEarlierPinTree(src, dst string) error {
	return filepath.WalkDir(src, func(name string, e os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(name) {
			return fmt.Errorf("linked package file %s", name)
		}
		rel, err := filepath.Rel(src, name)
		if err != nil {
			return err
		}
		to := filepath.Join(dst, rel)
		if e.IsDir() {
			return os.Mkdir(to, 0755)
		}
		if !e.Type().IsRegular() {
			return fmt.Errorf("invalid package file %s", name)
		}
		return copyFile(name, to)
	})
}
func (p *earlierPins) slice(work, salt, display string, names []string, attached bool, expected string) (earlierObject, error) {
	roots := []string{"*"}
	seen := map[string]bool{"*": true}
	for _, name := range names {
		if name == "" || seen[name] {
			continue
		}
		if strings.ContainsAny(name, "\r\n\t\x00") {
			return nil, fmt.Errorf("invalid earlier object name %q", name)
		}
		seen[name] = true
		roots = append(roots, name)
	}
	if err := os.WriteFile(filepath.Join(work, "roots.txt"), []byte(strings.Join(append(roots[1:], "*"), "\n")), 0600); err != nil {
		return nil, err
	}
	args := []string{"mod=mod", "out=out", "names=@lines"}
	if !attached && len(roots) > 1 {
		args = append(args, "fallback=1")
	}
	if expected != "" {
		args = append(args, "load=1")
	}
	output, runErr := p.run(work, args...)
	found := map[string]string{}
	built := false
	for _, line := range strings.Split(output, "\n") {
		line = strings.TrimSuffix(line, "\r")
		columns := strings.Split(line, "\t")
		if strings.HasPrefix(line, "build ") && len(columns) >= 2 {
			built = columns[0] == "build "+salt && pinHash(columns[1]) && (expected == "" || expected == columns[1])
		}
		if strings.HasPrefix(line, "unit ") && len(columns) == 7 && pinHash(columns[1]) {
			found[strings.TrimPrefix(columns[0], "unit ")] = columns[1]
		}
	}
	if !built {
		return nil, fmt.Errorf("the earlier build could not be verified and sliced: %s", strings.TrimSpace(output))
	}
	selected := []string{}
	whole := attached || len(roots) == 1
	for _, name := range roots[1:] {
		h := found[name]
		if h == "" {
			selected = nil
			whole = true
			break
		}
		selected = append(selected, h)
	}
	if whole {
		h := found["*"]
		if h == "" {
			return nil, fmt.Errorf("the earlier build cannot supply its whole mod: %s", strings.TrimSpace(output))
		}
		selected = append(selected, h)
	}
	if runErr != nil && len(selected) == 0 {
		return nil, runErr
	}
	row := earlierObject{"mod": pinJSON(salt), "name": pinJSON(display)}
	objects := map[string]string{}
	covered := map[string]bool{}
	var size int64
	for _, hash := range selected {
		manifest := filepath.Join(work, "out", "units", hash+".json")
		raw, err := os.ReadFile(manifest)
		if err != nil {
			return nil, err
		}
		if fmt.Sprintf("%x", sha256.Sum256(raw)) != hash {
			return nil, fmt.Errorf("unit manifest differs from %s", hash)
		}
		var unit struct {
			Files []earlierBlob
			Root  struct{ Name string }
		}
		if err = json.Unmarshal(raw, &unit); err != nil || unit.Root.Name == "" {
			return nil, fmt.Errorf("invalid generated unit %s", hash)
		}
		if unit.Root.Name == "*" {
			row["whole"] = pinJSON(hash)
		} else {
			objects[unit.Root.Name] = hash
		}
		for _, f := range unit.Files {
			if !pinHash(f.SHA256) || !pinRelative(f.Path) || f.Size < 0 {
				return nil, fmt.Errorf("invalid generated file %s", f.Path)
			}
			if covered[f.SHA256] {
				continue
			}
			if err = p.store(filepath.Join(work, "mod", filepath.FromSlash(f.Path)), f.SHA256, f.Size, false); err != nil {
				return nil, err
			}
			covered[f.SHA256] = true
			size += f.Size
		}
		if err = p.store(manifest, hash, int64(len(raw)), true); err != nil {
			return nil, err
		}
	}
	row["objects"], row["size"] = pinJSON(objects), pinJSON(size)
	return row, nil
}
func (p *earlierPins) store(file, hash string, size int64, unit bool) error {
	entry, rel := "files/"+hash, "files/"+hash[:2]+"/"+hash
	if unit {
		entry = "units/" + hash + ".json"
		rel = entry
	}
	dst := filepath.Join(p.data, "cache", "content", filepath.FromSlash(rel))
	if !plainPath(dst) || !plainPath(file) {
		return fmt.Errorf("linked mod content %s", entry)
	}
	r, e := os.Open(file)
	if e != nil {
		return e
	}
	if e = copyPinFile(r, dst, hash, size); e == nil {
		p.content[entry] = dst
	}
	return e
}

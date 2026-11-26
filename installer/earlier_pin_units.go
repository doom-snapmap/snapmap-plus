package main

import (
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
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

// Read the installed archives once for all the original stock sources in this folder.
func (p *earlierPins) stockFiles(work string, files []earlierBlob) (map[string]string, error) {
	paths := map[string]string{}
	if len(files) == 0 {
		return paths, nil
	}
	if err := p.ready(); err != nil {
		return nil, err
	}
	dir, err := os.MkdirTemp(work, "stock-")
	if err != nil {
		return nil, err
	}
	var requests strings.Builder
	for _, f := range files {
		hash, source := f.Stock.SHA256, f.Stock.Source
		if !pinHash(hash) || !pinRelative(source) || strings.ContainsAny(source, "\r\n\t") {
			return nil, fmt.Errorf("invalid installed-game pointer for %s", f.Path)
		}
		if paths[hash] == "" {
			fmt.Fprintf(&requests, "%s\t%s\n", hash, source)
			paths[hash] = filepath.Join(dir, hash)
		}
	}
	if err = os.WriteFile(filepath.Join(dir, "requests.txt"), []byte(requests.String()), 0600); err != nil {
		return nil, err
	}
	if out, err := p.run(dir, "stocks=1"); err != nil {
		return nil, fmt.Errorf("could not restore the original stock files: %s (%w)", strings.TrimSpace(out), err)
	}
	return paths, nil
}

// Some betas removed the author's stock copies after recording their hashes in mod.json.
type prunedStock struct {
	build string
	paths map[string]bool
}

func prunedAuthorPaths(doc map[string]any) prunedStock {
	block, _ := doc["snapmap-plus"].(map[string]any)
	hash, _ := block["bundle"].(string)
	if !pinHash(hash) {
		return prunedStock{}
	}
	rows, _ := block["stock"].([]any)
	paths := map[string]bool{}
	for _, item := range rows {
		row, _ := item.(map[string]any)
		path, _ := row["path"].(string)
		if row["inAssets"] == false && row["closure"] != true {
			paths[path] = true
		}
	}
	return prunedStock{hash, paths}
}

func (p *earlierPins) prunedAuthorFiles(src, work string, b *earlierBundle, removed prunedStock) (map[string]string, error) {
	if len(removed.paths) == 0 {
		return nil, nil
	}
	if b == nil || !b.assets || b.hash != removed.build {
		return nil, fmt.Errorf("could not verify the earlier build of the stock files in %s", src)
	}
	var missing []earlierBlob
	for _, f := range b.files {
		if !removed.paths[f.Path] || f.Stock == nil || f.Closure {
			continue
		}
		path := filepath.Join(src, filepath.FromSlash(f.Path))
		if _, err := os.Lstat(path); err == nil {
			continue
		} else if !os.IsNotExist(err) {
			return nil, err
		}
		missing = append(missing, f)
	}
	sources, err := p.stockFiles(work, missing)
	if err != nil {
		return nil, err
	}
	recovered := map[string]string{}
	for _, f := range missing {
		recovered[f.Request] = sources[f.Stock.SHA256]
	}
	return recovered, nil
}

func (p *earlierPins) verifyInline(b *earlierBundle, work string) error {
	if err := p.ready(); err != nil {
		return err
	}
	var doc earlierObject
	if err := json.Unmarshal(b.descriptor, &doc); err != nil {
		return err
	}
	doc["snapmap-plus"] = b.block
	if err := os.WriteFile(filepath.Join(work, "original.json"), pinJSON(doc), 0600); err != nil {
		return err
	}
	if _, err := p.run(work, "verify="+b.hash); err != nil {
		return fmt.Errorf("earlier author and build data do not match %s", b.hash)
	}
	return nil
}

func (p *earlierPins) stage(b *earlierBundle, work string) (string, error) {
	if b.assets && b.inline {
		if err := p.verifyInline(b, work); err != nil {
			return "", err
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
	var wanted []earlierBlob
	for _, f := range b.files {
		if b.assets && f.Stock != nil && !f.Closure {
			wanted = append(wanted, f)
		}
	}
	sources, err := p.stockFiles(work, wanted)
	if err != nil {
		return "", err
	}
	for _, f := range b.files {
		dest := filepath.Join(root, filepath.FromSlash(f.Path))
		if f.Stock != nil {
			if f.Closure || !b.assets {
				continue
			}
			r, e := os.Open(sources[f.Stock.SHA256])
			if e != nil {
				return "", e
			}
			if e := copyPinFile(r, dest, f.Stock.SHA256, -1); e != nil {
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
		if !b.inline {
			var e error
			b.added, e = p.globalNames(b, work)
			if e != nil {
				return "", e
			}
		}
		salt, e := translateEarlierModWithNames(root, filepath.Join(work, "mod"), b)
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

type missingEarlierPin struct {
	mod, build, reason string
}

type missingEarlierPins struct {
	items   []missingEarlierPin
	sources []string
}

func (e *missingEarlierPins) Error() string {
	items := make([]string, 0, len(e.items))
	for _, pin := range e.items {
		item := pin.mod + " @ " + pin.build
		if pin.reason != "" {
			item += " (" + pin.reason + ")"
		}
		items = append(items, item)
	}
	return "the original build files are unavailable: " + strings.Join(items, ", ")
}

func (p *earlierPins) candidatesForRow(row earlierObject) (string, []*earlierBundle, error) {
	if err := p.index(); err != nil {
		return "", nil, err
	}
	id, hash, salted := strings.Cut(pinString(row, "mod"), "@")
	version := pinString(row, "version")
	if !salted {
		id, hash = pinString(row, "id"), pinString(row, "bundle")
	}
	if !pinHash(hash) || !earlierID.MatchString(id) || (salted && !modSalt.MatchString(id)) {
		return "", nil, fmt.Errorf("invalid earlier mod identity %q", id)
	}
	var requestedBuild int64
	if row["build"] != nil && json.Unmarshal(row["build"], &requestedBuild) != nil {
		return "", nil, fmt.Errorf("invalid earlier build number")
	}

	var names []string
	var attached bool
	if raw := row["names"]; raw != nil && json.Unmarshal(raw, &names) != nil {
		return "", nil, fmt.Errorf("invalid earlier object names")
	}
	if raw := row["attached"]; raw != nil && json.Unmarshal(raw, &attached) != nil {
		return "", nil, fmt.Errorf("invalid earlier attachment")
	}
	var found []*earlierBundle
	for _, b := range p.candidates[hash] {
		if strings.EqualFold(b.id, id) && salted == b.inline && (salted || version == b.version) && (row["build"] == nil || requestedBuild == b.build) {
			found = append(found, b)
		}
	}
	if len(found) == 0 {
		if len(p.candidates[hash]) != 0 {
			return "", nil, fmt.Errorf("the pinned identity disagrees with the original build %s", hash)
		}
		return "", nil, &missingEarlierPins{items: []missingEarlierPin{{mod: id, build: hash}}}
	}
	return id, found, nil
}

type earlierRowCheck func([]earlierObject) error

func (p *earlierPins) checkRows(rows []earlierObject) error {
	var missing []missingEarlierPin
	for _, row := range rows {
		mod := pinString(row, "mod")
		salt, hash, salted := strings.Cut(mod, "@")
		if !salted && row["bundle"] == nil && row["id"] == nil {
			continue
		}
		if salted && modSalt.MatchString(salt) && hash == strings.Repeat("0", 64) && row["id"] == nil && row["bundle"] == nil {
			continue
		}
		if stored, e := p.retryFile(row); e != nil {
			return e
		} else if stored != "" {
			continue
		}
		_, _, err := p.candidatesForRow(row)
		if err == nil {
			continue
		}
		var absent *missingEarlierPins
		if !errors.As(err, &absent) {
			return err
		}
		missing = append(missing, absent.items...)
	}
	if len(missing) > 0 {
		slices.SortFunc(missing, func(a, b missingEarlierPin) int {
			if a.mod != b.mod {
				return strings.Compare(a.mod, b.mod)
			}
			return strings.Compare(a.build, b.build)
		})
		return &missingEarlierPins{items: slices.Compact(missing)}
	}
	return nil
}

func (p *earlierPins) row(row earlierObject) (earlierObject, error) {
	id, candidates, err := p.candidatesForRow(row)
	if err != nil {
		return nil, err
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
	for _, b := range candidates {
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
			if f, _, ok := b.added.at(s, "entitydef"); ok {
				translated[i] = f.engine
			}
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
	return nil, &missingEarlierPins{items: []missingEarlierPin{{mod: id, build: candidates[0].hash, reason: last.Error()}}}
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
			return os.MkdirAll(to, 0755)
		}
		if !e.Type().IsRegular() {
			return fmt.Errorf("invalid package file %s", name)
		}
		if isRegular(to) {
			if identicalFiles(name, to) {
				return nil
			}
			return fmt.Errorf("different files provide %s", rel)
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
	dst := filepath.Join(p.output, "cache", "content", filepath.FromSlash(rel))
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

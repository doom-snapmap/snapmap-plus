package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
)

type earlierBlob struct {
	Path, Request, Blob, SHA256 string
	Size                        int64
	Stock                       *struct{ SHA256, Source string }
	Source                      string
	Closure                     bool
}
type earlierBundle struct {
	originalFile              string
	staged, salt, built       string
	folder, id, version, hash string
	build                     int64
	inZIP, assets, inline     bool
	descriptor, block         []byte
	files                     []earlierBlob
}
type earlierBlobSource struct {
	bundle *earlierBundle
	path   string
}
type earlierPins struct {
	blobs                  map[string][]earlierBlobSource
	data, doom, temp, tool string
	archive                *zip.Reader
	entries                map[string]*zip.File
	carried                map[string]*earlierBundle
	candidates             map[string][]*earlierBundle
	consumed               map[string]bool
	content                map[string]string
	observed               map[string]bool
	indexed                bool
	scanErr                error
}

func newEarlierPins(data, doom string, z *zip.Reader) *earlierPins {
	return &earlierPins{
		data: data, doom: doom, archive: z,
		entries: map[string]*zip.File{}, carried: map[string]*earlierBundle{}, candidates: map[string][]*earlierBundle{},
		consumed: map[string]bool{}, content: map[string]string{}, observed: map[string]bool{}, blobs: map[string][]earlierBlobSource{},
	}
}
func (p *earlierPins) close() {
	if p.temp != "" {
		os.RemoveAll(p.temp)
	}
}
func pinString(row earlierObject, key string) string {
	var s string
	_ = json.Unmarshal(row[key], &s)
	return s
}
func pinJSON(value any) json.RawMessage { b, _ := json.Marshal(value); return b }
func pinHash(s string) bool {
	if len(s) != 64 || s != strings.ToLower(s) {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}
func pinRelative(s string) bool {
	return s != "." && fs.ValidPath(s) && !strings.ContainsAny(s, "\\:\x00") && filepath.IsLocal(filepath.FromSlash(s))
}
func (p *earlierPins) open(b *earlierBundle, rel string) (io.ReadCloser, error) {
	if !pinRelative(rel) {
		return nil, fmt.Errorf("invalid mod file %q", rel)
	}
	if b.inZIP {
		f := p.entries[strings.ToLower(path.Join(b.folder, rel))]
		if f == nil {
			return nil, fs.ErrNotExist
		}
		return f.Open()
	}
	name := filepath.Join(b.folder, filepath.FromSlash(rel))
	if !plainPath(name) || !isRegular(name) {
		return nil, fs.ErrNotExist
	}
	return os.Open(name)
}
func (p *earlierPins) read(b *earlierBundle, rel string) ([]byte, error) {
	r, err := p.open(b, rel)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}
func (p *earlierPins) inspect(folder string, inZIP bool) (*earlierBundle, error) {
	b := &earlierBundle{folder: folder, inZIP: inZIP}
	canon, err := p.read(b, "bundle.canonical.json")
	if errors.Is(err, fs.ErrNotExist) {
		var doc, block earlierObject
		if original, e := p.read(b, "mod.json"); e == nil && json.Unmarshal(original, &doc) == nil && json.Unmarshal(doc["snapmap-plus"], &block) == nil && pinHash(pinString(block, "bundle")) {
			if string(block["validated"]) == "false" || pinString(block, "cause") != "" {
				return nil, fmt.Errorf("the earlier mod was not ready to use")
			}
			canon, err = preSaltCanonical(doc, block)
			if err != nil {
				return nil, err
			}
			b.originalFile = "mod.json"
		}
	}

	if err == nil {
		var meta struct {
			Format      int
			ID, Version string
			Build       int64
			Files       []earlierBlob
		}
		if err = json.Unmarshal(canon, &meta); err != nil {
			return nil, err
		}
		if meta.Format != 1 || !earlierID.MatchString(meta.ID) {
			return nil, fmt.Errorf("unknown earlier bundle in %s", folder)
		}
		b.id, b.version, b.build, b.files, b.assets = meta.ID, meta.Version, meta.Build, meta.Files, true
		b.hash = fmt.Sprintf("%x", sha256.Sum256(canon))
		b.descriptor = canon
		pretty, err := p.read(b, "bundle.json")
		if err == nil {
			var original, duplicate map[string]any
			a, d := json.NewDecoder(bytes.NewReader(canon)), json.NewDecoder(bytes.NewReader(pretty))
			a.UseNumber()
			d.UseNumber()
			if a.Decode(&original) != nil || d.Decode(&duplicate) != nil || duplicate["bundle"] != b.hash {
				return nil, fmt.Errorf("bundle.json disagrees with the canonical build in %s", folder)
			}
			delete(duplicate, "bundle")
			if !reflect.DeepEqual(original, duplicate) {
				return nil, fmt.Errorf("bundle.json disagrees with the canonical build in %s", folder)
			}
		} else if !errors.Is(err, fs.ErrNotExist) {
			return nil, err
		}
	} else {
		if !errors.Is(err, fs.ErrNotExist) {
			return nil, err
		}
		doc, err := p.read(b, "mod.json")
		if err != nil {
			return nil, err
		}
		var descriptor earlierObject
		if err = json.Unmarshal(doc, &descriptor); err != nil {
			return nil, err
		}
		block := descriptor["snapmap-plus"]
		var info struct {
			Salt, Build  string
			Format       int
			Files, Stock []earlierBlob
			Cause        *string
		}
		if json.Unmarshal(block, &info) != nil {
			return nil, fs.ErrNotExist
		}
		if info.Build == "" {
			salt := info.Salt
			block, err = p.read(b, ".build/manifest.json")
			if err != nil {
				return nil, err
			}
			if err = json.Unmarshal(block, &info); err != nil || info.Salt != salt {
				return nil, fmt.Errorf("mismatched build for %s", folder)
			}
		}
		if info.Cause != nil || !modSalt.MatchString(info.Salt) || info.Format != 2 || !pinHash(info.Build) {
			return nil, fmt.Errorf("unbuilt mod in %s", folder)
		}
		for _, s := range info.Stock {
			s.Stock = &struct{ SHA256, Source string }{s.SHA256, s.Source}
			info.Files = append(info.Files, s)
		}
		b.id, b.hash, b.block, b.descriptor, b.inline, b.files = info.Salt, info.Build, block, doc, true, info.Files
		for _, f := range b.files {
			if strings.HasPrefix(f.Path, "assets/") {
				b.assets = true
			}
		}

	}
	seen := map[string]bool{}
	for _, f := range b.files {
		if !b.inline && f.Request != strings.TrimPrefix(f.Path, "assets/") {
			return nil, fmt.Errorf("file %s names a different game request", f.Path)
		}
		k := strings.ToLower(f.Path)
		if !pinRelative(f.Path) || seen[k] || (b.assets && !strings.HasPrefix(f.Path, "assets/")) || k == "mod.json" || strings.HasPrefix(k, ".build/") {
			return nil, fmt.Errorf("invalid file in %s: %q", folder, f.Path)
		}
		seen[k] = true
		if f.Stock == nil && (!pinHash(f.hash()) || f.Size < 0) {
			return nil, fmt.Errorf("invalid file identity for %s", f.Path)
		}
		if f.Stock != nil && (!pinHash(f.Stock.SHA256) || !pinRelative(f.Stock.Source)) {
			return nil, fmt.Errorf("invalid installed-game pointer for %s", f.Path)
		}
	}
	return b, nil
}
func (f earlierBlob) hash() string {
	if f.Blob != "" {
		return f.Blob
	}
	return f.SHA256
}
func (p *earlierPins) index() error {
	if p.indexed {
		return p.scanErr
	}
	p.indexed = true
	p.scanErr = p.find()
	return p.scanErr
}
func (p *earlierPins) find() error {
	add := func(b *earlierBundle) {
		p.candidates[b.hash] = append(p.candidates[b.hash], b)
		for _, f := range b.files {
			if f.Stock == nil {
				p.blobs[f.hash()] = append(p.blobs[f.hash()], earlierBlobSource{b, f.Path})
			}
		}
		if b.inZIP {
			p.carried[b.folder] = b
		}
	}
	if p.archive != nil {
		folders := map[string]bool{}
		for _, f := range p.archive.File {
			name := strings.ToLower(f.Name)
			if f.FileInfo().IsDir() {
				continue
			}
			if p.entries[name] != nil {
				return fmt.Errorf("duplicate ZIP entry %s", f.Name)
			}
			p.entries[name] = f
			if strings.HasPrefix(name, "mods/") && (!pinRelative(name) || !f.Mode().IsRegular()) {
				return fmt.Errorf("invalid mod ZIP entry %s", name)
			}
			if strings.HasPrefix(name, "mods/") && (path.Base(name) == "bundle.canonical.json" || path.Base(name) == "mod.json") {
				folders[path.Dir(name)] = true
			}
		}
		for folder := range folders {
			b, err := p.inspect(folder, true)
			if err != nil {
				return fmt.Errorf("%s: %w", folder, err)
			}
			add(b)
		}
	}
	for _, sub := range []string{"mods", "cache/content/served", "cache/served", "backups/earlier-data/mod-folders"} {
		root := filepath.Join(p.data, filepath.FromSlash(sub))
		if !plainPath(root) {
			continue
		}
		err := filepath.WalkDir(root, func(dir string, e fs.DirEntry, err error) error {
			if errors.Is(err, fs.ErrNotExist) {
				return nil
			}
			if err != nil {
				return err
			}
			if !e.IsDir() {
				return nil
			}
			if !plainPath(dir) {
				return filepath.SkipDir
			}
			if isRegular(filepath.Join(dir, "mod.json")) || isRegular(filepath.Join(dir, "bundle.canonical.json")) {
				b, err := p.inspect(dir, false)
				if err == nil {
					add(b)
				} // Other installed mods may be unbuilt.
				return filepath.SkipDir
			}
			switch strings.ToLower(e.Name()) {
			case "assets", "files", "units", "partial":
				return filepath.SkipDir
			}
			return nil
		})
		if err != nil {
			return err
		}
	}
	return nil
}
func copyPinFile(r io.ReadCloser, dst, hash string, size int64) error {
	defer r.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	temp, err := os.CreateTemp(filepath.Dir(dst), ".unit-*")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	h := sha256.New()
	var from io.Reader = r
	if size >= 0 {
		from = io.LimitReader(r, size+1)
	}
	n, err := io.Copy(io.MultiWriter(temp, h), from)
	cerr := temp.Close()
	if err != nil {
		return err
	}
	if cerr != nil {
		return cerr
	}
	if (size >= 0 && n != size) || hex.EncodeToString(h.Sum(nil)) != hash {
		return fmt.Errorf("file differs from its recorded SHA-256 %s", hash)
	}
	if err = publishNativeCopy(temp.Name(), dst); err != nil {
		if same, e := fileSHA256(dst); e == nil && same == hash {
			return nil
		}
		return err
	}
	return nil
}
func (p *earlierPins) extract(b *earlierBundle, f earlierBlob, dst string) error {
	try := func(src *earlierBundle, rel string) error {
		r, e := p.open(src, rel)
		if e != nil {
			return e
		}
		return copyPinFile(r, dst, f.hash(), f.Size)
	}
	first := try(b, f.Path)
	if first == nil {
		return nil
	}
	// Earlier exports stored a shared blob under the first bundle that carried it.
	for _, candidate := range p.blobs[f.hash()] {
		if (candidate.bundle != b || candidate.path != f.Path) && try(candidate.bundle, candidate.path) == nil {
			return nil
		}
	}
	hash := f.hash()
	for _, root := range []string{"cache/content", "mods/.content"} {
		cache := &earlierBundle{folder: filepath.Join(p.data, filepath.FromSlash(root))}
		if try(cache, "files/"+hash[:2]+"/"+hash) == nil {
			return nil
		}
	}
	return fmt.Errorf("%s: %w", f.Path, first)
}
func (p *earlierPins) addToArchive(out *zip.Writer, existing map[string]bool) error {
	if err := p.index(); err != nil {
		return err
	}
	for folder := range p.carried {
		if !p.consumed[folder] {
			return fmt.Errorf("archive carries an unused mod build in %s", folder)
		}
	}
	for name := range p.entries {
		if (strings.HasPrefix(name, "mods/") || path.Base(name) == "bundle.canonical.json") && !p.observed[name] {
			return fmt.Errorf("unconverted mod file in archive: %s", name)
		}
	}
	if f := p.entries["export.json"]; f != nil {
		raw, err := earlierEntry(f)
		if err != nil {
			return err
		}
		var index struct {
			Bundles []struct {
				Folder, Bundle, ID, Version string
				Build                       int64
			}
		}
		if err = json.Unmarshal(raw, &index); err != nil {
			return err
		}
		for _, item := range index.Bundles {
			b := p.carried[strings.ToLower(item.Folder)]
			if b == nil || !p.consumed[b.folder] || b.hash != item.Bundle || b.id != item.ID || b.version != item.Version || b.build != item.Build {
				return fmt.Errorf("export index names unresolved build %s of %s", item.Bundle, item.ID)
			}
		}
	}
	names := make([]string, 0, len(p.content))
	for name := range p.content {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		file := p.content[name]
		hash := strings.TrimSuffix(path.Base(name), ".json")
		if p.entries[name] != nil && !existing[name] {
			return fmt.Errorf("unexpected spelling or duplicate archive entry %s", name)
		}
		if existing[name] {
			f := p.entries[name]
			if f == nil {
				return fmt.Errorf("missing existing %s", name)
			}
			r, e := f.Open()
			if e != nil {
				return e
			}
			h := sha256.New()
			_, e = io.Copy(h, r)
			r.Close()
			if e != nil || hex.EncodeToString(h.Sum(nil)) != hash {
				return fmt.Errorf("earlier archive contains a different %s", name)
			}
			continue
		}
		src, e := os.Open(file)
		if e != nil {
			return e
		}
		dst, e := out.Create(name)
		if e == nil {
			h := sha256.New()
			_, e = io.Copy(io.MultiWriter(dst, h), src)
			if e == nil && hex.EncodeToString(h.Sum(nil)) != hash {
				e = fmt.Errorf("cached file changed during export: %s", name)
			}
		}
		src.Close()
		if e != nil {
			return e
		}
	}
	return nil
}

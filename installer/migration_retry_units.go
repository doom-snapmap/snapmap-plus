package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
)

// Only a known, incomplete document needs a recovery entry. Keep the verified units of its available row.
type earlierRetry struct {
	Original earlierObject
	Row      earlierObject
	Aliases  [][6]string
}

func retryKey(row earlierObject) string { return fmt.Sprintf("%x", sha256.Sum256(pinJSON(row))) }
func (p *earlierPins) retryFile(row earlierObject) (string, error) {
	key := retryKey(row)
	root := filepath.Join(p.data, "cache", "migration-units")
	if !plainPath(root) {
		return "", nil
	}
	files, err := os.ReadDir(root)
	if os.IsNotExist(err) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	for _, entry := range files {
		name := entry.Name()
		if !strings.HasPrefix(name, key+"-") || !strings.HasSuffix(name, ".zip") {
			continue
		}
		f := filepath.Join(root, name)
		suffix := strings.TrimSuffix(strings.TrimPrefix(name, key+"-"), ".zip")
		if !pinHash(suffix) || !plainPath(f) || !isRegular(f) {
			continue
		}
		actual, e := migrationFileHash(f)
		if e != nil {
			return "", e
		}
		if actual == suffix {
			return f, nil
		}
	}
	return "", nil
}
func (p *earlierPins) restoreRetry(row earlierObject, added *earlierGlobalNames) (earlierObject, error) {
	path, err := p.retryFile(row)
	if err != nil || path == "" {
		return nil, err
	}
	return p.readRetry(path, retryKey(row), added)
}

func (p *earlierPins) readRetry(path, key string, added *earlierGlobalNames) (earlierObject, error) {
	z, err := zip.OpenReader(path)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	var record earlierRetry
	members := map[string]*zip.File{}
	for _, f := range z.File {
		if members[f.Name] != nil {
			return nil, fmt.Errorf("duplicate migration unit entry %s", f.Name)
		}
		members[f.Name] = f
	}
	doc := members["pin.json"]
	if doc == nil {
		return nil, fmt.Errorf("missing historical unit reference")
	}
	raw, err := earlierEntry(doc)
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(raw, &record); err != nil || retryKey(record.Original) != key || !historicalRow(record.Original) || !modSalt.MatchString(pinString(record.Row, "mod")) {
		return nil, fmt.Errorf("historical unit reference changed")
	}
	delete(members, "pin.json")
	for name, file := range members {
		h := strings.TrimSuffix(filepath.Base(name), ".json")
		if !pinHash(h) {
			return nil, fmt.Errorf("invalid historical unit entry %s", name)
		}
		rel := name
		switch name {
		case "units/" + h + ".json":
		case "files/" + h:
			rel = "files/" + h[:2] + "/" + h
		default:
			return nil, fmt.Errorf("unknown historical unit entry %s", name)
		}
		dest := filepath.Join(p.output, "cache", "content", filepath.FromSlash(rel))
		r, e := file.Open()
		if e != nil {
			return nil, e
		}
		if e = copyPinFile(r, dest, h, int64(file.UncompressedSize64)); e != nil {
			return nil, e
		}
		p.content[name] = dest
	}
	want, err := p.unitFiles([]earlierObject{record.Row})
	if err != nil {
		return nil, err
	}
	if len(want) == 0 || len(want) != len(members) {
		return nil, fmt.Errorf("historical unit reference has incomplete content")
	}
	for name := range want {
		if members[name] == nil {
			return nil, fmt.Errorf("historical unit reference lacks %s", name)
		}
	}
	for _, v := range record.Aliases {
		if e := added.add(earlierGlobalName{v[0], v[1], v[2], v[3], v[4], v[5]}); e != nil {
			return nil, e
		}
	}
	if p.placements != nil && p.placements.plan.Export == "" {
		plan := p.placements.plan
		if plan.usedRecovery == nil {
			plan.usedRecovery = map[string]bool{}
		}
		plan.usedRecovery[path] = true
	}
	return record.Row, nil
}

func (p *earlierPins) preserveAvailable(rows []earlierObject, missing *missingEarlierPins) error {
	if p.placements == nil || p.placements.plan.Export != "" {
		return nil
	}
	plan := p.placements.plan
	if plan.keepRecovery == nil {
		plan.keepRecovery = map[string]bool{}
	}
	for _, r := range rows {
		path, err := p.retryFile(r)
		if err != nil {
			return err
		}
		if path != "" {
			plan.keepRecovery[path] = true
			continue
		}
		_, choices, err := p.candidatesForRow(r)
		if err != nil || len(choices) == 0 {
			continue
		}
		tmp, err := os.MkdirTemp(plan.work, "row-*")
		if err != nil {
			return err
		}
		output, content := p.output, p.content
		p.output, p.content = tmp, map[string]string{}
		next, err := p.row(r)
		var files map[string]string
		if err == nil {
			files, err = p.unitFiles([]earlierObject{next})
		}
		p.output, p.content = output, content
		if err != nil {
			for _, b := range choices {
				if !b.inZIP {
					missing.sources = append(missing.sources, b.folder)
				}
			}
			continue
		}
		rec := earlierRetry{Original: r, Row: next}
		for _, b := range choices {
			if b.salt != pinString(next, "mod") {
				continue
			}
			for _, a := range b.added.files {
				rec.Aliases = append(rec.Aliases, [6]string{a.kind, a.name, a.file, a.reference, a.engine, a.build})
			}
			break
		}
		sort.Slice(rec.Aliases, func(i, j int) bool { return slices.Compare(rec.Aliases[i][:], rec.Aliases[j][:]) < 0 })
		file, err := os.CreateTemp(plan.work, "recovery-*.zip")
		if err != nil {
			return err
		}
		err = writeUnitArchive(file, "pin.json", pinJSON(rec), files)
		if err = errors.Join(err, file.Close()); err != nil {
			return err
		}
		h, err := fileSHA256(file.Name())
		if err != nil {
			return err
		}
		dst := filepath.Join(p.data, "cache", "migration-units", retryKey(r)+"-"+h+".zip")
		if err = plan.put(dst, file.Name(), "Mods", false); err != nil {
			return err
		}
		plan.keepRecovery[dst] = true
	}
	return nil
}

package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// A map outside the game's kept-map list needs the same portable content as an ordinary rawmap save.
func (p *earlierPins) unitFiles(rows []earlierObject) (map[string]string, error) {
	files := map[string]string{}
	var locate = func(rel string) (string, error) {
		for _, root := range []string{filepath.Join(p.output, "cache", "content"), filepath.Join(p.data, "cache", "content"), filepath.Join(p.data, "mods", ".content")} {
			file := filepath.Join(root, filepath.FromSlash(rel))
			if plainPath(file) && isRegular(file) {
				return file, nil
			}
		}
		return "", fmt.Errorf("missing %s", rel)
	}
	for _, row := range rows {
		var roots map[string]string
		if raw := row["objects"]; raw != nil {
			if err := json.Unmarshal(raw, &roots); err != nil {
				return nil, err
			}
		}
		hashes := []string{}
		for _, h := range roots {
			hashes = append(hashes, h)
		}
		if h := pinString(row, "whole"); h != "" {
			hashes = append(hashes, h)
		}
		for _, h := range hashes {
			if !pinHash(h) {
				return nil, fmt.Errorf("invalid pinned unit %q", h)
			}
			entry := "units/" + h + ".json"
			if files[entry] != "" {
				continue
			}
			file, err := locate(entry)
			if err != nil {
				return nil, err
			}
			body, err := os.ReadFile(file)
			if err != nil {
				return nil, err
			}
			if fmt.Sprintf("%x", sha256.Sum256(body)) != h {
				return nil, fmt.Errorf("invalid pinned unit %s", h)
			}
			var unit struct{ Files []earlierBlob }
			if err = json.Unmarshal(body, &unit); err != nil {
				return nil, err
			}
			files[entry] = file
			for _, blob := range unit.Files {
				if !pinHash(blob.SHA256) {
					return nil, fmt.Errorf("invalid file in unit %s", h)
				}
				n := "files/" + blob.SHA256
				if files[n] != "" {
					continue
				}
				file, err := locate("files/" + blob.SHA256[:2] + "/" + blob.SHA256)
				if err != nil {
					return nil, err
				}
				files[n] = file
			}
		}
	}
	return files, nil
}

func currentRows(body []byte) ([]earlierObject, error) {
	var root earlierObject
	if err := json.Unmarshal(earlierJSONText(bytes.TrimPrefix(body, []byte{0xef, 0xbb, 0xbf})), &root); err != nil {
		return nil, err
	}
	for _, k := range []string{"snapmap-plus", "snapmapPlus"} {
		if raw := root[k]; raw != nil {
			var block struct{ Mods []earlierObject }
			if err := json.Unmarshal(raw, &block); err != nil {
				return nil, err
			}
			return block.Mods, nil
		}
	}
	return nil, nil
}

func writeUnitArchive(dst io.Writer, name string, body []byte, files map[string]string) error {
	w := zip.NewWriter(dst)
	doc, err := w.Create(name)
	if err != nil {
		return err
	}
	if _, err = doc.Write(body); err != nil {
		return err
	}
	pins := earlierPins{indexed: true, content: files}
	if err = pins.addToArchive(w, nil); err != nil {
		return err
	}
	return w.Close()
}

func (p *migrationPlan) companion(path string, body []byte, stage string) (string, error) {
	if !plainPath(filepath.Dir(path)) {
		return "", fmt.Errorf("linked export directory %s", filepath.Dir(path))
	}
	rows, err := currentRows(body)
	if err != nil {
		return "", err
	}
	files, err := p.pins.unitFiles(rows)
	if err != nil {
		return "", err
	}
	if len(files) == 0 {
		return "", nil
	}
	tmp, err := os.CreateTemp(p.work, "share-*.zip")
	if err != nil {
		return "", err
	}
	defer os.Remove(tmp.Name())
	err = writeUnitArchive(tmp, filepath.Base(path), body, files)
	if err = errors.Join(err, tmp.Close()); err != nil {
		return "", err
	}
	h, err := fileSHA256(tmp.Name())
	if err != nil {
		return "", err
	}
	for i := 0; ; i++ {
		dst := companionPath(path, i)
		if !plainPath(dst) {
			if !plainPath(filepath.Dir(dst)) {
				return "", fmt.Errorf("linked export directory %s", filepath.Dir(dst))
			}
			continue
		}
		if op, ok := p.destinations[strings.ToLower(dst)]; ok {
			if p.Changes[op].After == h {
				return dst, nil
			}
			continue
		}
		if earlierPathExists(dst) {
			if isRegular(dst) {
				c, e := migrationFileHash(dst)
				if e != nil {
					return "", e
				}
				if c == h {
					return dst, nil
				}
			}
			continue
		}
		return dst, p.put(dst, tmp.Name(), stage, false)
	}
}

func companionPath(path string, i int) string {
	stem := strings.TrimSuffix(path, filepath.Ext(path))
	if i == 0 {
		return stem + ".zip"
	}
	return fmt.Sprintf("%s (updated %d).zip", stem, i)
}

type migratedNative struct {
	Source              string
	Map, MapSHA         string
	Archive, ArchiveSHA string
}

func (p *migrationPlan) nativeCopyRecord(slot, h string) (string, error) {
	if !filepath.IsAbs(slot) || !pinHash(h) {
		return "", fmt.Errorf("invalid native source identity")
	}
	key := sha256.Sum256([]byte(strings.ToLower(filepath.Clean(slot)) + "\x00" + h))
	return filepath.Join(p.data, "cache", "migrated-saves", fmt.Sprintf("%x.json", key)), nil
}
func (p *migrationPlan) knownNativeCopy(slot, sourceSHA string) (bool, error) {
	file, err := p.nativeCopyRecord(slot, sourceSHA)
	if err != nil {
		return false, err
	}
	if !plainPath(file) || !isRegular(file) {
		return false, nil
	}
	raw, err := os.ReadFile(file)
	if err != nil {
		return false, err
	}
	var record migratedNative
	if json.Unmarshal(raw, &record) != nil || record.Source != sourceSHA || !pinHash(record.MapSHA) {
		return false, nil
	}
	check := func(name, want, ext string) (bool, error) {
		if strings.ToLower(filepath.Ext(name)) != ext || filepath.Base(name) != name || !pinRelative(name) || !pinHash(want) {
			return false, nil
		}
		h, e := migrationFileHash(filepath.Join(p.data, "rawmaps", "Updated saves", name))
		return h == want, e
	}
	match, err := check(record.Map, record.MapSHA, ".json")
	if !match || err != nil {
		return false, err
	}
	if record.Archive != "" {
		return check(record.Archive, record.ArchiveSHA, ".zip")
	}
	if record.ArchiveSHA != "" {
		return false, nil
	}
	body, err := os.ReadFile(filepath.Join(p.data, "rawmaps", "Updated saves", record.Map))
	if err != nil {
		return false, err
	}
	rows, err := currentRows(body)
	if err != nil {
		return false, nil
	}
	for _, row := range rows {
		if pinString(row, "whole") != "" || (row["objects"] != nil && string(row["objects"]) != "{}" && string(row["objects"]) != "null") {
			return false, nil
		}
	}
	return true, nil
}
func (p *migrationPlan) rememberNativeCopy(slot, sourceSHA, file, hash, archive string) error {
	dst, err := p.nativeCopyRecord(slot, sourceSHA)
	if err != nil {
		return err
	}
	record := migratedNative{Source: sourceSHA, Map: filepath.Base(file), MapSHA: hash}
	if archive != "" {
		record.Archive = filepath.Base(archive)
		if idx, ok := p.destinations[strings.ToLower(archive)]; ok {
			record.ArchiveSHA = p.Changes[idx].After
		} else {
			record.ArchiveSHA, err = migrationFileHash(archive)
			if err != nil {
				return err
			}
		}
	}
	if earlierPathExists(dst) {
		if !plainPath(dst) || !isRegular(dst) {
			return nil
		}
		raw, err := os.ReadFile(dst)
		if err != nil {
			return err
		}
		var previous migratedNative
		if json.Unmarshal(raw, &previous) != nil || !bytes.Equal(raw, pinJSON(previous)) || previous.Source != sourceSHA || !pinHash(previous.MapSHA) {
			return nil
		}
		return p.putBytes(dst, pinJSON(record), "Maps", true)
	}
	return p.putBytes(dst, pinJSON(record), "Maps", false)
}

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
	"path"
	"path/filepath"
	"strings"
)

var errNotSnapmapExport = errors.New("archive contains no Snapmap+ map or prefab")

func convertEarlierExport(data, filename string) error {
	if data == "" || !plainPath(data) || !plainPath(filename) || !isRegular(filename) {
		return fmt.Errorf("cannot read export %q", filename)
	}
	switch strings.ToLower(filepath.Ext(filename)) {
	case ".json":
		if strings.HasSuffix(strings.ToLower(filename), ".meta.json") {
			return fmt.Errorf("choose the prefab file beside %s", filepath.Base(filename))
		}
		pins := newEarlierPins(data, earlierInstallDir(data), nil)
		defer pins.close()
		return convertEarlierDocument(data, filename, true, pins)
	case ".zip":
		return convertEarlierArchive(data, earlierInstallDir(data), filename)
	default:
		return fmt.Errorf("choose a Snapmap+ JSON or ZIP file")
	}
}

func earlierEntry(f *zip.File) ([]byte, error) {
	r, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer r.Close()
	return io.ReadAll(r)
}

func earlierInstallDir(data string) string {
	if !strings.EqualFold(filepath.Clean(data), filepath.Clean(appDataDir())) {
		return ""
	}
	if record, err := loadRecord(); err == nil && record != nil {
		return record.DoomPath
	}
	return ""
}

func earlierBundleEntry(name string) bool {
	name = strings.ToLower(name)
	return strings.HasPrefix(name, "mods/") || path.Base(name) == "bundle.canonical.json"
}

func earlierArchiveDocument(z *zip.ReadCloser) (doc, side, index *zip.File, err error) {
	for _, f := range z.File {
		name := strings.ToLower(f.Name)
		if earlierBundleEntry(name) {
			continue
		}
		if strings.Contains(name, "/") || !strings.HasSuffix(name, ".json") {
			continue
		}
		if strings.HasSuffix(name, ".meta.json") {
			if side != nil {
				return nil, nil, nil, fmt.Errorf("export has multiple prefab metadata files")
			}
			side = f
			continue
		}
		if name == "export.json" {
			body, err := earlierEntry(f)
			if err != nil {
				return nil, nil, nil, err
			}
			var info earlierObject
			if err = json.Unmarshal(body, &info); err != nil {
				return nil, nil, nil, err
			}
			if info["~type"] == nil {
				var version int
				var bundles []earlierObject
				if index != nil || json.Unmarshal(info["format"], &version) != nil || version != 1 ||
					json.Unmarshal(info["bundles"], &bundles) != nil {
					return nil, nil, nil, fmt.Errorf("unrecognized or unresolved earlier export index")
				}
				index = f
				continue
			}
		}
		if doc != nil {
			return nil, nil, nil, fmt.Errorf("export has multiple documents")
		}
		doc = f
	}
	if doc == nil {
		return nil, nil, nil, errNotSnapmapExport
	}
	if side != nil && !strings.EqualFold(side.Name, strings.TrimSuffix(doc.Name, path.Ext(doc.Name))+".meta.json") &&
		!strings.EqualFold(side.Name, "prefab.meta.json") {
		return nil, nil, nil, fmt.Errorf("the metadata does not belong to the prefab")
	}
	return doc, side, index, nil
}

// Hold the original archive as a verified backup before replacing its directory entry.
func keepEarlierArchive(data, filename, hash string) error {
	key := fmt.Sprintf("%x", sha256.Sum256([]byte(filepath.Clean(filename)+"\x00"+hash)))
	dst := filepath.Join(data, "backups", "earlier-data", key, filepath.Base(filename))
	if !plainPath(dst) {
		return fmt.Errorf("cannot back up %s through a link", filename)
	}
	if prior, err := fileSHA256(dst); err == nil {
		if prior != hash {
			return fmt.Errorf("backup of %s differs", filename)
		}
		return nil
	} else if !os.IsNotExist(err) {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(dst), ".install-export-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if err = f.Close(); err != nil {
		return err
	}
	if err = copyFile(filename, f.Name()); err != nil {
		return err
	}
	got, err := fileSHA256(f.Name())
	if err != nil {
		return err
	}
	if got != hash {
		return fmt.Errorf("export changed before its backup finished")
	}
	return os.Rename(f.Name(), dst)
}

func convertEarlierArchive(data, doom, filename string) error {
	z, err := zip.OpenReader(filename)
	if err != nil {
		return err
	}
	defer z.Close()
	doc, side, index, err := earlierArchiveDocument(z)
	if err != nil {
		return err
	}
	body, err := earlierEntry(doc)
	if err != nil {
		return err
	}
	var metadata []byte
	if side != nil {
		metadata, err = earlierEntry(side)
		if err != nil {
			return err
		}
	}
	pins := newEarlierPins(data, doom, &z.Reader)
	defer pins.close()
	next, changed, err := convertEarlierPinnedJSON(body, metadata, pins)
	carries := false
	for _, f := range z.File {
		if earlierBundleEntry(f.Name) {
			carries = true
			break
		}
	}
	if err != nil || (!changed && side == nil && index == nil && !carries) {
		return err
	}
	hash, err := fileSHA256(filename)
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(filename), ".install-export-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	defer tmp.Close()
	out := zip.NewWriter(tmp)
	if err = out.SetComment(z.Comment); err != nil {
		return err
	}
	existing := map[string]bool{}
	for _, f := range z.File {
		if f == side || f == index || earlierBundleEntry(f.Name) {
			continue
		}
		if existing[f.Name] {
			return fmt.Errorf("duplicate export entry %s", f.Name)
		}
		existing[f.Name] = true
		if f != doc || bytes.Equal(next, body) {
			if err = out.Copy(f); err != nil {
				return err
			}
			continue
		}
		header := f.FileHeader
		w, err := out.CreateHeader(&header)
		if err != nil {
			return err
		}
		if _, err = w.Write(next); err != nil {
			return err
		}
	}
	if err = pins.addToArchive(out, existing); err != nil {
		return err
	}
	if err = out.Close(); err != nil {
		return err
	}
	if err = tmp.Sync(); err != nil {
		return err
	}
	if err = tmp.Close(); err != nil {
		return err
	}
	if err = z.Close(); err != nil {
		return err
	}
	if err = keepEarlierArchive(data, filename, hash); err != nil {
		return err
	}
	got, err := fileSHA256(filename)
	if err != nil {
		return err
	}
	if got != hash {
		return fmt.Errorf("export changed during conversion; retry")
	}
	return os.Rename(tmp.Name(), filename)
}

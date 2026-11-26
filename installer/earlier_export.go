package main

import (
	"archive/zip"
	"bytes"
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
	return convertWithConsent(data, filename, true)
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
	file := statePath(data)
	if !plainPath(file) {
		return ""
	}
	var record installRecord
	if b, err := os.ReadFile(file); err == nil && json.Unmarshal(b, &record) == nil {
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

func (parent *earlierPins) rewriteArchive(filename, stage string, dest io.Writer) (migratedDocument, error) {
	z, err := zip.OpenReader(filename)
	if err != nil {
		return migratedDocument{}, err
	}
	defer z.Close()
	doc, side, index, err := earlierArchiveDocument(z)
	if err != nil {
		return migratedDocument{}, err
	}
	body, err := earlierEntry(doc)
	if err != nil {
		return migratedDocument{}, err
	}
	var metadata []byte
	if side != nil {
		metadata, err = earlierEntry(side)
		if err != nil {
			return migratedDocument{}, err
		}
	}
	pins := newEarlierPins(parent.data, parent.doom, &z.Reader)
	defer pins.close()
	pins.output, pins.libraries, pins.placements = parent.output, parent.libraries, parent.placements
	result, err := pins.migrateDocument(body, metadata, stage)
	carries := false
	for _, f := range z.File {
		if earlierBundleEntry(f.Name) {
			carries = true
			break
		}
	}
	if err != nil || (!result.changed && !knownPrefabSide(metadata) && !carries) {
		return migratedDocument{}, err
	}
	out := zip.NewWriter(dest)
	if err = out.SetComment(z.Comment); err != nil {
		return migratedDocument{}, err
	}
	existing := map[string]bool{}
	for _, f := range z.File {
		if (f == side && knownPrefabSide(metadata)) || f == index || earlierBundleEntry(f.Name) {
			continue
		}
		name := strings.ToLower(f.Name)
		if result.vanilla && (strings.HasPrefix(name, "units/") || strings.HasPrefix(name, "files/")) {
			continue
		}
		if existing[f.Name] {
			return migratedDocument{}, fmt.Errorf("duplicate export entry %s", f.Name)
		}
		existing[f.Name] = true
		if f != doc || bytes.Equal(result.body, body) {
			if err = out.Copy(f); err != nil {
				return migratedDocument{}, err
			}
			continue
		}
		header := f.FileHeader
		w, err := out.CreateHeader(&header)
		if err != nil {
			return migratedDocument{}, err
		}
		if _, err = w.Write(result.body); err != nil {
			return migratedDocument{}, err
		}
	}
	if !result.vanilla {
		if err = pins.addToArchive(out, existing); err != nil {
			return migratedDocument{}, err
		}
	}
	result.changed = true
	return result, out.Close()
}

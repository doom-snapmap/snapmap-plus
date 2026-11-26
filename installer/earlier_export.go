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

func rewriteEarlierArchive(data, output, doom, filename string, libraries []string, dest io.Writer, placements ...*earlierPlacements) (bool, error) {
	z, err := zip.OpenReader(filename)
	if err != nil {
		return false, err
	}
	defer z.Close()
	doc, side, index, err := earlierArchiveDocument(z)
	if err != nil {
		return false, err
	}
	body, err := earlierEntry(doc)
	if err != nil {
		return false, err
	}
	var metadata []byte
	if side != nil {
		metadata, err = earlierEntry(side)
		if err != nil {
			return false, err
		}
	}
	pins := newEarlierPins(data, doom, &z.Reader)
	defer pins.close()
	pins.output = output
	pins.libraries = libraries
	if len(placements) > 0 {
		pins.placements = placements[0]
	}
	next, changed, err := convertEarlierPinnedJSON(body, metadata, pins)
	carries := false
	for _, f := range z.File {
		if earlierBundleEntry(f.Name) {
			carries = true
			break
		}
	}
	if err != nil || (!changed && !knownPrefabSide(metadata) && !carries) {
		return false, err
	}
	out := zip.NewWriter(dest)
	if err = out.SetComment(z.Comment); err != nil {
		return false, err
	}
	existing := map[string]bool{}
	for _, f := range z.File {
		if (f == side && knownPrefabSide(metadata)) || f == index || earlierBundleEntry(f.Name) {
			continue
		}
		if existing[f.Name] {
			return false, fmt.Errorf("duplicate export entry %s", f.Name)
		}
		existing[f.Name] = true
		if f != doc || bytes.Equal(next, body) {
			if err = out.Copy(f); err != nil {
				return false, err
			}
			continue
		}
		header := f.FileHeader
		w, err := out.CreateHeader(&header)
		if err != nil {
			return false, err
		}
		if _, err = w.Write(next); err != nil {
			return false, err
		}
	}
	if err = pins.addToArchive(out, existing); err != nil {
		return false, err
	}
	return true, out.Close()
}

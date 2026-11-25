package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func convertEarlierData(doom, data string) error {
	if data == "" || !plainPath(data) {
		return fmt.Errorf("invalid Snapmap+ data folder %q", data)
	}
	var failures []error
	pins := newEarlierPins(data, doom, nil)
	defer pins.close()
	if err := convertEarlierFolders(data); err != nil {
		failures = append(failures, err)
	}
	for _, sub := range []string{"prefabs", "rawmaps", "maps"} {
		dir := filepath.Join(data, sub)
		if !plainPath(dir) {
			continue
		}
		err := filepath.WalkDir(dir, func(path string, entry fs.DirEntry, err error) error {
			if errors.Is(err, fs.ErrNotExist) && path == dir {
				return nil
			}
			if err != nil {
				return err
			}
			if !plainPath(path) {
				if entry.IsDir() {
					return filepath.SkipDir
				}
				return nil
			}
			name := strings.ToLower(entry.Name())
			if entry.IsDir() || strings.HasSuffix(name, ".meta.json") {
				return nil
			}
			var convErr error
			switch strings.ToLower(filepath.Ext(name)) {
			case ".json":
				convErr = convertEarlierDocument(data, path, sub == "prefabs", pins)
			case ".zip":
				convErr = convertEarlierArchive(data, doom, path)
			}
			if convErr != nil && !errors.Is(convErr, errNotSnapmapExport) {
				failures = append(failures, fmt.Errorf("%s: %w", path, convErr))
			}
			return nil
		})
		if err != nil {
			failures = append(failures, err)
		}
	}
	path := filepath.Join(data, "rawmap.json")
	if plainPath(path) && isRegular(path) {
		if err := convertEarlierDocument(data, path, false, pins); err != nil {
			failures = append(failures, fmt.Errorf("%s: %w", path, err))
		}
	}
	path = filepath.Join(data, "rawmap.zip")
	if plainPath(path) && isRegular(path) {
		if err := convertEarlierArchive(data, doom, path); err != nil {
			failures = append(failures, fmt.Errorf("%s: %w", path, err))
		}
	}
	if doom != "" {
		root, err := nativeSaveRoot()
		if err == nil {
			var outputs []string
			outputs, err = convertEarlierNativeSaves(data, root, pins)
			for _, p := range outputs {
				ui.Note("Updated map copy: %s. Open it with File > Load Rawmap and use Save As in DOOM.", p)
			}
		}
		if err != nil {
			failures = append(failures, err)
		}
	}
	return errors.Join(failures...)
}

// Originals are retained outside the library under their content and path, including on a retry.
func keepEarlierFile(data, path string, body []byte) error {
	digest := sha256.New()
	_, _ = digest.Write([]byte(filepath.Clean(path)))
	_, _ = digest.Write([]byte{0})
	_, _ = digest.Write(body)
	destination := filepath.Join(data, "backups", "earlier-data", hex.EncodeToString(digest.Sum(nil)), filepath.Base(path))
	if !plainPath(destination) {
		return fmt.Errorf("cannot preserve %s through a link", path)
	}
	already, err := os.ReadFile(destination)
	if err == nil {
		if !bytes.Equal(already, body) {
			return fmt.Errorf("different backup already exists at %s", destination)
		}
		return nil
	}
	if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	if err = os.MkdirAll(filepath.Dir(destination), 0755); err != nil {
		return err
	}
	return writeAtomicFile(destination, body)
}

func convertEarlierDocument(data, path string, prefab bool, pins *earlierPins) error {
	original, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var side []byte
	sidePath := strings.TrimSuffix(path, filepath.Ext(path)) + ".meta.json"
	if prefab {
		if !plainPath(sidePath) {
			return fmt.Errorf("linked prefab metadata %s", sidePath)
		}
		side, err = os.ReadFile(sidePath)
		if err != nil && !errors.Is(err, fs.ErrNotExist) {
			return err
		}
	}
	out, changed, err := convertEarlierPinnedJSON(original, side, pins)
	if err != nil {
		return err
	}
	if !changed && side == nil {
		return nil
	}
	if len(side) > 0 {
		if err := keepEarlierFile(data, sidePath, side); err != nil {
			return err
		}
	}
	if changed {
		if err := keepEarlierFile(data, path, original); err != nil {
			return err
		}
		now, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if !bytes.Equal(now, original) {
			return fmt.Errorf("file changed during conversion; retry")
		}
		if err := writeAtomicFile(path, out); err != nil {
			return err
		}
	}
	if side != nil {
		now, err := os.ReadFile(sidePath)
		if err != nil {
			return err
		}
		if !bytes.Equal(now, side) {
			return fmt.Errorf("prefab metadata changed during conversion; retry")
		}
		return os.Remove(sidePath)
	}
	return nil
}

func convertEarlierJSON(original, side []byte) ([]byte, bool, error) {
	return convertEarlierJSONWithRows(original, side, nil)
}

func convertEarlierPinnedJSON(original, side []byte, pins *earlierPins) ([]byte, bool, error) {
	if pins == nil {
		return convertEarlierJSON(original, side)
	}
	return convertEarlierMapContent(original, side, pins.row, pins.pinCarried)
}

func convertEarlierJSONWithRows(original, side []byte, resolve earlierRowResolver) ([]byte, bool, error) {
	return convertEarlierMapContent(original, side, resolve, nil)
}

func convertEarlierMapContent(original, side []byte, resolve earlierRowResolver, carried earlierCarriedResolver) ([]byte, bool, error) {
	ids := map[string]string{}
	if resolve != nil {
		convert := resolve
		resolve = func(row earlierObject) (earlierObject, error) {
			next, err := convert(row)
			if err != nil {
				return nil, err
			}
			var id, salt string
			_ = json.Unmarshal(row["id"], &id)
			_ = json.Unmarshal(next["mod"], &salt)
			if id != "" && id != salt {
				ids[id] = salt
			}
			return next, nil
		}
	}
	var root earlierObject
	if err := json.Unmarshal(earlierJSONText(bytes.TrimPrefix(original, []byte{0xef, 0xbb, 0xbf})), &root); err != nil || root == nil {
		if side == nil && !bytes.Contains(original, []byte(`"snapmap-plus"`)) && !bytes.Contains(original, []byte(`"snapmapPlus"`)) {
			return original, false, nil
		}
		return nil, false, fmt.Errorf("unreadable player document")
	}
	out, err := earlierVariables(original, root, carried, ids)
	if err != nil {
		return nil, false, err
	}
	if raw, ok := root["snapmap-plus"]; ok {
		converted, changed, err := earlierMapBlock(raw, resolve)
		if err != nil {
			return nil, false, err
		}
		if changed {
			out, err = earlierSetMember(out, "snapmap-plus", converted)
			if err != nil {
				return nil, false, err
			}
		}
	}
	if side != nil || root["snapmapPlus"] != nil {
		var block earlierObject
		if raw := root["snapmapPlus"]; raw != nil {
			if err := json.Unmarshal(raw, &block); err != nil || block == nil {
				return nil, false, fmt.Errorf("unreadable prefab block")
			}
		}
		if side != nil {
			var kind string
			_ = json.Unmarshal(root["~type"], &kind)
			if kind != "idSnapEntityPrefab" && kind != "idSnapMap" {
				return nil, false, fmt.Errorf("metadata belongs to an unrecognized prefab; retained both files")
			}
			var metadata earlierObject
			if err := json.Unmarshal(bytes.TrimPrefix(side, []byte{0xef, 0xbb, 0xbf}), &metadata); err != nil || metadata == nil {
				return nil, false, fmt.Errorf("unreadable prefab metadata")
			}
			expected, err := earlierPrefabBlock(metadata)
			if err != nil {
				return nil, false, err
			}
			if _, err = earlierRows(expected, resolve); err != nil {
				return nil, false, err
			}
			if block != nil {
				// An interrupted update can leave both copies. Retire the sidecar only when they agree.
				left, _ := json.Marshal(block)
				right, _ := json.Marshal(expected)
				if !bytes.Equal(left, right) {
					return nil, false, fmt.Errorf("inline and sidecar prefab metadata differ")
				}
			} else {
				block = expected
				encoded, err := json.Marshal(block)
				if err != nil {
					return nil, false, err
				}
				out, err = earlierSetMember(out, "snapmapPlus", encoded)
				if err != nil {
					return nil, false, err
				}
			}
		}
		old, err := earlierRows(block, resolve)
		if err != nil {
			return nil, false, err
		}
		if old {
			encoded, err := json.Marshal(block)
			if err != nil {
				return nil, false, err
			}
			out, err = earlierSetMember(out, "snapmapPlus", encoded)
			if err != nil {
				return nil, false, err
			}
		}
	}
	out = earlierMapNamespaces(out, ids)
	return out, !bytes.Equal(out, original), nil
}

package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func (p *migrationPlan) legacyHomes() []string {
	out := []string{p.data}
	if !samePath(p.data, appDataDir()) {
		return out
	}
	if local := os.Getenv("LOCALAPPDATA"); local != "" {
		out = append(out, filepath.Join(local, "open-snaphak"))
	}
	if profile := os.Getenv("USERPROFILE"); profile != "" {
		out = append(out, filepath.Join(profile, "snaphak"))
	}
	return out
}

func historicalRow(row earlierObject) bool {
	if earlierID.MatchString(pinString(row, "id")) && row["bundle"] != nil {
		return true
	}
	salt, _, ok := strings.Cut(pinString(row, "mod"), "@")
	return ok && modSalt.MatchString(salt)
}

func historicalRows(raw json.RawMessage) bool {
	var rows []earlierObject
	if json.Unmarshal(raw, &rows) != nil {
		return false
	}
	for _, row := range rows {
		if historicalRow(row) {
			return true
		}
	}
	return false
}

func historicalVariable(name, value string) bool {
	if earlierRowName.MatchString(name) {
		var row earlierObject
		return json.Unmarshal([]byte(value), &row) == nil && historicalRow(row)
	}
	if name == "smp.render.v1" || name == "smp.bounds.v1" {
		if len(value) < 3 || value[0] < '1' || value[0] > '4' || value[1] != ';' || (name == "smp.bounds.v1" && value[0] != '1') {
			return false
		}
		digit := false
		for i := 2; i < len(value); i++ {
			c := value[i]
			if c >= '0' && c <= '9' {
				digit = true
			} else if !strings.ContainsRune("+-.eE; ", rune(c)) {
				return false
			}
		}
		return digit
	}
	return earlierCarrierName.MatchString(name) || earlierNavName.MatchString(name)
}

func documentEvidence(body, metadata []byte) bool {
	var root earlierObject
	if json.Unmarshal(earlierJSONText(bytes.TrimPrefix(body, []byte{0xef, 0xbb, 0xbf})), &root) != nil {
		return false
	}
	var block earlierObject
	json.Unmarshal(root["snapmap-plus"], &block)
	var version int
	json.Unmarshal(block["format"], &version)
	if version == 1 || version == 2 {
		if historicalRows(block["mods"]) {
			return true
		}
		var settings earlierObject
		if json.Unmarshal(block["settings"], &settings) == nil {
			for _, key := range []string{"render", "bounds"} {
				value := pinString(settings, key)
				if historicalVariable("smp."+key+".v1", value) && (version == 1 || key == "render" && value[0] < '4') {
					return true
				}
			}
		}
		var nav []earlierNav
		if version == 1 && json.Unmarshal(block["nav"], &nav) == nil {
			for _, set := range nav {
				if set.Module != "" && set.Class != "" && set.Digest != "" && set.Data != nil {
					return true
				}
			}
		}
		if version == 1 {
			var rows []earlierObject
			json.Unmarshal(block["mods"], &rows)
			for _, row := range rows {
				if modSalt.MatchString(pinString(row, "mod")) {
					return true
				}
			}
		}
	}
	var vars struct {
		String []struct {
			InitialValue string
			Info         struct {
				Name string
				Type string `json:"~type"`
			}
		}
	}
	json.Unmarshal(root["variables"], &vars)
	for _, row := range vars.String {
		if row.Info.Type != "snapVarInfo_t" {
			continue
		}
		if historicalVariable(row.Info.Name, row.InitialValue) {
			return true
		}
	}
	var prefab earlierObject
	if json.Unmarshal(root["snapmapPlus"], &prefab) == nil && historicalRows(prefab["mods"]) {
		return true
	}
	if kind := pinString(root, "~type"); kind == "idSnapMap" || kind == "idSnapEntityPrefab" {
		var side earlierObject
		if json.Unmarshal(bytes.TrimPrefix(metadata, []byte{0xef, 0xbb, 0xbf}), &side) == nil && historicalRows(side["mods"]) {
			return true
		}
	}
	return earlierPrototypeDependency(body, root) != nil
}

func exportStage(body, side []byte) string {
	var root earlierObject
	json.Unmarshal(earlierJSONText(bytes.TrimPrefix(body, []byte{0xef, 0xbb, 0xbf})), &root)
	if knownPrefabSide(side) || pinString(root, "~type") == "idSnapEntityPrefab" || root["snapmapPlus"] != nil {
		return "Prefabs"
	}
	return "Maps"
}

func olderArchive(file string, pins *earlierPins) (string, error) {
	z, err := zip.OpenReader(file)
	if err != nil {
		return "", nil
	}
	defer z.Close()
	var docs [][]byte
	var metadata []byte
	for _, entry := range z.File {
		n := strings.ToLower(entry.Name)
		if strings.Contains(n, "/") || !strings.HasSuffix(n, ".json") {
			continue
		}
		body, err := earlierEntry(entry)
		if err != nil {
			return "", err
		}
		if strings.HasSuffix(n, ".meta.json") {
			metadata = body
		} else {
			docs = append(docs, body)
		}
	}
	for _, doc := range docs {
		if hasHistoricalEvidence(doc, metadata, pins) {
			return exportStage(doc, metadata), nil
		}
	}
	return "", nil
}

func (p *migrationPlan) keepMissingPins(path string, err error) bool {
	var absent *missingEarlierPins
	var prototype unavailablePrototype
	var reason error
	switch {
	case errors.As(err, &absent):
		reason = absent
	case errors.As(err, &prototype):
		reason = prototype
	default:
		return false
	}
	note := fmt.Sprintf("%s: %s", path, reason)
	p.Deferred = append(p.Deferred, note)
	ui.Warn("Kept %s.", note)
	return true
}

func (p *migrationPlan) document(src, dst, stage string, archive bool, pins *earlierPins) (err error) {
	if pins != nil && pins.keepMissing != nil {
		defer func() {
			if err != nil && pins.keepMissing(src, err) {
				err = nil
			}
		}()
	}

	legacy := !samePath(src, dst)
	var original string
	if archive {
		needed, err := olderArchive(src, pins)
		if err != nil || needed == "" {
			return err
		}
		if stage == "" {
			stage = needed
		}
		original, err = migrationFileHash(src)
		if err != nil {
			return err
		}
		tmp, err := os.CreateTemp(p.work, "archive-*.zip")
		if err != nil {
			return err
		}
		defer os.Remove(tmp.Name())
		changed, err := rewriteEarlierArchive(pins.data, pins.output, pins.doom, src, pins.libraries, tmp, pins.placements)
		if closeErr := tmp.Close(); closeErr != nil {
			return closeErr
		}
		if err != nil {
			return fmt.Errorf("%s: %w", src, err)
		}
		if !changed {
			return nil
		}
		if err = p.put(dst, tmp.Name(), stage, !legacy); err != nil {
			return err
		}
	} else {
		raw, err := os.ReadFile(src)
		if err != nil {
			return err
		}
		sideFile := strings.TrimSuffix(src, filepath.Ext(src)) + ".meta.json"
		var side []byte
		if (stage == "Prefabs" || stage == "") && plainPath(sideFile) {
			side, err = os.ReadFile(sideFile)
			if err != nil && !errors.Is(err, fs.ErrNotExist) {
				return err
			}
		}
		if !hasHistoricalEvidence(raw, side, pins) {
			return nil
		}
		original = fmt.Sprintf("%x", sha256.Sum256(raw))
		if stage == "" {
			stage = exportStage(raw, side)
		}
		next, changed, err := convertEarlierPinnedJSON(raw, side, pins)
		if err != nil {
			return fmt.Errorf("%s: %w", src, err)
		}
		retire := knownPrefabSide(side)
		if !changed && !retire {
			return nil
		}
		if err = p.putBytes(dst, next, stage, !legacy); err != nil {
			return err
		}
		if retire {
			if err = p.record(sideFile, fmt.Sprintf("%x", sha256.Sum256(side)), "", "", stage); err != nil {
				return err
			}
		}
	}
	if legacy {
		return p.record(src, original, "", "", stage)
	}
	if i, ok := p.destinations[strings.ToLower(filepath.Clean(dst))]; ok && p.Changes[i].Before != original {
		return fmt.Errorf("%s changed during conversion", src)
	}
	return nil
}

func (p *migrationPlan) documents(pins *earlierPins) error {
	for _, home := range p.legacyHomes() {
		for _, sub := range []string{"maps", "rawmaps", "prefabs"} {
			root := filepath.Join(home, sub)
			stage := "Maps"
			if sub == "prefabs" {
				stage = "Prefabs"
			}
			err := walkLibrary(root, func(file string, e fs.DirEntry, _ error) error {
				if e.IsDir() {
					return nil
				}
				ext := strings.ToLower(filepath.Ext(file))
				if (ext != ".json" && ext != ".zip") || strings.HasSuffix(strings.ToLower(file), ".meta.json") {
					return nil
				}
				rel, err := filepath.Rel(home, file)
				if err != nil {
					return err
				}
				return p.document(file, filepath.Join(p.data, rel), stage, ext == ".zip", pins)
			})
			if err != nil {
				return err
			}
		}
		for _, name := range []string{"rawmap.json", "rawmap.zip"} {
			src := filepath.Join(home, name)
			if plainPath(src) && isRegular(src) {
				if err := p.document(src, filepath.Join(p.data, name), "Maps", strings.HasSuffix(name, ".zip"), pins); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

func (p *migrationPlan) mapCopy(title string, out []byte) error {
	dir := filepath.Join(p.data, "rawmaps", "Updated saves")
	for index := 1; ; index++ {
		name := title + ".json"
		if index > 1 {
			name = fmt.Sprintf("%s (%d).json", title, index)
		}
		file := filepath.Join(dir, name)
		if idx, ok := p.destinations[strings.ToLower(file)]; ok {
			if p.Changes[idx].After == fmt.Sprintf("%x", sha256.Sum256(out)) {
				return nil
			}
			continue
		}
		if earlierPathExists(file) {
			sum, err := migrationFileHash(file)
			if err != nil {
				return err
			}
			if sum == fmt.Sprintf("%x", sha256.Sum256(out)) {
				return nil
			}
			continue
		}
		return p.putBytes(file, out, "Maps", false)
	}
}

func (p *migrationPlan) native(pins *earlierPins) error {
	root, err := nativeSaveRoot()
	if err != nil {
		return err
	}
	// The game owns the integrity record. Its converted author copy can be opened and saved through DOOM.
	tmp, err := os.MkdirTemp(p.work, "slots-*")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	files, err := convertEarlierNativeSaves(tmp, root, pins)
	if err != nil {
		return err
	}
	for _, file := range files {
		body, err := os.ReadFile(file)
		if err != nil {
			return err
		}
		if err = p.mapCopy(strings.TrimSuffix(filepath.Base(file), ".json"), body); err != nil {
			return err
		}
	}
	if local := os.Getenv("LOCALAPPDATA"); local != "" {
		for _, kind := range []string{"saved-maps", "published-maps"} {
			dir := filepath.Join(local, "id Software", "DOOM", "base", "doomsnapmaps", kind)
			if err = walkLibrary(dir, func(file string, e fs.DirEntry, _ error) error {
				if e.IsDir() {
					return nil
				}
				name := strings.ToLower(e.Name())
				if !strings.HasSuffix(name, ".decl") {
					return nil
				}
				body, err := os.ReadFile(file)
				if err != nil {
					return err
				}
				if !hasHistoricalEvidence(body, nil, pins) {
					return nil
				}
				next, changed, err := convertEarlierPinnedJSON(body, nil, pins)
				if err != nil && p.keepMissingPins(file, err) {
					return nil
				}
				if err != nil || !changed {
					return err
				}
				title := strings.TrimSuffix(e.Name(), filepath.Ext(e.Name()))
				return p.mapCopy(title, next)
			}); err != nil {
				return err
			}
		}
	}
	return nil
}

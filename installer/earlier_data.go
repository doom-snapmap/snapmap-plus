package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func convertEarlierData(doom, data string) error {
	plan, cleanup, err := preparePlayerMigration(doom, data, "")
	defer cleanup()
	if err != nil {
		return err
	}
	return plan.commit()
}

func convertEarlierJSON(original, side []byte) ([]byte, bool, error) {
	return convertEarlierJSONWithRows(original, side, nil)
}

type migratedDocument struct {
	body    []byte
	changed bool
	vanilla bool
}

func (pins *earlierPins) migrateDocument(original, side []byte, stage string) (migratedDocument, error) {
	out, changed, err := convertEarlierPinnedJSON(original, side, pins)
	if err == nil {
		return migratedDocument{body: out, changed: changed}, nil
	}
	var missing *missingEarlierPins
	var prototype unavailablePrototype
	if !errors.As(err, &missing) && !errors.As(err, &prototype) {
		return migratedDocument{}, err
	}
	if stage == "Maps" && exportStage(original, side) == "Maps" {
		if missing != nil && missing.cause != nil && !errors.Is(missing.cause, os.ErrNotExist) && !errors.Is(missing.cause, errEarlierBlobMismatch) {
			return migratedDocument{}, missing.cause
		}
		out, err = projectVanillaMap(original, pins)
		if err != nil {
			return migratedDocument{}, err
		}
		return migratedDocument{body: out, changed: true, vanilla: true}, nil
	}
	if missing != nil {
		if e := pins.preserveAvailable(missing.rows, missing); e != nil {
			return migratedDocument{}, e
		}
	}
	return migratedDocument{}, err
}

func convertEarlierPinnedJSON(original, side []byte, pins *earlierPins) ([]byte, bool, error) {
	if pins == nil || pins.placements == nil || !under(pins.placements.plan.work, pins.output) {
		return convertPinnedDocument(original, side, pins)
	}
	// Stage each document separately so a failed conversion can discard its files.
	stage, err := os.MkdirTemp(pins.placements.plan.work, "document-*")
	if err != nil {
		return nil, false, err
	}
	defer os.RemoveAll(stage)
	output, content := pins.output, pins.content
	pins.output, pins.content = stage, map[string]string{}
	out, changed, err := convertPinnedDocument(original, side, pins)
	staged := pins.content
	pins.output, pins.content = output, content
	if err != nil {
		return nil, false, err
	}
	for name, src := range staged {
		rel, err := filepath.Rel(stage, src)
		if err != nil || !filepath.IsLocal(rel) {
			return nil, false, fmt.Errorf("invalid staged unit %s", name)
		}
		target := filepath.Join(pins.output, rel)
		hash := strings.TrimSuffix(filepath.Base(name), ".json")
		if existing, err := migrationFileHash(target); err != nil {
			return nil, false, err
		} else if existing == "" {
			if err = os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return nil, false, err
			}
			if err = os.Rename(src, target); err != nil {
				return nil, false, err
			}
		} else if existing != hash {
			return nil, false, fmt.Errorf("different temporary unit content at %s", name)
		}
		pins.content[name] = target
	}
	return out, changed, nil
}
func convertPinnedDocument(original, side []byte, pins *earlierPins) ([]byte, bool, error) {
	if pins == nil {
		return convertEarlierJSON(original, side)
	}
	var added earlierGlobalNames
	resolve := func(row earlierObject) (earlierObject, error) {
		if prior, err := pins.restoreRetry(row, &added); err != nil || prior != nil {
			return prior, err
		}
		next, err := pins.row(row)
		if err != nil {
			return nil, err
		}
		for _, b := range pins.candidates[pinString(row, "bundle")] {
			if b.salt != pinString(next, "mod") {
				continue
			}
			for _, f := range b.added.files {
				if err := added.add(f); err != nil {
					return nil, err
				}
			}
		}
		return next, nil
	}
	carried := func(root earlierObject, deliveries map[string]*earlierCarrier, ids map[string]string) ([]json.RawMessage, error) {
		return pins.pinEarlierDeliveries(root, deliveries, ids, &added)
	}
	var checked []earlierObject
	check := func(rows []earlierObject) error {
		checked = append(checked, rows...)
		return pins.checkRows(rows)
	}
	out, changed, err := convertEarlierMapContent(original, side, resolve, carried, check)
	var missing *missingEarlierPins
	if errors.As(err, &missing) {
		missing.rows = checked
	}
	if err != nil {
		return nil, false, err
	}
	next, err := added.placedReferences(out)
	if err != nil {
		return nil, false, err
	}
	if pins.placements != nil {
		next, err = pins.translatePlacements(original, next)
	}
	return next, changed || !bytes.Equal(next, original), err
}

func convertEarlierJSONWithRows(original, side []byte, resolve earlierRowResolver) ([]byte, bool, error) {
	return convertEarlierMapContent(original, side, resolve, nil, nil)
}

func convertEarlierMapContent(original, side []byte, resolve earlierRowResolver, carried earlierCarriedResolver, check earlierRowCheck) ([]byte, bool, error) {
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
	if err := earlierPrototypeDependency(original, root); err != nil {
		return nil, false, err
	}
	out, err := earlierVariables(original, root, carried, ids, check)
	if err != nil {
		return nil, false, err
	}
	if raw, ok := root["snapmap-plus"]; ok {
		converted, changed, err := earlierMapBlock(raw, resolve, check)
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
	if knownPrefabSide(side) || root["snapmapPlus"] != nil {
		var block earlierObject
		if raw := root["snapmapPlus"]; raw != nil {
			if err := json.Unmarshal(raw, &block); err != nil || block == nil {
				return nil, false, fmt.Errorf("unreadable prefab block")
			}
		}
		if knownPrefabSide(side) {
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
			if _, err = earlierRows(expected, resolve, check); err != nil {
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
		old, err := earlierRows(block, resolve, check)
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

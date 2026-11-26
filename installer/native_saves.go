package main

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Read the local slots in each native account. Service caches may contain someone else's map or an old revision.
func (p *migrationPlan) nativeFrom(saveRoot string, pins *earlierPins) error {
	if saveRoot == "" || !plainPath(saveRoot) {
		return fmt.Errorf("could not locate the native save folder")
	}
	profiles, err := os.ReadDir(saveRoot)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var failures []error
	for _, profile := range profiles {
		dir := filepath.Join(saveRoot, profile.Name())
		if _, err := strconv.ParseUint(profile.Name(), 10, 64); err != nil || !profile.IsDir() || !plainPath(dir) {
			continue
		}
		slots, err := os.ReadDir(dir)
		if err != nil {
			failures = append(failures, err)
			continue
		}
		for _, slot := range slots {
			name := slot.Name()
			if !slot.IsDir() || len(name) != 28 || !strings.EqualFold(name[:8], "SNAPMAPS") {
				continue
			}
			id := name[8:]
			if _, err := hex.DecodeString(id); err != nil {
				continue
			}
			slotDir := filepath.Join(dir, name)
			if !plainPath(slotDir) {
				continue
			}
			title, err := nativeSlotTitle(slotDir, id)
			if err == nil && title != "" {
				p.progress.detail(title)
				err = p.convertNativeSave(slotDir, title, pins)
			}
			if err != nil && !(pins != nil && pins.keepMissing != nil && pins.keepMissing(title, err)) {
				failures = append(failures, fmt.Errorf("%s: %w", slotDir, err))
			}
		}
	}
	return errors.Join(failures...)
}

func (p *migrationPlan) convertNativeSave(slot, title string, pins *earlierPins) error {
	path := filepath.Join(slot, "map.decl")
	if !plainPath(path) {
		return nil
	}
	stat, err := os.Stat(path)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	if !stat.Mode().IsRegular() {
		return fmt.Errorf("map.decl is not a regular file")
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	source := bufio.NewReader(f)
	if _, err = source.Discard(4); err != nil {
		return err
	}
	z, err := zlib.NewReader(source)
	if err != nil {
		return err
	}
	defer z.Close()
	var scan nativeMapMarker
	if pins != nil && pins.placements != nil && len(pins.placements.byName) > 0 {
		scan.placed = pins.placements
	}
	size, err := io.Copy(&scan, io.LimitReader(z, math.MaxInt32+1))
	if err != nil {
		return err
	}
	if size > math.MaxInt32 {
		return fmt.Errorf("map exceeds the rawmap reader's size limit")
	}
	if _, err = source.Peek(1); err != io.EOF {
		return fmt.Errorf("unexpected data after map.decl's zlib stream")
	}
	if !scan.found {
		return nil
	}
	if _, err = f.Seek(4, io.SeekStart); err != nil {
		return err
	}
	source.Reset(f)
	if err = z.(zlib.Resetter).Reset(source, nil); err != nil {
		return err
	}
	original, err := io.ReadAll(io.LimitReader(z, size+1))
	if err != nil {
		return err
	}
	if int64(len(original)) != size {
		return fmt.Errorf("map changed during conversion")
	}
	if !hasHistoricalEvidence(original, nil, pins) {
		return nil
	}
	digest := fmt.Sprintf("%x", sha256.Sum256(original))
	if known, err := p.knownVanillaSlot(slot, digest); err != nil || known {
		return err
	}
	if known, err := p.knownNativeCopy(slot, digest); err != nil || known {
		return err
	}
	result, err := pins.migrateDocument(original, nil, "Maps")
	if err != nil || !result.changed {
		return err
	}
	return p.nativeCopy(slot, digest, title, result)
}

type vanillaSlot struct {
	Format int    `json:"format"`
	Source string `json:"source"`
	Map    string `json:"map"`
	SHA256 string `json:"sha256"`
}

// The native reader selects this map only for the same local account, slot and source bytes.
func (p *migrationPlan) vanillaSlotFile(slot, source string) (string, error) {
	name := strings.ToLower(filepath.Base(slot))
	account := filepath.Base(filepath.Dir(slot))
	if _, err := strconv.ParseUint(account, 10, 64); err != nil || !filepath.IsAbs(slot) || !pinHash(source) ||
		len(name) != 28 || !strings.HasPrefix(name, "snapmaps") {
		return "", fmt.Errorf("invalid local map slot")
	}
	if _, err := hex.DecodeString(name[8:]); err != nil {
		return "", fmt.Errorf("invalid local map ID")
	}
	return filepath.Join(p.data, "rawmaps", "Updated saves", ".local", account, name[8:], source+".json"), nil
}

func (p *migrationPlan) knownVanillaSlot(slot, source string) (bool, error) {
	file, err := p.vanillaSlotFile(slot, source)
	if err != nil {
		return false, err
	}
	if !plainPath(file) {
		return false, fmt.Errorf("linked migrated map record")
	}
	raw, err := os.ReadFile(file)
	if errors.Is(err, fs.ErrNotExist) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var record vanillaSlot
	if json.Unmarshal(raw, &record) != nil || !bytes.Equal(raw, pinJSON(record)) || record.Format != 1 ||
		record.Source != source || record.SHA256 == source || !pinHash(record.SHA256) ||
		!pinRelative(record.Map) || filepath.Base(record.Map) != record.Map || !strings.EqualFold(filepath.Ext(record.Map), ".json") {
		return false, nil
	}
	hash, err := migrationFileHash(filepath.Join(p.data, "rawmaps", "Updated saves", record.Map))
	return hash == record.SHA256, err
}

// Scan stock maps without allocating their uncompressed size.
type nativeMapMarker struct {
	placed *earlierPlacements
	tail   []byte
	found  bool
}

func (s *nativeMapMarker) Write(p []byte) (int, error) {
	const marker = "\"snapmap-plus\""
	if !s.found {
		width := len(marker) - 1
		if s.placed != nil {
			width = max(width, s.placed.width)
		}
		overlap := min(len(p), width)
		edge := append(s.tail, p[:overlap]...)
		for _, word := range []string{marker, `"smp`, `"smnav1.`} {
			if bytes.Contains(edge, []byte(word)) || bytes.Contains(p, []byte(word)) {
				s.found = true
				break
			}
		}
		if !s.found && s.placed != nil {
			s.found = s.placed.couldMention(p) || s.placed.couldMention(edge)
		}
		if len(p) >= width {
			s.tail = append(s.tail[:0], p[len(p)-width:]...)
		} else {
			s.tail = append(s.tail, p...)
			s.tail = s.tail[max(0, len(s.tail)-width):]
		}
	}
	return len(p), nil
}

// The metadata localId ties this directory to the native My Maps slot. Display names may repeat or be absent.
func nativeSlotTitle(slot, id string) (string, error) {
	path := filepath.Join(slot, "game.details")
	if !plainPath(path) || !isRegular(path) {
		return "", nil
	}
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	r := bufio.NewReader(f)
	for {
		line, err := r.ReadString('\n')
		if value, ok := strings.CutPrefix(line, "metadata="); ok {
			var meta struct{ LocalID, Name string }
			if json.Unmarshal([]byte(value), &meta) != nil || !strings.EqualFold(meta.LocalID, id) {
				return "", nil
			}
			clean := strings.Map(func(r rune) rune {
				if r < 32 || strings.ContainsRune("<>:\"/\\|?*", r) {
					return '_'
				}
				return r
			}, meta.Name)
			name := []rune(strings.Trim(clean, " ."))
			name = name[:min(len(name), 80)]
			if len(name) > 0 && filepath.IsLocal(string(name)+".json") {
				return string(name), nil
			}
			return "Untitled map", nil
		}
		if errors.Is(err, io.EOF) {
			return "", nil
		}
		if err != nil {
			return "", err
		}
	}
}

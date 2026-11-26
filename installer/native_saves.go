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
	"strings"
)

// Existing slots stay owned by DOOM. Their converted copies open through File > Load Rawmap.
func convertEarlierNativeSaves(data, saveRoot string, pins ...*earlierPins) ([]string, error) {
	if data == "" || saveRoot == "" || !plainPath(data) || !plainPath(saveRoot) {
		return nil, fmt.Errorf("could not locate the Snapmap+ data and native save folders")
	}
	profiles, err := os.ReadDir(saveRoot)
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var outputs []string
	var failures []error
	for _, profile := range profiles {
		dir := filepath.Join(saveRoot, profile.Name())
		if !profile.IsDir() || !plainPath(dir) {
			continue
		}
		slots, err := os.ReadDir(dir)
		if err != nil {
			failures = append(failures, err)
			continue
		}
		for _, slot := range slots {
			name := slot.Name()
			if !slot.IsDir() || len(name) <= len("SNAPMAPS") || !strings.EqualFold(name[:8], "SNAPMAPS") {
				continue
			}
			dir := filepath.Join(dir, name)
			if !plainPath(dir) {
				continue
			}
			var resolver *earlierPins
			if len(pins) > 0 {
				resolver = pins[0]
			}
			path, err := convertNativeSave(data, dir, name[8:], resolver)
			if err != nil && !(resolver != nil && resolver.keepMissing != nil && resolver.keepMissing(dir, err)) {
				failures = append(failures, fmt.Errorf("%s: %w", dir, err))
			}
			if path != "" {
				outputs = append(outputs, path)
			}
		}
	}
	return outputs, errors.Join(failures...)
}

func convertNativeSave(data, slot, id string, pins *earlierPins) (string, error) {
	path := filepath.Join(slot, "map.decl")
	if !plainPath(path) {
		return "", nil
	}
	stat, err := os.Stat(path)
	if errors.Is(err, fs.ErrNotExist) {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	if !stat.Mode().IsRegular() {
		return "", fmt.Errorf("map.decl is not a regular file")
	}
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	source := bufio.NewReader(f)
	if _, err = source.Discard(4); err != nil {
		return "", err
	}
	z, err := zlib.NewReader(source)
	if err != nil {
		return "", err
	}
	defer z.Close()
	var scan nativeMapMarker
	if pins != nil && pins.placements != nil && len(pins.placements.byName) > 0 {
		scan.placed = pins.placements
	}
	size, err := io.Copy(&scan, io.LimitReader(z, math.MaxInt32+1))
	if err != nil {
		return "", err
	}
	if size > math.MaxInt32 {
		return "", fmt.Errorf("map exceeds the rawmap reader's size limit")
	}
	if _, err = source.Peek(1); err != io.EOF {
		return "", fmt.Errorf("unexpected data after map.decl's zlib stream")
	}
	if !scan.found {
		return "", nil
	}
	if _, err = f.Seek(4, io.SeekStart); err != nil {
		return "", err
	}
	source.Reset(f)
	if err = z.(zlib.Resetter).Reset(source, nil); err != nil {
		return "", err
	}
	original, err := io.ReadAll(io.LimitReader(z, size+1))
	if err != nil {
		return "", err
	}
	if int64(len(original)) != size {
		return "", fmt.Errorf("map changed during conversion")
	}
	if !hasHistoricalEvidence(original, nil, pins) {
		return "", nil
	}
	out, changed, err := convertEarlierPinnedJSON(original, nil, pins)
	if err != nil || !changed {
		return "", err
	}
	title := nativeMapTitle(slot, id)
	return writeNativeCopy(filepath.Join(data, "rawmaps", "Updated saves"), title, out)
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

func nativeMapTitle(slot, id string) string {
	title := "Map " + id
	path := filepath.Join(slot, "game.details")
	if !plainPath(path) {
		return title
	}
	f, err := os.Open(path)
	if err != nil {
		return title
	}
	defer f.Close()
	r := bufio.NewReader(f)
	for {
		line, err := r.ReadString('\n')
		if value, ok := strings.CutPrefix(line, "metadata="); ok {
			var meta struct{ Name string }
			if json.Unmarshal([]byte(value), &meta) == nil {
				clean := strings.Map(func(r rune) rune {
					if r < 32 || strings.ContainsRune("<>:\"/\\|?*", r) {
						return '_'
					}
					return r
				}, meta.Name)
				name := []rune(strings.Trim(clean, " ."))
				name = name[:min(len(name), 80)]
				if len(name) > 0 && filepath.IsLocal(string(name)+".json") {
					return string(name)
				}
			}
		}
		if err != nil {
			return title
		}
	}
}

func writeNativeCopy(dir, title string, out []byte) (string, error) {
	if !plainPath(dir) {
		return "", fmt.Errorf("linked rawmap destination %s", dir)
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return "", err
	}
	sum := sha256.Sum256(out)
	digest := hex.EncodeToString(sum[:])
	var staged *os.File
	for i := 1; ; i++ {
		name := title + ".json"
		if i > 1 {
			name = fmt.Sprintf("%s (%d).json", title, i)
		}
		path := filepath.Join(dir, name)
		if !plainPath(dir) {
			return "", fmt.Errorf("rawmap destination changed during conversion: %s", dir)
		}
		info, err := os.Lstat(path)
		if err == nil {
			if info.Mode().IsRegular() && info.Size() == int64(len(out)) {
				hash, err := fileSHA256(path)
				if err != nil {
					return "", err
				}
				if hash == digest {
					return "", nil
				}
			}
			continue
		}
		if !errors.Is(err, fs.ErrNotExist) {
			return "", err
		}
		if staged == nil {
			staged, err = os.CreateTemp(dir, ".updated-map-*")
			if err != nil {
				return "", err
			}
			defer os.Remove(staged.Name())
			_, err = staged.Write(out)
			if err == nil {
				err = staged.Sync()
			}
			closeErr := staged.Close()
			if err != nil {
				return "", err
			}
			if closeErr != nil {
				return "", closeErr
			}
		}
		if err = publishNativeCopy(staged.Name(), path); errors.Is(err, fs.ErrExist) {
			i--
			continue
		}
		if err != nil {
			return "", err
		}
		return path, nil
	}
}

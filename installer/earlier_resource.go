package main

import (
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"
)

// The early resource bridge matched (type, logical name, provider path) in gameresources.pindex.
type earlierResource struct {
	kind, name, request string
	implicit            bool
}

func (r earlierResource) valid() bool {
	for _, word := range []string{r.kind, r.name, r.request} {
		if !pinRelative(word) || strings.IndexFunc(word, func(c rune) bool { return c < '!' || c > '~' }) >= 0 {
			return false
		}
	}
	return !strings.Contains(r.kind, "/")
}

type earlierResourceRoot struct {
	doom, work string
	verified   map[string]bool
	namesOnly  bool
}
type earlierResourceSlice struct {
	offset       uint64
	size, stored uint32
	archive      byte
	implicit     bool
}

func earlierIndexWord(b *bytes.Buffer) ([]byte, bool) {
	s := b.Next(4)
	if len(s) != 4 {
		return nil, false
	}
	n := binary.LittleEndian.Uint32(s)
	if n > 4096 || uint64(n) > uint64(b.Len()) {
		return nil, false
	}
	return b.Next(int(n)), true
}

func visitEarlierResourceFile(file string, visit func(kind, name, provider []byte, row earlierResourceSlice) error) error {
	if !plainPath(file) {
		return fmt.Errorf("linked installed-game resource index %s", file)
	}
	f, err := os.Open(file)
	if err != nil {
		return err
	}
	defer f.Close()
	const maxIndex = 64 << 20
	data, err := io.ReadAll(io.LimitReader(f, maxIndex+1))
	if err != nil {
		return err
	}
	if len(data) > maxIndex {
		return errors.New("installed-game resource index exceeds the size limit")
	}
	return walkEarlierResourceIndex(data, visit)
}

func walkEarlierResourceIndex(index []byte, visit func(kind, name, provider []byte, row earlierResourceSlice) error) error {
	bad := errors.New("unreadable installed-game resource index")
	if len(index) < 36 || !bytes.Equal(index[:4], []byte{5, 'S', 'E', 'R'}) || int64(binary.BigEndian.Uint32(index[4:])) != int64(len(index)-32) {
		return bad
	}
	rows := binary.BigEndian.Uint32(index[32:])
	if rows > 1000000 || uint64(rows)*37 > uint64(len(index)-36) {
		return bad
	}
	b := bytes.NewBuffer(index[36:])
	for range rows {
		if len(b.Next(4)) != 4 {
			return bad
		}
		kind, typeOK := earlierIndexWord(b)
		name, nameOK := earlierIndexWord(b)
		alias, aliasOK := earlierIndexWord(b)
		fixed := b.Next(21)
		if !typeOK || !nameOK || !aliasOK || len(fixed) != 21 {
			return bad
		}
		implicit := len(alias) == 0
		if implicit {
			alias = name
		}
		row := earlierResourceSlice{binary.BigEndian.Uint64(fixed), binary.BigEndian.Uint32(fixed[8:]), binary.BigEndian.Uint32(fixed[12:]), fixed[20], implicit}
		if err := visit(kind, name, alias, row); err != nil {
			return err
		}
	}
	if b.Len() != 0 {
		return bad
	}
	return nil
}

func earlierResourceRows(index []byte, wanted map[string]earlierResource) (map[string][]earlierResourceSlice, error) {
	found := map[string][]earlierResourceSlice{}
	err := walkEarlierResourceIndex(index, func(kind, name, alias []byte, row earlierResourceSlice) error {
		r, ok := wanted[string(alias)]
		if !ok || string(name) != r.name || string(kind) != r.kind {
			return nil
		}
		old := found[r.request]
		if len(old) >= 16 || (len(old) > 0 && (row.size != old[0].size || row.stored != old[0].stored || row.archive != old[0].archive)) {
			return fmt.Errorf("conflicting installed-game rows for %s", r.request)
		}
		found[r.request] = append(old, row)
		return nil
	})
	if err != nil {
		return nil, err
	}
	return found, nil
}

func (s earlierResourceSlice) packed(base string) ([]byte, error) {
	if s.archive > 1 || s.offset > 1<<63-1 || s.stored > 64<<20 || s.size > 64<<20 {
		return nil, errors.New("invalid installed-game resource extent")
	}
	if s.stored == 0 && s.size == 0 && s.offset == 0 {
		return []byte{}, nil
	}
	if s.stored == 0 || s.size == 0 {
		return nil, errors.New("invalid empty installed-game resource")
	}
	file := "gameresources.resources"
	if s.archive == 1 {
		file = "gameresources.patch"
	}
	f, err := os.Open(filepath.Join(base, file))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	buf := make([]byte, s.stored)
	_, err = f.ReadAt(buf, int64(s.offset))
	return buf, err
}

func (r *earlierResourceRoot) read(wanted map[string]earlierResource, files map[string]string) error {
	doom := r.doom
	if doom == "" {
		var err error
		if doom, err = resolveDoom(""); err != nil {
			return fmt.Errorf("locate the game to recover earlier package resources: %w", err)
		}
	}
	base := filepath.Join(doom, "base")
	name := filepath.Join(base, "gameresources.pindex")
	index, err := os.Open(name)
	if err != nil {
		return err
	}
	defer index.Close()
	data, err := io.ReadAll(io.LimitReader(index, (64<<20)+1))
	if err != nil {
		return err
	}
	if len(data) > 64<<20 {
		return errors.New("installed-game resource index exceeds its earlier size limit")
	}
	found, err := earlierResourceRows(data, wanted)
	if err != nil {
		return err
	}
	for key, rows := range found {
		row := rows[0]
		if row.size != 0 || row.stored != 0 {
			continue
		}
		item := wanted[key]
		declKey := "generated/decls/md6def/" + item.name + ".decl"
		decl, declared := wanted[declKey]
		if _, physical := files[strings.ToLower(key)]; physical {
			continue
		}
		// A .md6 collision model is constructed from its same-name md6Def on both renderers.
		if item.kind != "cm" || !item.implicit || !strings.HasSuffix(item.name, ".md6") || item.name != key ||
			!declared || !strings.EqualFold(decl.kind, "md6Def") || decl.name != item.name ||
			len(found[declKey]) == 0 || found[declKey][0].size == 0 {
			return fmt.Errorf("no supported installed-game payload for empty resource %s", key)
		}
		for _, entry := range rows {
			if !entry.implicit || entry.offset != 0 || entry.archive > 1 {
				return fmt.Errorf("invalid derived collision model %s", key)
			}
		}
		delete(wanted, key)
	}
	dir, err := os.MkdirTemp(r.work, "retail-")
	if err != nil {
		return err
	}
	var total uint64
	for _, key := range slices.Sorted(maps.Keys(wanted)) {
		rows := found[key]
		if len(rows) == 0 {
			return fmt.Errorf("installed game has no exact %s resource %q for %s", wanted[key].kind, wanted[key].name, key)
		}
		first := rows[0]
		total += uint64(first.size)
		if total > 256<<20 {
			return errors.New("earlier package resources exceed the decoded byte limit")
		}
		stored, err := first.packed(base)
		if err != nil {
			return fmt.Errorf("%s: %w", key, err)
		}
		for _, dup := range rows[1:] {
			if dup.offset == first.offset {
				continue
			}
			other, err := dup.packed(base)
			if err != nil {
				return fmt.Errorf("%s: %w", key, err)
			}
			if !bytes.Equal(stored, other) {
				return fmt.Errorf("conflicting installed-game bytes for %s", key)
			}
		}
		if first.stored != first.size {
			// Retail Z_SYNC_FLUSH slices end at an empty non-final block. Supply only the final empty block.
			sync := bytes.HasSuffix(stored, []byte{0, 0, 255, 255})
			if sync {
				stored = append(stored, 1, 0, 0, 255, 255)
			}
			input := bytes.NewReader(stored)
			reader := flate.NewReader(input)
			stored, err = io.ReadAll(io.LimitReader(reader, int64(first.size)+1))
			reader.Close()
			if err != nil || uint64(len(stored)) != uint64(first.size) || (input.Len() != 0 && !(sync && input.Len() == 5)) {
				return fmt.Errorf("could not decode installed-game resource %s", key)
			}
		}
		// The earlier bridge served a physical authored override before the matching retail row.
		if files[strings.ToLower(key)] != "" {
			continue
		}
		path := filepath.Join(dir, fmt.Sprintf("%x", sha256.Sum256(stored)))
		if err = os.WriteFile(path, stored, 0600); err != nil {
			return err
		}
		files[strings.ToLower(key)] = path
		if r.verified == nil {
			r.verified = map[string]bool{}
		}
		r.verified[path] = true
	}
	return nil
}

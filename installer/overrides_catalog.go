package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// The installed catalog is the authority for old manifest triples and shader
// include aliases. No game data or guessed resource names ship in the installer.
type overrideRecord struct {
	kind, name, path, archive string
	offset                    uint64
	size, stored              uint32
}
type overrideCatalog struct {
	base   string
	rows   []overrideRecord
	loaded bool
}

func (c *overrideCatalog) load() error {
	if c.loaded {
		return nil
	}
	var rows []overrideRecord
	for _, stem := range []string{"gameresources", "snap_gameresources"} {
		path := filepath.Join(c.base, stem+".pindex")
		if !plainPath(path) {
			return fmt.Errorf("linked resource catalog: %s", path)
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if len(b) < 36 || !bytes.Equal(b[:4], []byte{5, 'S', 'E', 'R'}) || uint64(binary.BigEndian.Uint32(b[4:8])) != uint64(len(b)-32) {
			return fmt.Errorf("invalid resource catalog: %s", path)
		}
		count, at := binary.BigEndian.Uint32(b[32:36]), 36
		if uint64(count) > uint64(len(b)-at)/37 {
			return fmt.Errorf("truncated resource catalog: %s", path)
		}
		field := func() (string, error) {
			if at > len(b)-4 {
				return "", io.ErrUnexpectedEOF
			}
			n := uint64(binary.LittleEndian.Uint32(b[at : at+4]))
			at += 4
			if n > uint64(len(b)-at) {
				return "", io.ErrUnexpectedEOF
			}
			s := strings.ToLower(strings.ReplaceAll(string(b[at:at+int(n)]), `\`, "/"))
			at += int(n)
			if strings.ContainsRune(s, 0) {
				return "", fmt.Errorf("NUL in resource identity")
			}
			return s, nil
		}
		for i := uint32(0); i < count; i++ {
			if at > len(b)-4 {
				return io.ErrUnexpectedEOF
			}
			at += 4
			var r overrideRecord
			if r.kind, err = field(); err != nil {
				return err
			}
			if r.name, err = field(); err != nil {
				return err
			}
			if r.path, err = field(); err != nil {
				return err
			}
			if at > len(b)-21 || b[at+20] > 1 {
				return fmt.Errorf("invalid resource row in %s", path)
			}
			r.offset = binary.BigEndian.Uint64(b[at : at+8])
			r.size = binary.BigEndian.Uint32(b[at+8 : at+12])
			r.stored = binary.BigEndian.Uint32(b[at+12 : at+16])
			suffix := ".resources"
			if b[at+20] == 1 {
				suffix = ".patch"
			}
			r.archive = filepath.Join(c.base, stem+suffix)
			at += 21
			rows = append(rows, r)
		}
		if at != len(b) {
			return fmt.Errorf("unexpected catalog trailer: %s", path)
		}
	}
	c.rows = rows
	c.loaded = true
	return nil
}

func (r overrideRecord) read() ([]byte, error) {
	if !plainPath(r.archive) {
		return nil, fmt.Errorf("linked resource archive: %s", r.archive)
	}
	f, err := os.Open(r.archive)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if r.offset > uint64(info.Size()) || uint64(r.stored) > uint64(info.Size())-r.offset || (r.size == 0) != (r.stored == 0) {
		return nil, fmt.Errorf("resource outside archive: %s", r.path)
	}
	section := io.NewSectionReader(f, int64(r.offset), int64(r.stored))
	var reader io.Reader = section
	if r.size != r.stored {
		z := flate.NewReader(section)
		defer z.Close()
		reader = z
	}
	b, err := io.ReadAll(io.LimitReader(reader, int64(r.size)+1))
	if err != nil {
		return nil, err
	}
	if uint64(len(b)) != uint64(r.size) {
		return nil, fmt.Errorf("resource size mismatch: %s", r.path)
	}
	return b, nil
}

func (c *overrideCatalog) resolve(kind, name, path string) ([]byte, error) {
	if err := c.load(); err != nil {
		return nil, err
	}
	var chosen *overrideRecord
	// Old manifests selected campaign records. Match all three fields, retaining
	// shader permutations and refusing ambiguous records even when names agree.
	for i := range c.rows {
		r := &c.rows[i]
		provider := r.path
		if provider == "" {
			provider = r.name
		}
		if !strings.HasPrefix(filepath.Base(r.archive), "gameresources.") || r.kind != strings.ToLower(kind) || r.name != strings.ToLower(name) || provider != strings.ToLower(path) {
			continue
		}
		if chosen != nil && (r.archive != chosen.archive || r.offset != chosen.offset || r.size != chosen.size || r.stored != chosen.stored) {
			return nil, rejectOverride("ambiguous manifest record %s / %s / %s", kind, name, path)
		}
		chosen = r
	}
	if chosen == nil {
		return nil, rejectOverride("manifest record is not installed: %s / %s / %s", kind, name, path)
	}
	return chosen.read()
}

func (c *overrideCatalog) includes(relative string) ([]string, error) {
	if err := c.load(); err != nil {
		return nil, err
	}
	paths := map[string]bool{}
	for _, r := range c.rows {
		path := r.path
		if path == "" {
			path = r.name
		}
		if strings.Index(path, ".inc") != len(path)-4 {
			continue
		}
		alias := path
		if !strings.HasPrefix(alias, "includes") {
			if at := strings.IndexByte(alias, '/'); at >= 0 {
				alias = alias[at+1:]
			}
		}
		if alias == strings.ToLower(relative) {
			paths[path] = true
		}
	}
	if len(paths) == 0 {
		return nil, rejectOverride("cannot resolve shader include %s against the installed catalog", relative)
	}
	out := make([]string, 0, len(paths))
	for p := range paths {
		out = append(out, p)
	}
	return out, nil
}

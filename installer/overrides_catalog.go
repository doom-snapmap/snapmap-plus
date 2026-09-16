package main

import (
	"bytes"
	"compress/flate"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// The installed catalogs are the only authority for legacy manifest records,
// shader-include aliases and loose engine paths. The installer ships no game
// data or resource names. Reads are streamed and never write to game archives.

const (
	archiveCampaign = 0 // gameresources: the archive legacy manifests selected from
	archiveSnapMap  = 1 // snap_gameresources: stock SnapMap resources
)

type catalogRow struct {
	kind, name, path string
	offset           uint64
	size, stored     uint32
	patch            bool
	archive          int
}

// Provider path: the historical bridge used the name when a row has no path.
func (r *catalogRow) provider() string {
	if r.path == "" {
		return r.name
	}
	return r.path
}

type catalogTriple struct{ kind, name, provider string }

type overrideCatalog struct {
	base     string
	loaded   bool
	rows     []catalogRow
	exact    map[catalogTriple][]int // campaign rows, exact spelling
	paths    map[string][]int        // lower-case provider path, both archives
	roots    map[string]bool         // lower-case first path segments
	dirs     map[string]bool         // lower-case two-segment directory prefixes
	includes map[string][]string     // lower-case shader_includes alias -> provider paths
}

func newOverrideCatalog(doom string) *overrideCatalog {
	if doom == "" {
		return &overrideCatalog{}
	}
	return &overrideCatalog{base: filepath.Join(doom, "base")}
}

func (c *overrideCatalog) load() error {
	if c.loaded {
		return nil
	}
	if c.base == "" {
		return errors.New("the DOOM installation is required to read its resource catalog; pass --doom with your DOOM folder")
	}
	c.exact = map[catalogTriple][]int{}
	c.paths = map[string][]int{}
	c.roots = map[string]bool{}
	c.dirs = map[string]bool{}
	c.includes = map[string][]string{}
	for archive, stem := range []string{"gameresources", "snap_gameresources"} {
		if err := c.loadIndex(archive, stem); err != nil {
			return err
		}
	}
	for path := range c.paths {
		segments := strings.Split(path, "/")
		c.roots[segments[0]] = true
		if len(segments) > 2 {
			c.dirs[segments[0]+"/"+segments[1]] = true
		}
		// build_override_path: the first ".inc" must end the name; strip the
		// first component unless the name starts with "includes".
		if strings.Index(path, ".inc") == len(path)-4 {
			alias := path
			if !strings.HasPrefix(path, "includes") {
				if slash := strings.IndexByte(path, '/'); slash >= 0 {
					alias = path[slash+1:]
				}
			}
			c.includes[alias] = append(c.includes[alias], path)
		}
	}
	for alias := range c.includes {
		sort.Strings(c.includes[alias])
	}
	c.loaded = true
	return nil
}

func (c *overrideCatalog) loadIndex(archive int, stem string) error {
	path := filepath.Join(c.base, stem+".pindex")
	if !plainPath(path) {
		return fmt.Errorf("linked resource catalog: %s", path)
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("cannot read the installed resource catalog: %w", err)
	}
	if len(b) < 36 || !bytes.Equal(b[:4], []byte{5, 'S', 'E', 'R'}) || uint64(binary.BigEndian.Uint32(b[4:8])) != uint64(len(b)-32) {
		return fmt.Errorf("invalid resource catalog: %s", path)
	}
	count, at := binary.BigEndian.Uint32(b[32:36]), 36
	if uint64(count) > uint64(len(b)-at)/37 {
		return fmt.Errorf("truncated resource catalog: %s", path)
	}
	field := func() (string, bool) {
		if len(b)-at < 4 {
			return "", false
		}
		n := binary.LittleEndian.Uint32(b[at:])
		at += 4
		if uint64(n) > uint64(len(b)-at) {
			return "", false
		}
		s := b[at : at+int(n)]
		at += int(n)
		return string(s), bytes.IndexByte(s, 0) < 0
	}
	for i := uint32(0); i < count; i++ {
		if len(b)-at < 4 {
			return fmt.Errorf("truncated resource catalog: %s", path)
		}
		at += 4
		var r catalogRow
		var ok1, ok2, ok3 bool
		r.kind, ok1 = field()
		r.name, ok2 = field()
		r.path, ok3 = field()
		if !ok1 || !ok2 || !ok3 || len(b)-at < 21 || b[at+20] > 1 {
			return fmt.Errorf("invalid resource catalog row in %s", path)
		}
		r.offset = binary.BigEndian.Uint64(b[at:])
		r.size = binary.BigEndian.Uint32(b[at+8:])
		r.stored = binary.BigEndian.Uint32(b[at+12:])
		r.patch = b[at+20] == 1
		r.archive = archive
		at += 21
		index := len(c.rows)
		c.rows = append(c.rows, r)
		provider := strings.ReplaceAll(r.provider(), `\`, "/")
		if archive == archiveCampaign {
			key := catalogTriple{r.kind, r.name, r.provider()}
			c.exact[key] = append(c.exact[key], index)
		}
		lower := asciiLower(provider)
		c.paths[lower] = append(c.paths[lower], index)
	}
	if at != len(b) {
		return fmt.Errorf("unexpected resource catalog trailer: %s", path)
	}
	return nil
}

// Whether an engine path is an installed provider path.
func (c *overrideCatalog) hasPath(path string) (bool, error) {
	if err := c.load(); err != nil {
		return false, err
	}
	return len(c.paths[asciiLower(path)]) > 0, nil
}

// Whether a lower-case first segment is an installed engine root.
func (c *overrideCatalog) isRoot(segment string) (bool, error) {
	if err := c.load(); err != nil {
		return false, err
	}
	return c.roots[asciiLower(segment)], nil
}

// Installed engine content under a wrapper-like name, such as the
// decls/renderprogs shader-include tree, is never reinterpreted.
func (c *overrideCatalog) engineContent(path string) (bool, error) {
	if err := c.load(); err != nil {
		return false, err
	}
	lower := asciiLower(path)
	if len(c.paths[lower]) > 0 {
		return true, nil
	}
	segments := strings.Split(lower, "/")
	return len(segments) > 2 && c.dirs[segments[0]+"/"+segments[1]], nil
}

// A folder is engine content when installed paths live in or below its first
// two segments.
func (c *overrideCatalog) engineDirectory(path string) (bool, error) {
	if err := c.load(); err != nil {
		return false, err
	}
	segments := strings.Split(asciiLower(path), "/")
	return len(segments) >= 2 && c.dirs[segments[0]+"/"+segments[1]], nil
}

func (c *overrideCatalog) includeAliases(relative string) ([]string, error) {
	if err := c.load(); err != nil {
		return nil, err
	}
	return c.includes[asciiLower(relative)], nil
}

type countingByteReader struct {
	r   io.Reader
	buf [64 * 1024]byte
	at  int
	end int
	n   int64
}

func (c *countingByteReader) fill() error {
	if c.at < c.end {
		return nil
	}
	n, err := c.r.Read(c.buf[:])
	c.at, c.end = 0, n
	if n > 0 {
		return nil
	}
	if err == nil {
		err = io.ErrNoProgress
	}
	return err
}

func (c *countingByteReader) Read(p []byte) (int, error) {
	if err := c.fill(); err != nil {
		return 0, err
	}
	n := copy(p, c.buf[c.at:c.end])
	c.at += n
	c.n += int64(n)
	return n, nil
}

func (c *countingByteReader) ReadByte() (byte, error) {
	if err := c.fill(); err != nil {
		return 0, err
	}
	b := c.buf[c.at]
	c.at++
	c.n++
	return b, nil
}

func (c *overrideCatalog) archivePath(r *catalogRow) string {
	stem := "gameresources"
	if r.archive == archiveSnapMap {
		stem = "snap_gameresources"
	}
	if r.patch {
		return filepath.Join(c.base, stem+".patch")
	}
	return filepath.Join(c.base, stem+".resources")
}

// Open the stored slice of one record. Nothing is buffered beyond one read chunk.
func (c *overrideCatalog) openRecord(r *catalogRow) (*io.SectionReader, io.Closer, error) {
	if r.size == 0 && r.stored == 0 {
		if r.offset != 0 {
			return nil, nil, fmt.Errorf("empty resource record has an offset: %s", r.provider())
		}
		return io.NewSectionReader(bytes.NewReader(nil), 0, 0), io.NopCloser(nil), nil
	}
	if r.stored == 0 || r.size == 0 {
		return nil, nil, fmt.Errorf("invalid resource record sizes: %s", r.provider())
	}
	path := c.archivePath(r)
	if !plainPath(path) {
		return nil, nil, fmt.Errorf("linked resource archive: %s", path)
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, nil, err
	}
	info, err := f.Stat()
	if err != nil {
		f.Close()
		return nil, nil, err
	}
	if r.offset > uint64(info.Size()) || uint64(r.stored) > uint64(info.Size())-r.offset {
		f.Close()
		return nil, nil, fmt.Errorf("resource record lies outside its archive: %s", r.provider())
	}
	return io.NewSectionReader(f, int64(r.offset), int64(r.stored)), f, nil
}

func (c *overrideCatalog) storedDigest(r *catalogRow) ([32]byte, error) {
	var sum [32]byte
	section, closer, err := c.openRecord(r)
	if err != nil {
		return sum, err
	}
	defer closer.Close()
	h := sha256.New()
	if n, err := io.Copy(h, section); err != nil || n != section.Size() {
		return sum, fmt.Errorf("cannot read resource record: %s", r.provider())
	}
	copy(sum[:], h.Sum(nil))
	return sum, nil
}

// Decode exactly one record to w. Accept a complete final DEFLATE block or
// Doom's Z_SYNC_FLUSH form ending in an empty stored block at the exact slice
// end, matching sh_inflate_raw; shorter, longer or trailing data is refused.
func (c *overrideCatalog) decode(r *catalogRow, w io.Writer) error {
	section, closer, err := c.openRecord(r)
	if err != nil {
		return err
	}
	defer closer.Close()
	if r.size == r.stored {
		if n, err := io.Copy(w, section); err != nil || n != int64(r.size) {
			return fmt.Errorf("cannot read resource record: %s", r.provider())
		}
		return nil
	}
	input := &countingByteReader{r: section}
	z := flate.NewReader(input)
	defer z.Close()
	if n, err := io.CopyN(w, z, int64(r.size)); err != nil || n != int64(r.size) {
		return fmt.Errorf("resource record does not decode to its recorded size: %s", r.provider())
	}
	var extra [1]byte
	n, err := z.Read(extra[:])
	for n == 0 && err == nil {
		n, err = z.Read(extra[:])
	}
	consumed := input.n == int64(r.stored)
	if n == 0 && err == io.EOF && consumed {
		return nil
	}
	if n == 0 && errors.Is(err, io.ErrUnexpectedEOF) && consumed && r.stored >= 4 {
		var tail [4]byte
		if _, err := section.ReadAt(tail[:], int64(r.stored)-4); err == nil && tail == [4]byte{0, 0, 0xff, 0xff} {
			return nil
		}
	}
	return fmt.Errorf("resource record has an invalid compressed stream: %s", r.provider())
}

func (c *overrideCatalog) recordDigest(r *catalogRow) ([32]byte, int64, error) {
	h := sha256.New()
	counter := &countingWriter{w: h}
	if err := c.decode(r, counter); err != nil {
		return [32]byte{}, 0, err
	}
	var sum [32]byte
	copy(sum[:], h.Sum(nil))
	return sum, counter.n, nil
}

type countingWriter struct {
	w io.Writer
	n int64
}

func (c *countingWriter) Write(p []byte) (int, error) {
	n, err := c.w.Write(p)
	c.n += int64(n)
	return n, err
}

// Resolve a legacy manifest row exactly like resource_bridge.c: case-sensitive
// type, name and provider path in the campaign catalog. Repeated rows must
// have identical metadata and stored bytes; they never select a winner.
func (c *overrideCatalog) resolveManifest(kind, name, provider string) (*catalogRow, error) {
	if err := c.load(); err != nil {
		return nil, err
	}
	matches := c.exact[catalogTriple{kind, name, provider}]
	if len(matches) == 0 {
		return nil, rejectOverride("manifest record is not in the installed game: %s\t%s\t%s", kind, name, provider)
	}
	first := &c.rows[matches[0]]
	if len(matches) > 1 {
		want, err := c.storedDigest(first)
		if err != nil {
			return nil, err
		}
		for _, index := range matches[1:] {
			other := &c.rows[index]
			if other.size != first.size || other.stored != first.stored || other.patch != first.patch {
				return nil, rejectOverride("manifest record is ambiguous in the installed game: %s	%s	%s", kind, name, provider)
			}
			got, err := c.storedDigest(other)
			if err != nil {
				return nil, err
			}
			if got != want {
				return nil, rejectOverride("manifest record is ambiguous in the installed game: %s	%s	%s", kind, name, provider)
			}
		}
	}
	return first, nil
}

// A record identical to every SnapMap row at the same path is already an
// installed dependency. Materializing it would add bytes, not behavior.
func (c *overrideCatalog) stockIdentical(r *catalogRow) (bool, error) {
	var stock []*catalogRow
	for _, index := range c.paths[asciiLower(r.provider())] {
		if c.rows[index].archive == archiveSnapMap {
			stock = append(stock, &c.rows[index])
		}
	}
	if len(stock) == 0 {
		return false, nil
	}
	want, _, err := c.recordDigest(r)
	if err != nil {
		return false, err
	}
	for _, s := range stock {
		if s.size != r.size {
			return false, nil
		}
		got, _, err := c.recordDigest(s)
		if err != nil {
			return false, err
		}
		if got != want {
			return false, nil
		}
	}
	return true, nil
}

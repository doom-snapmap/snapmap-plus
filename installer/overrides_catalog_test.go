package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakeRecord describes one synthetic catalog row. No game data is used.
type fakeRecord struct {
	kind, name, path string
	body             string
	mode             string // "stored" (default), "final" or "sync"
	snap             bool   // snap_gameresources instead of gameresources
	patch            bool
	stored           []byte // explicit stored bytes, overriding mode
	offset           int64  // explicit offset when >= 0 with stored bytes shared
	sharedOffset     bool
}

func deflateFixture(t *testing.T, body string, sync bool) []byte {
	t.Helper()
	var b bytes.Buffer
	w, err := flate.NewWriter(&b, flate.BestCompression)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write([]byte(body)); err != nil {
		t.Fatal(err)
	}
	if sync {
		// Z_SYNC_FLUSH: the stream ends in an empty non-final stored block.
		if err := w.Flush(); err != nil {
			t.Fatal(err)
		}
	} else if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func writeFakeCatalog(t *testing.T, doom string, records []fakeRecord) {
	t.Helper()
	base := filepath.Join(doom, "base")
	mkdirAll(t, base)
	for _, stem := range []string{"gameresources", "snap_gameresources"} {
		archives := map[bool]*bytes.Buffer{false: {}, true: {}}
		index := make([]byte, 36)
		copy(index, []byte{5, 'S', 'E', 'R'})
		count := 0
		for _, r := range records {
			if r.snap != (stem == "snap_gameresources") {
				continue
			}
			stored := r.stored
			if stored == nil {
				switch r.mode {
				case "final":
					stored = deflateFixture(t, r.body, false)
				case "sync":
					stored = deflateFixture(t, r.body, true)
				default:
					stored = []byte(r.body)
				}
			}
			archive := archives[r.patch]
			offset := int64(archive.Len())
			archive.Write(stored)
			if r.sharedOffset {
				offset = r.offset
			}
			ordinal := make([]byte, 4)
			binary.BigEndian.PutUint32(ordinal, uint32(count))
			index = append(index, ordinal...)
			for _, field := range []string{r.kind, r.name, r.path} {
				length := make([]byte, 4)
				binary.LittleEndian.PutUint32(length, uint32(len(field)))
				index = append(index, length...)
				index = append(index, field...)
			}
			fixed := make([]byte, 21)
			binary.BigEndian.PutUint64(fixed, uint64(offset))
			binary.BigEndian.PutUint32(fixed[8:], uint32(len(r.body)))
			binary.BigEndian.PutUint32(fixed[12:], uint32(len(stored)))
			if r.patch {
				fixed[20] = 1
			}
			index = append(index, fixed...)
			count++
		}
		binary.BigEndian.PutUint32(index[32:], uint32(count))
		binary.BigEndian.PutUint32(index[4:], uint32(len(index)-32))
		if err := os.WriteFile(filepath.Join(base, stem+".pindex"), index, 0o644); err != nil {
			t.Fatal(err)
		}
		for patch, suffix := range map[bool]string{false: ".resources", true: ".patch"} {
			if err := os.WriteFile(filepath.Join(base, stem+suffix), archives[patch].Bytes(), 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}
}

func decodeRecord(t *testing.T, c *overrideCatalog, kind, name, provider string) (string, error) {
	t.Helper()
	r, err := c.resolveManifest(kind, name, provider)
	if err != nil {
		return "", err
	}
	var b bytes.Buffer
	err = c.decode(r, &b)
	return b.String(), err
}

func TestOverrideCatalogDecodesStoredFinalAndSyncFlushRecords(t *testing.T) {
	doom := t.TempDir()
	long := strings.Repeat("compressible payload ", 400)
	writeFakeCatalog(t, doom, []fakeRecord{
		{kind: "image", name: "stored", path: "generated/image/stored.bimage", body: "stored bytes"},
		{kind: "image", name: "final", path: "generated/image/final.bimage", body: long, mode: "final"},
		{kind: "model", name: "sync", path: "generated/model/sync.bmodel", body: long + "sync", mode: "sync", patch: true},
		{kind: "image", name: "empty-path", body: "named"},
	})
	c := newOverrideCatalog(doom)
	for _, tc := range []struct{ kind, name, provider, want string }{
		{"image", "stored", "generated/image/stored.bimage", "stored bytes"},
		{"image", "final", "generated/image/final.bimage", long},
		{"model", "sync", "generated/model/sync.bmodel", long + "sync"},
		{"image", "empty-path", "empty-path", "named"},
	} {
		got, err := decodeRecord(t, c, tc.kind, tc.name, tc.provider)
		if err != nil || got != tc.want {
			t.Fatalf("%s: %v (%d bytes)", tc.name, err, len(got))
		}
	}
	// Legacy manifests matched every field exactly, including letter case.
	if _, err := c.resolveManifest("Image", "stored", "generated/image/stored.bimage"); err == nil {
		t.Fatal("case-folded manifest type accepted")
	}
}

func TestOverrideCatalogRefusesMalformedStreams(t *testing.T) {
	doom := t.TempDir()
	body := strings.Repeat("resource ", 100)
	final := deflateFixture(t, body, false)
	sync := deflateFixture(t, body, true)
	writeFakeCatalog(t, doom, []fakeRecord{
		{kind: "a", name: "trailing", path: "a/trailing", body: body, stored: append(append([]byte{}, final...), 0)},
		{kind: "a", name: "truncated", path: "a/truncated", body: body, stored: sync[:len(sync)-5]},
		{kind: "a", name: "short", path: "a/short", body: body + "x", stored: final},
		{kind: "a", name: "long", path: "a/long", body: body[:len(body)-1], stored: final},
	})
	c := newOverrideCatalog(doom)
	for _, name := range []string{"trailing", "truncated", "short", "long"} {
		if _, err := decodeRecord(t, c, "a", name, "a/"+name); err == nil {
			t.Errorf("%s stream accepted", name)
		}
	}
}

func TestOverrideCatalogDuplicateRowsRequireIdenticalStoredBytes(t *testing.T) {
	doom := t.TempDir()
	writeFakeCatalog(t, doom, []fakeRecord{
		{kind: "decl", name: "same", path: "generated/decls/x/same.decl", body: "{ same }"},
		{kind: "decl", name: "same", path: "generated/decls/x/same.decl", body: "{ same }"},
		{kind: "decl", name: "different", path: "generated/decls/x/different.decl", body: "{ one }"},
		{kind: "decl", name: "different", path: "generated/decls/x/different.decl", body: "{ two }"},
	})
	c := newOverrideCatalog(doom)
	if got, err := decodeRecord(t, c, "decl", "same", "generated/decls/x/same.decl"); err != nil || got != "{ same }" {
		t.Fatalf("identical duplicate rows refused: %v", err)
	}
	if _, err := c.resolveManifest("decl", "different", "generated/decls/x/different.decl"); err == nil {
		t.Fatal("ambiguous duplicate rows accepted")
	}
	row := c.rows[c.exact[catalogTriple{"decl", "same", "generated/decls/x/same.decl"}][0]]
	row.offset = 1 << 40
	if err := c.decode(&row, &bytes.Buffer{}); err == nil {
		t.Fatal("record outside its archive accepted")
	}
}

func TestOverrideCatalogIncludeAliasesAndRoots(t *testing.T) {
	doom := t.TempDir()
	writeFakeCatalog(t, doom, []fakeRecord{
		{kind: "renderProg", name: "a", path: "decls/renderprogs/includes/vertex.inc", body: "v"},
		{kind: "renderProg", name: "b", path: "decls/renderprogs/includes/vertex.inc", body: "v"},
		{kind: "renderProg", name: "c", path: "includes/direct.inc", body: "d"},
		{kind: "renderProg", name: "d", path: "decls/renderprogs/global.inc.bak", body: "x"},
		{kind: "image", name: "stock", path: "maps/game/stock.bimage", body: "s", snap: true},
	})
	c := newOverrideCatalog(doom)
	for relative, want := range map[string]string{
		"renderprogs/includes/vertex.inc": "decls/renderprogs/includes/vertex.inc",
		"RenderProgs/Includes/Vertex.inc": "decls/renderprogs/includes/vertex.inc",
		"includes/direct.inc":             "includes/direct.inc",
	} {
		aliases, err := c.includeAliases(relative)
		if err != nil || len(aliases) != 1 || aliases[0] != want {
			t.Fatalf("%s: %v %v", relative, aliases, err)
		}
	}
	if aliases, _ := c.includeAliases("renderprogs/global.inc.bak"); len(aliases) != 0 {
		t.Fatal("a name whose first .inc is not terminal became an include")
	}
	for segment, want := range map[string]bool{"maps": true, "decls": true, "demons": false} {
		if got, err := c.isRoot(segment); err != nil || got != want {
			t.Fatalf("root %s = %v %v", segment, got, err)
		}
	}
	for path, want := range map[string]bool{"decls/renderprogs/new.inc": true, "decls/snapeditorentitydef/x.decl": false,
		"maps/game/stock.bimage": true, "images/icon.bimage": false} {
		if got, err := c.engineContent(path); err != nil || got != want {
			t.Fatalf("engine content %s = %v %v", path, got, err)
		}
	}
}

func TestOverrideCatalogStockIdentity(t *testing.T) {
	doom := t.TempDir()
	writeFakeCatalog(t, doom, []fakeRecord{
		{kind: "image", name: "same", path: "generated/image/same.bimage", body: "stock bytes", mode: "final"},
		{kind: "image", name: "same", path: "generated/image/same.bimage", body: "stock bytes", snap: true},
		{kind: "image", name: "changed", path: "generated/image/changed.bimage", body: "campaign"},
		{kind: "image", name: "changed", path: "generated/image/changed.bimage", body: "snapmap!", snap: true},
		{kind: "image", name: "campaign", path: "generated/image/campaign.bimage", body: "only"},
	})
	c := newOverrideCatalog(doom)
	for name, want := range map[string]bool{"same": true, "changed": false, "campaign": false} {
		r, err := c.resolveManifest("image", name, "generated/image/"+name+".bimage")
		if err != nil {
			t.Fatal(err)
		}
		if got, err := c.stockIdentical(r); err != nil || got != want {
			t.Fatalf("%s stock identical = %v %v", name, got, err)
		}
	}
}

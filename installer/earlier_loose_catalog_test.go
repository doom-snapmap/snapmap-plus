package main

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func writeLooseIndexes(t *testing.T, doom string, campaign, snap []testResource) {
	t.Helper()
	writeResourceIndex(t, doom, snap...)
	base := filepath.Join(doom, "base")
	if err := os.Rename(filepath.Join(base, "gameresources.pindex"), filepath.Join(base, "snap_gameresources.pindex")); err != nil {
		t.Fatal(err)
	}
	writeResourceIndex(t, doom, campaign...)
}

func TestEarlierLooseCatalogResolvesPhysicalShadowPaths(t *testing.T) {
	doom := t.TempDir()
	row := func(kind, name, provider string) testResource {
		return testResource{kind: kind, name: name, alias: provider, size: 100, stored: 45}
	}
	font := row("font", "courier", "fonts/courier/64_df.dat")
	vertex := row("renderProg", "vertex", "decls/renderprogs/includes/vertex.inc")
	other := row("renderProg", "gl-vertex", "another/renderprogs/includes/vertex.inc")
	bare := row("renderProg", "includes/native.inc", "")
	earlierInc := row("renderProg", "include", "prog/a.inc-dir/a.inc")
	zero := testResource{kind: "cm", name: "missing.md6"}
	writeLooseIndexes(t, doom, []testResource{font, vertex, bare, earlierInc, zero}, []testResource{vertex, other, font})
	const alias = "shader_includes/renderprogs/includes/vertex.inc"
	keys := []string{"Fonts/courier/64_df.dat", "fonts/courier/64_df.dat", alias, "shader_includes/includes/native.inc",
		"prog/a.inc-dir/a.inc", "shader_includes/a.inc-dir/a.inc", vertex.alias, "courier", "missing.md6", "my/notes.txt", "../bad"}
	got, err := earlierLooseCatalog(doom, keys)
	want := map[string][]string{
		keys[0]: {font.alias}, keys[1]: {font.alias}, alias: {other.alias, vertex.alias},
		keys[3]: {bare.name}, keys[4]: {earlierInc.alias},
	}
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("resolved %v, error %v, want %v", got, err, want)
	}
}

func TestEarlierLooseCatalogDefersUnavailableOrRejectsBrokenIndexes(t *testing.T) {
	doom := t.TempDir()
	font := testResource{kind: "font", name: "courier", alias: "fonts/courier/64_df.dat", size: 100, stored: 45}
	for _, rows := range [][]testResource{nil, {font}} {
		if rows != nil {
			writeResourceIndex(t, doom, rows...)
		}
		if got, err := earlierLooseCatalog(doom, []string{font.alias, "notes.txt"}); err != nil || len(got) != 0 {
			t.Fatalf("incomplete game became authoritative: %v, %v", got, err)
		}
	}
	writeF(t, filepath.Join(doom, "base", "snap_gameresources.pindex"), "damaged")
	if got, err := earlierLooseCatalog(doom, []string{font.alias}); err == nil || len(got) != 0 {
		t.Fatalf("used a partial result from a damaged install: %v, %v", got, err)
	}
	if got, err := earlierLooseCatalog(doom, nil); err != nil || len(got) != 0 {
		t.Fatalf("unrelated migration required a catalog: %v, %v", got, err)
	}
}

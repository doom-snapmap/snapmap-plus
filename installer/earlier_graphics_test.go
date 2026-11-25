package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestEarlierParticleSourceAndCache(t *testing.T) {
	data := t.TempDir()
	mod := filepath.Join(data, "mods", "sample")
	path := filepath.Join(mod, "materials", "glow.decl")
	manifest := filepath.Join(mod, modBuildManifest)
	original, err := os.ReadFile(filepath.Join("..", "tests", "fixtures", "modprog", "legacy_particle_explosion.decl"))
	if err != nil {
		t.Fatal(err)
	}
	for file, contents := range map[string][]byte{filepath.Join(mod, "mod.json"): []byte(`{"name":"Example"}`), path: original, manifest: []byte("immutable original")} {
		if err := os.MkdirAll(filepath.Dir(file), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, contents, 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := upgradeGraphicsFolders(data); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, part := range []string{`"snapmapplus/particles/uberparticles"`, "particleuberparms1\t{ 0, 0, 20, 0 }", "particleemissivemap\t", "smokebrightness\t{ 1, 0, 0, 0 }"} {
		if !strings.Contains(string(got), part) {
			t.Fatal("missing", part)
		}
	}
	if bytes.Contains(got, []byte("eternalparticle")) || exists(manifest) {
		t.Fatal("retained obsolete shader data")
	}
	if err := upgradeGraphicsFolders(data); err != nil {
		t.Fatal(err)
	}
	again, _ := os.ReadFile(path)
	if !bytes.Equal(again, got) {
		t.Fatal("second conversion changed the source")
	}
	backups, _ := filepath.Glob(filepath.Join(data, "backups", "earlier-data", "*", "glow.decl"))
	if len(backups) != 1 {
		t.Fatalf("%d author backups", len(backups))
	}
	b, _ := os.ReadFile(backups[0])
	if !bytes.Equal(b, original) {
		t.Fatal("author backup changed")
	}
	side, _ := filepath.Glob(filepath.Join(mod, ".build", "before-graphics-*.json"))
	if len(side) != 1 || readF(t, side[0]) != "immutable original" {
		t.Fatal("missing generated manifest backup")
	}
}

func TestUnrelatedAndMalformedEarlierParticleSources(t *testing.T) {
	unrelated := []byte("{\r\n// eternalparticle in a comment\r\nemissive\tcustomExpr\r\n}\r\n")
	if out, err := upgradeParticleMaterial(unrelated); err != nil || !bytes.Equal(out, unrelated) {
		t.Fatal(out, err)
	}
	if _, err := upgradeParticleMaterial([]byte("{\neternalparticle\t{1, invalid}\n}")); err == nil {
		t.Fatal("accepted malformed vector")
	}
}

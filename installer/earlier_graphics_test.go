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
	for _, part := range []string{"type\tparticle", "lighting\texplosion", "gradient\t{ 1, 0.099899, 0, 1, 0.111932, 0.022174 }", `glowmask	"mods/wuahwfly/effect/texture/fire/explosion_m_33_emissive_2.tga"`, "softdistance\t20.997376"} {
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
	unrelated := []byte("{\r\n/*\r\neternalparticle {1,2,3,4}\r\nstageprogram snapmapplus/eternal/uberparticles\r\n*/\r\nsubblock {\r\nmultiblendsoftness {1,1,1,1}\r\n}\r\nemissive\tcustomExpr\r\n}\r\n")
	if out, err := upgradeEarlierMaterial(unrelated); err != nil || !bytes.Equal(out, unrelated) {
		t.Fatal(out, err)
	}
	if _, err := upgradeEarlierMaterial([]byte("{\neternalparticle\t{1, invalid}\n}")); err == nil {
		t.Fatal("accepted malformed vector")
	}
}

func TestEarlierLayeredSource(t *testing.T) {
	original, err := os.ReadFile(filepath.Join("..", "tests", "fixtures", "modprog", "legacy_wounds.decl"))
	if err != nil {
		t.Fatal(err)
	}
	got, err := upgradeEarlierMaterial(original)
	if err != nil {
		t.Fatal(err)
	}
	for _, part := range []string{"type\tlayered", "wounds\tyes", `layer1	"mods/wuahwfly/ml/art/tile/gore/blood_gore_char_albedo.tga"`, "surfacetype\t"} {
		if !strings.Contains(string(got), part) {
			t.Fatal("missing", part)
		}
	}
	for _, part := range []string{"multiblend", "blendlayer1", "layerstilingfactor"} {
		if strings.Contains(string(got), part) {
			t.Fatal("retained old key", part)
		}
	}
	again, err := upgradeEarlierMaterial(got)
	if err != nil || !bytes.Equal(again, got) {
		t.Fatal("layered source changed on second conversion", err)
	}
}

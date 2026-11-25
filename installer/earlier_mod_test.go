package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestEarlierModFolder(t *testing.T) {
	data := t.TempDir()
	mod := filepath.Join(data, "mods", "Pack group", "My Pack")
	desc := `{"id":"alex.tiny","version":"1.2.3","name":"My Pack","author":"Alex","clip_events":[],"strings":{"English":{"Label":"A"}}}`
	writeF(t, filepath.Join(mod, "mod.json"), desc)
	for name, body := range map[string]string{
		"assets/generated/decls/entitydef/mods/alex.tiny/door.decl": `{ inherit = "mods/alex.tiny/door"; skin = "mods/alex.tiny/thing"; image = "mods/alex.tiny/surface.tga"; model = "materials/native_model"; }`,
		"assets/generated/decls/skin/mods/alex.tiny/thing.decl":     "{}",
		"assets/generated/image/mods/alex.tiny/surface.bimage":      "\x00image\xff",
		"assets/generated/decls/entitydef/stock/door.decl":          `{ inherit = "mods/alex.tiny/door"; }`,
		"assets/mods/alex.tiny/scripts/entry.lua":                   "self:pos()\non.anim()\nlocal p=fx('mods/alex.tiny/door')",
		"notes/from-author.txt":                                     "keep exactly\n",
	} {
		writeF(t, filepath.Join(mod, name), body)
	}
	if err := convertEarlierData("", data); err != nil {
		t.Fatal(err)
	}
	salt := earlierSalt("alex.tiny")
	author := readF(t, filepath.Join(mod, "mod.json"))
	if !strings.Contains(author, salt) || strings.Contains(author, "clip_events") || !strings.Contains(author, "animation_events") || !strings.Contains(author, "1.2.3") {
		t.Fatalf("descriptor: %s", author)
	}
	body := readF(t, filepath.Join(mod, "entities", "door.decl"))
	for _, s := range []string{`"entities/door"`, `"decls/skin/thing"`, `"images/surface.tga"`, `"native:materials/native_model"`} {
		if !strings.Contains(body, s) {
			t.Errorf("missing %s: %s", s, body)
		}
	}
	if readF(t, filepath.Join(mod, "images", "surface.bimage")) != "\x00image\xff" || readF(t, filepath.Join(mod, "notes/from-author.txt")) != "keep exactly\n" || exists(filepath.Join(mod, "assets")) {
		t.Fatal("source files not preserved")
	}
	if lua := readF(t, filepath.Join(mod, "scripts/entry.lua")); !strings.Contains(lua, "self:position()") || !strings.Contains(lua, "on.animation()") || !strings.Contains(lua, "play_effect('mods/"+salt+"/door')") {
		t.Fatal(lua)
	}
	originals, _ := filepath.Glob(filepath.Join(data, "backups/earlier-data/mod-folders/*/original", "mod.json"))
	if len(originals) != 1 || readF(t, originals[0]) != desc {
		t.Fatal("missing original")
	}
	writeF(t, filepath.Join(mod, "entities", "door.decl"), "author edited after upgrade")
	if err := convertEarlierData("", data); err != nil || readF(t, filepath.Join(mod, "entities", "door.decl")) != "author edited after upgrade" {
		t.Fatalf("retry %v", err)
	}
}

func TestEarlierFolderResume(t *testing.T) {
	data := t.TempDir()
	record := filepath.Join(data, "backups/earlier-data/mod-folders/test")
	mod := filepath.Join(data, "mods", "Example")
	backup := filepath.Join(record, "original")
	writeF(t, filepath.Join(backup, "mod.json"), `{"name":"before","id":"example"}`)
	digest, err := earlierTreeHash(backup)
	if err != nil {
		t.Fatal(err)
	}
	converted := `{"name":"after","snapmap-plus":{"format":2,"salt":"a1234567"}}`
	writeF(t, filepath.Join(record, "converted", "mod.json"), converted)
	plan, err := json.Marshal(earlierModPlan{filepath.Join("mods", "Example"), filepath.Join("mods", "Example"), digest})
	if err != nil {
		t.Fatal(err)
	}
	writeF(t, filepath.Join(record, "plan.json"), string(plan))
	if err := convertEarlierFolders(data); err != nil {
		t.Fatal(err)
	}
	if !exists(backup) || readF(t, filepath.Join(mod, "mod.json")) != converted || !exists(filepath.Join(record, "finished.json")) {
		t.Fatal("incomplete recovery")
	}
}

func TestEarlierFolderKeepsIncompatibleBinary(t *testing.T) {
	data := t.TempDir()
	src := filepath.Join(data, "mods", "Earlier")
	bin := filepath.Join(src, "assets/generated/image/mods/old.id/imp.bimage")
	writeF(t, filepath.Join(src, "mod.json"), `{"id":"old.id"}`)
	writeF(t, bin, "\x00mods/old.id/embedded")
	if err := convertEarlierFolders(data); err == nil || !strings.Contains(err.Error(), "binary") {
		t.Fatal(err)
	}
	if _, err := os.Stat(bin); err != nil {
		t.Fatal(err)
	}
	if exists(filepath.Join(src, "images")) {
		t.Fatal("a failed translation modified the mod")
	}
}

func TestEarlierCookedSpellingsAndBorrowedSound(t *testing.T) {
	src := filepath.Join(t.TempDir(), "earlier")
	dst := filepath.Join(t.TempDir(), "author")
	writeF(t, filepath.Join(src, "mod.json"), `{"id":"release-one","strings":{"en":{"mods/release-one/tile":"Tile"}},"companions":{"mods/release-one/tile":[{"entityDef":"mods/release-one/tile"}]}}`)
	files := map[string]string{
		"cooked/anim/md6/mods/release_one/clips/a.bmd6anim":                                "animation",
		"generated/basemodel/md6/mods/release_one/m.bmd6model":                             "model",
		"cooked/model/mods/release_one/preview.bmodel":                                     "preview",
		"generated/swf/mods/release-one/screens/a.bswf":                                    "movie",
		"generated/swf/mods/release-one/screens/a.bimage":                                  "frame",
		"generated/image/textures/guis/snapmaps/entity_icons/mods/release_one/tile.bimage": "icon",
		"generated/decls/sound/mods/release-one/ambient/hum.decl":                          `{ volume = -12; }`,
		"generated/decls/entitydef/mods/release-one/tile.decl":                             `{ anim="md6/mods/release-one/clips/a.md6anim"; image="textures/guis/snapmaps/entity_icons/mods/release_one/tile.tga"; sound="mods/release-one/ambient/hum"; label="#str_mods/release-one/tile"; external="textures/guis/snapmaps/entity_icons/mods/another-pack/tile.tga"; }`,
		"cooked/anim/md6/characters/humans/idle.bmd6anim":                                  "external clip",
	}
	for req, data := range files {
		writeF(t, filepath.Join(src, "assets", filepath.FromSlash(req)), data)
	}
	if _, err := translateEarlierMod(src, dst); err != nil {
		t.Fatal(err)
	}
	for author, data := range map[string]string{
		"animations/clips/a.bmd6anim": "animation", "meshes/m.bmd6model": "model",
		"meshes/preview.bmodel": "preview", "movies/screens/a.bswf": "movie", "movies/screens/a.bimage": "frame",
		"images/textures/guis/snapmaps/entity_icons/tile.bimage": "icon",
		"decls/sound/ambient/hum.decl":                           `{ volume = -12; }`,
		"native/cooked/anim/md6/characters/humans/idle.bmd6anim": "external clip",
	} {
		if got := readF(t, filepath.Join(dst, author)); got != data {
			t.Errorf("%s: %q", author, got)
		}
	}
	body := readF(t, filepath.Join(dst, "entities/tile.decl"))
	for _, want := range []string{"animations/clips/a.md6anim", "images/textures/guis/snapmaps/entity_icons/tile.tga", "decls/sound/ambient/hum", "#str_tile", "textures/guis/snapmaps/entity_icons/mods/another-pack/tile.tga"} {
		if !strings.Contains(body, want) {
			t.Errorf("expected %q in %s", want, body)
		}
	}
	var doc struct {
		Companions map[string][]map[string]string
		Strings    map[string]map[string]string
	}
	if err := json.Unmarshal([]byte(readF(t, filepath.Join(dst, "mod.json"))), &doc); err != nil {
		t.Fatal(err)
	}
	if rows := doc.Companions["entities/tile"]; len(rows) != 1 || rows[0]["entityDef"] != "entities/tile" || doc.Strings["en"]["tile"] != "Tile" {
		t.Fatalf("descriptor: %+v", doc)
	}
	if _, err := earlierAuthorPath("release-one", "generated/image/textures/mods/another-pack/tile.bimage"); err == nil {
		t.Fatal("claimed another mod's file")
	}
}

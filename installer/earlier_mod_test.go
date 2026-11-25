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

package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

func TestModPreviewCountsDistinctChangedAuthorFolders(t *testing.T) {
	r := newUpdateRig(t, "dev")
	mods := filepath.Join(r.data, "mods")
	material := readF(t, filepath.Join("..", "tests", "fixtures", "modprog", "legacy_particle_explosion.decl"))
	for _, name := range []string{"first", "second"} {
		folder := filepath.Join(mods, "group", name)
		writeF(t, filepath.Join(folder, "mod.json"), fmt.Sprintf("\ufeff{\"name\":\"Crates\",\"snapmap-plus\":%s}", testModBlock))
	}
	// Editing two materials also invalidates the generated build of this one mod.
	folder := filepath.Join(mods, "effects")
	writeF(t, filepath.Join(folder, "mod.json"), "{\"name\":\"Light\\u001b\\nTest\"}")
	writeF(t, filepath.Join(folder, modBuildManifest), testModBlock)
	for _, name := range []string{"red", "blue"} {
		writeF(t, filepath.Join(folder, "materials", name+".decl"), material)
	}
	writeF(t, filepath.Join(mods, "unchanged", "mod.json"), "{\"name\":\"Current\"}")
	writeF(t, filepath.Join(mods, "pieces.old-3", "mod.json"), fmt.Sprintf("{\"snapmap-plus\":%s}", testModBlock))
	// An old copy in the served cache has its own mod.json, but is shared map content.
	writeF(t, filepath.Join(mods, ".content", "served", "pin", "mod.json"), fmt.Sprintf("{\"name\":\"Cache\",\"snapmap-plus\":%s}", testModBlock))
	legacy := filepath.Join(os.Getenv("USERPROFILE"), "snaphak", "mods", "group", "tunes")
	writeF(t, filepath.Join(legacy, "mod.json"), "{\"id\":\"old.tunes\",\"name\":\"Music\"}")
	writeF(t, filepath.Join(legacy, "assets", "mods", "old.tunes", "scripts", "first.lua"), "print('one')")
	writeF(t, filepath.Join(legacy, "assets", "mods", "old.tunes", "scripts", "second.lua"), "print('two')")
	// Loose strings from two earlier homes go to the same editable my-overrides folder.
	for _, home := range []string{r.data, filepath.Join(os.Getenv("LOCALAPPDATA"), "open-snaphak")} {
		writeF(t, filepath.Join(home, "strings", "strids.json"), "{\"example\":\"Text\"}")
	}
	writeF(t, filepath.Join(r.data, "config.json"), "{\"schema_version\":1,\"settings\":{\"mods.upload_confirmed\":true}}")
	expected := []string{"Crates (group/first)", "Crates (group/second)", "Light  Test", "Music", "My overrides"}
	plan, done, err := preparePlayerMigration("", r.data, "")
	if err != nil {
		t.Fatal(err)
	}
	defer done()
	preview := plan.modPreview()
	if !preview.Settings || !preview.Shared || !slices.Equal(preview.Names, expected) {
		t.Fatalf("mixed preview %+v", preview)
	}
	if err = plan.commit(); err != nil {
		t.Fatal(err)
	}
	again, done, err := preparePlayerMigration("", r.data, "")
	if err != nil {
		t.Fatal(err)
	}
	defer done()
	if v := again.modPreview(); again.ready() || len(v.Names) != 0 || v.Settings || v.Shared {
		t.Fatalf("repeat preview %+v", v)
	}
}

func TestModPreviewSeparatesSettingsAndSharedStorage(t *testing.T) {
	for _, item := range []struct {
		file, body       string
		settings, shared bool
	}{
		{"config.json", "{\"schema_version\":1,\"settings\":{\"mods.upload_confirmed\":false}}", true, false},
		{"mods/.served/banks/item.bnk", "bytes", false, true},
		{"mods/.builds/personal/mod.json", "{}", false, false},
		{"rawmaps/own.json", "{\"~type\":\"idSnapMap\",\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;90000\"}}}", false, false},
	} {
		t.Run(item.file, func(t *testing.T) {
			data := t.TempDir()
			writeF(t, filepath.Join(data, item.file), item.body)
			p, done, err := preparePlayerMigration("", data, "")
			if err != nil {
				t.Fatal(err)
			}
			defer done()
			if v := p.modPreview(); len(v.Names) != 0 || item.settings != v.Settings || item.shared != v.Shared {
				t.Fatalf("%+v", v)
			}
		})
	}
}

func TestModPreviewRecoveredAuthorAndUnnamedPackage(t *testing.T) {
	data := t.TempDir()
	writeF(t, filepath.Join(data, "overrides", "old", "package.json"), "{\"id\":\"old\"}")
	writeF(t, filepath.Join(data, "overrides", "old", "assets", "mods", "old", "scripts", "package.lua"), "print('test')")
	record := filepath.Join(data, "backups", "earlier-data", "mod-folders", "test")
	original := filepath.Join(record, "original")
	writeF(t, filepath.Join(original, "mod.json"), "{\"name\":\"Previous name\",\"id\":\"previous\"}")
	h, err := earlierTreeHash(original)
	if err != nil {
		t.Fatal(err)
	}
	writeF(t, filepath.Join(record, "converted", "mod.json"), "{\"name\":\"Recovered name\",\"snapmap-plus\":{\"format\":2,\"salt\":\"a1234567\"}}")
	prior, err := json.Marshal(earlierModPlan{"mods/Recovered", "mods/Recovered", h})
	if err != nil {
		t.Fatal(err)
	}
	writeF(t, filepath.Join(record, "plan.json"), string(prior))
	p, done, err := preparePlayerMigration("", data, "")
	if err != nil {
		t.Fatal(err)
	}
	defer done()
	if v := p.modPreview(); !slices.Equal(v.Names, []string{"Recovered name", "old"}) || v.Settings || v.Shared {
		t.Fatalf("%+v", v)
	}
}

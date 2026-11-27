package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestEditorMigrationKeepsBindingsCatalogsAndCachedMapBytes(t *testing.T) {
	r := newUpdateRig(t, "dev")
	dir := filepath.Join(r.data, "mods", "collection", "controls")
	const row = `{ inherit = "prop/prop_base"; edit = { nested = { entityDef = "wrong"; } entityDef = "entities/panel"; categoryNameTag = "#str_ci_tab"; displayNameTag = "#str_use"; previewImage = "materials/panel"; } }`
	const lift = `{ edit = { propertySheets = { num = 7; } } }`
	doc := `{"name":"Controls","strings":{"en":{"ci_tab":"Controls","use":"Switch"},"it":{"use":"Interruttore"}},"snapmap-plus":{"format":2,"salt":"` + earlierSalt("campaign-interactables-panels") + `"}}`
	for path, body := range map[string]string{
		"mod.json": doc, "entities/panel.decl": "{ }", "decls/snapeditorentitydef/panel.decl": row,
		"native/generated/decls/snapeditorentitydef/func/lift.decl": lift, "editor/strings/en.json": `{"other":"Earlier edit"}`,
	} {
		writeF(t, filepath.Join(dir, path), body)
	}
	cache := filepath.Join(r.data, "cache", "content", "served", "pin", "mod.json")
	writeF(t, cache, doc)
	plan, done, err := preparePlayerMigration("", r.data, "")
	if err != nil {
		t.Fatal(err)
	}
	defer done()
	if readF(t, filepath.Join(dir, "mod.json")) != doc {
		t.Fatal("preflight changed the source")
	}
	if got := plan.modPreview().Names; len(got) != 1 || got[0] != "Controls" {
		t.Fatalf("preview %v", got)
	}
	if err := plan.commit(); err != nil {
		t.Fatal(err)
	}
	for path, want := range map[string]string{
		"editor/templates/panel.decl": row, "editor/native/generated/decls/snapeditorentitydef/func/lift.decl": lift,
	} {
		if actual := readF(t, filepath.Join(dir, path)); actual != want {
			t.Fatalf("%s changed native data", path)
		}
	}
	if readF(t, cache) != doc || strings.Contains(readF(t, filepath.Join(dir, "mod.json")), `"strings"`) {
		t.Fatal("catalog location")
	}
	for path, want := range map[string][]string{
		"editor/strings/en.json": {"Earlier edit", "Controls", "Switch"}, "editor/strings/it.json": {"Interruttore"},
		"editor/imported-tiles.json": {"shared/campaign-interactables", "entities/panel", "editor/templates/panel.decl"},
	} {
		got := readF(t, filepath.Join(dir, path))
		if !json.Valid([]byte(got)) {
			t.Fatalf("%s invalid JSON", path)
		}
		for _, text := range want {
			if !strings.Contains(got, text) {
				t.Fatalf("%s missing %s", path, text)
			}
		}
	}
	again, done, err := preparePlayerMigration("", r.data, "")
	if err != nil {
		t.Fatal(err)
	}
	defer done()
	if again.ready() {
		t.Fatalf("second pass: %+v", again.Changes)
	}
}

func TestEditorMigrationRejectsConflictingCatalogBeforeChangingSource(t *testing.T) {
	r := newUpdateRig(t, "dev")
	dir := filepath.Join(r.data, "mods", "test")
	file := filepath.Join(dir, "mod.json")
	const doc = `{"strings":{"en":{"a":"old"}},"name":"My mod"}`
	writeF(t, file, doc)
	writeF(t, filepath.Join(dir, "editor", "strings", "en.json"), `{"a":"new"}`)
	_, done, err := preparePlayerMigration("", r.data, "")
	defer done()
	if err == nil || !strings.Contains(err.Error(), "different translations") || readF(t, file) != doc {
		t.Fatalf("conflicting catalog: %v", err)
	}
	if _, err = os.Stat(filepath.Join(dir, "editor", "imported-tiles.json")); !os.IsNotExist(err) {
		t.Fatal(err)
	}
}

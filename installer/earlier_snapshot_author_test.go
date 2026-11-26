package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestEarlierSnapshotBuildFieldsAndDistinctStringKeys(t *testing.T) {
	data := t.TempDir()
	src, dst := filepath.Join(data, "old"), filepath.Join(data, "new")
	writeF(t, filepath.Join(src, "bundle.json"), `{"format":1,"id":"old.id","shadows":["generated/decls/entitydef/player/coop.decl"],"legacy":{"source":"overrides","layout":"C","keyForm":"overrides"},"requirements":{"cvars":{"r_example":"1"}},"strings":{"en":{"Label":"Plain","Label  ":"Padded","Solo ":"Alone"},"fr":{"Label  ":"Autre"}},"example":"#str_Label  "}`)
	writeF(t, filepath.Join(src, "assets/generated/decls/entitydef/player/coop.decl"), `{ edit = { title = "#str_Label  "; plain = "#str_Label"; } }`)
	writeF(t, filepath.Join(src, "assets/mods/old.id/scripts/main.lua"), `local value = '#str_Label  '`)
	salt, err := translateEarlierMod(src, dst)
	if err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(filepath.Join(dst, "mod.json"))
	if err != nil {
		t.Fatal(err)
	}
	var top earlierObject
	if err = json.Unmarshal(body, &top); err != nil {
		t.Fatal(err)
	}
	for _, old := range []string{"id", "format", "shadows", "legacy"} {
		if _, ex := top[old]; ex {
			t.Fatalf("stale build field %s", old)
		}
	}
	if salt != earlierSalt("old.id") || pinString(top, "example") != "#str_Label__" || top["requirements"] == nil {
		t.Fatal(string(body))
	}
	var strs map[string]map[string]string
	if err = json.Unmarshal(top["strings"], &strs); err != nil {
		t.Fatal(err)
	}
	if len(strs["en"]) != 3 || strs["en"]["Label"] != "Plain" || strs["en"]["Label__"] != "Padded" || strs["en"]["Solo"] != "Alone" || strs["fr"]["Label__"] != "Autre" {
		t.Fatal(strs)
	}
	if got := readF(t, filepath.Join(dst, "native/generated/decls/entitydef/player/coop.decl")); got != `{ edit = { title = "#str_Label__"; plain = "#str_Label"; } }` {
		t.Fatal(got)
	}
	if got := readF(t, filepath.Join(dst, "scripts/main.lua")); got != "local value = '#str_Label__'" {
		t.Fatal(got)
	}
}

func TestEarlierSnapshotRefusesAmbiguousStringAlias(t *testing.T) {
	src, dst := filepath.Join(t.TempDir(), "old"), filepath.Join(t.TempDir(), "new")
	writeF(t, filepath.Join(src, "bundle.json"), `{"id":"old.id","strings":{"en":{"Label":"Plain","Label  ":"One","Label__":"Two"}}}`)
	if _, err := translateEarlierMod(src, dst); err == nil {
		t.Fatal("overwrote an existing distinct string")
	}
	if _, err := os.Stat(dst); !os.IsNotExist(err) {
		t.Fatal("published an ambiguous translation")
	}
}

package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestMigratePlayerConfig(t *testing.T) {
	const nav = `"navmesh.embed_in_saved_maps": false`
	const other = `"external": {"packages.embed_in_saved_maps":"keep", "number":12345678901234567890123}`
	const old = `"packages.embed_in_saved_maps":false`
	const prior = `"overrides.user_enabled":true, "mods.upload_confirmed":false`
	for _, tc := range []struct{ before, after string }{
		{old + ", " + nav + ", " + other, " " + nav + ", " + other},
		{nav + ", " + prior + ", " + other, nav + ", " + other},
		{nav + ", " + prior, nav}, {old + ", " + prior, ""}, {other, other},
	} {
		input := []byte("\xef\xbb\xbf" + `{ "schema_version" : 1, "settings":{` + tc.before + `}, "future" : true }` + "\r\n")
		want := bytes.Replace(input, []byte("{"+tc.before+"}"), []byte("{"+tc.after+"}"), 1)
		got, keep, err := migratePlayerConfig(input, false)
		if err != nil || keep || !bytes.Equal(got, want) {
			t.Fatalf("%s -> %s, kept loose %v, error %v, want %s", input, got, keep, err, want)
		}
		again, keep, err := migratePlayerConfig(got, false)
		if err != nil || keep || !bytes.Equal(again, got) {
			t.Fatalf("second run rewrote %s", got)
		}
	}
}

func TestConfigMigrationPreservesUnsupportedDocuments(t *testing.T) {
	for _, tc := range []struct {
		source  string
		invalid bool
	}{
		{`{"schema_version":2,"settings":{"packages.embed_in_saved_maps":true}}`, false},
		{`{"settings":{"packages.embed_in_saved_maps":true}}`, false},
		{`{"schema_version":1,"settings":{"x":1,"x":2,"mods.upload_confirmed":true}}`, true},
		{`{"schema_version":1,"settings":{},"settings":{}}`, true},
		{`{"schema_version":1,"settings":[]}`, true}, {`{"x":1} trailing`, true},
		{`{"schema_version":1,"settings":{"mods.upload_confirmed":true,"extra":[{"key":0,"key":1}]}}`, true},
		{`{"schema_version":1,"settings":{"mods.upload_confirmed":true,"extra":` + strings.Repeat("[", 64) + "0" + strings.Repeat("]", 64) + "}}", true},
	} {
		data := []byte(tc.source)
		got, keep, err := migratePlayerConfig(data, true)
		if !bytes.Equal(data, got) || !keep || (err != nil) != tc.invalid {
			t.Fatalf("%s: kept loose %v, err %v", data, keep, err)
		}
	}
}

func TestConfigMigrationKeepsDisabledLooseOverrides(t *testing.T) {
	for _, disabled := range []string{"false", "0", "null"} {
		in := []byte(`{"schema_version":1,"settings":{"packages.embed_in_saved_maps":true,"overrides.user_enabled":` + disabled + `,"mods.upload_confirmed":true,"theme":"dark"}}`)
		want := `{"schema_version":1,"settings":{"overrides.user_enabled":` + disabled + `,"theme":"dark"}}`
		for attempt := 0; attempt < 2; attempt++ {
			got, keep, err := migratePlayerConfig(in, true)
			if err != nil || !keep || string(got) != want {
				t.Fatalf("got %s, kept loose %v, error %v", got, keep, err)
			}
			in = got
		}
	}
}

func TestConfigMigrationMatchesNativeUnicodeStrings(t *testing.T) {
	for _, tail := range []string{
		`"ext":"\uD800"`, `"ext":[{"k":"\udfff"}]`, `"\uD800":"v"`,
		`"ext":"\uD800\uD801"`, `"ext":"\uD800\\udc00"`,
	} {
		input := []byte(`{"schema_version":1,"settings":{"mods.upload_confirmed":true,` + tail + `}}`)
		got, keep, err := migratePlayerConfig(input, true)
		if err == nil || !keep || !bytes.Equal(got, input) {
			t.Errorf("did not preserve invalid config %s", input)
		}
	}
	for _, tail := range []string{`"ext":"\uD83D\uDE80"`, `"ext":"\\uD800"`, `"\u0000":"\ufffd"`} {
		input := []byte(`{"schema_version":1,"settings":{"mods.upload_confirmed":true,` + tail + `}}`)
		got, keep, err := migratePlayerConfig(input, true)
		want := bytes.Replace(input, []byte(`"mods.upload_confirmed":true,`), nil, 1)
		if err != nil || keep || !bytes.Equal(got, want) {
			t.Errorf("changed supported extension text %s: got %s, error %v", input, got, err)
		}
	}
}

func TestUnpairedConfigProtectsLooseFilesWhileMigratingMap(t *testing.T) {
	data := t.TempDir()
	config := filepath.Join(data, "config.json")
	body := `{"schema_version":1,"settings":{"mods.upload_confirmed":true,"extension":"\uD800"}}`
	writeF(t, config, body)
	before, err := os.Stat(config)
	if err != nil {
		t.Fatal(err)
	}
	loose := filepath.Join(data, "overrides", "generated", "decls", "material", "demo", "lamp.decl")
	writeF(t, loose, "held by existing settings")
	room := filepath.Join(data, "rawmap.json")
	writeF(t, room, `{"snapmap-plus":{"format":1,"settings":{"bounds":"1;32000"}}}`)
	for range 2 {
		if err := convertEarlierData("", data); err != nil {
			t.Fatal(err)
		}
		info, err := os.Stat(config)
		if err != nil || !info.ModTime().Equal(before.ModTime()) || readF(t, config) != body ||
			readF(t, loose) != "held by existing settings" || !strings.Contains(readF(t, room), `"format":2`) {
			t.Fatal("invalid config or its loose file changed, or the neighboring map was skipped")
		}
	}
}

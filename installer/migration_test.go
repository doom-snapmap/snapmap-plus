package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/iotest"
	"time"
)

func migrationZip(t *testing.T, file, body string) {
	t.Helper()
	var b bytes.Buffer
	z := zip.NewWriter(&b)
	for name, value := range map[string]string{"Room.json": body, "thumbnail.txt": "original"} {
		w, err := z.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = w.Write([]byte(value)); err != nil {
			t.Fatal(err)
		}
	}
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	writeF(t, file, b.String())
}

func TestStageEarlierArchiveWhileItsSourceIsOpen(t *testing.T) {
	data := t.TempDir()
	src := filepath.Join(data, "room.zip")
	migrationZip(t, src, `{"~type":"idSnapMap","snapmap-plus":{"format":1,"settings":{"bounds":"1;90000"}}}`)
	original := readF(t, src)
	reader, err := os.Open(src)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	var staged bytes.Buffer
	pins := newEarlierPins(data, "", nil)
	pins.output = t.TempDir()
	result, err := pins.rewriteArchive(src, "Maps", &staged)
	if err != nil || !result.changed || readF(t, src) != original {
		t.Fatalf("staging an open export: changed %v, error %v", result.changed, err)
	}
	updated, err := zip.NewReader(bytes.NewReader(staged.Bytes()), int64(staged.Len()))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range updated.File {
		if f.Name == "Room.json" {
			b, err := earlierEntry(f)
			if err != nil || !bytes.Contains(b, []byte(`"format":2`)) {
				t.Fatalf("staged map: %s, %v", b, err)
			}
			return
		}
	}
	t.Fatal("staged export lost the map")
}

func TestMixedMigrationKeepsVanillaAndCurrentBytesAndTimes(t *testing.T) {
	r := newUpdateRig(t, "dev")
	stamp := time.Unix(1643673600, 0)
	saved := map[string]string{}
	keep := func(file string) {
		t.Helper()
		if err := os.Chtimes(file, stamp, stamp); err != nil {
			t.Fatal(err)
		}
		saved[file] = readF(t, file)
	}
	const stock = "{\"~type\":\"idSnapMap\",\"entities\":[]}"
	const earlier = "{\"~type\":\"idSnapMap\",\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;90000\"}}}"
	const current = "{\"snapmap-plus\":{\"format\":2,\"mods\":[{\"mod\":\"a1234567\",\"local\":true,\"objects\":{},\"size\":0}]}}"
	names := []string{"smpkg.digest", "smpkg.my-preview", "smpmod1.favorite", "smpprod1.my-version", "smpprod1.v0.2.1", "smp.render.v1.custom", "smpref.foo"}
	rows := []map[string]any{}
	row := func(name, v string) map[string]any {
		return map[string]any{"info": map[string]string{"~type": "snapVarInfo_t", "name": name}, "initialValue": v}
	}
	for _, n := range names {
		rows = append(rows, row(n, "player value"))
	}
	rows = append(rows, row("smpmod1.0.1", "{\"id\":\"my object\"}"))
	vars := func(rows []map[string]any) string {
		data, err := json.Marshal(map[string]any{"~type": "idSnapMap", "variables": map[string]any{"allocCount": []int{0, 0, 0, 0, len(rows)}, "string": rows}})
		if err != nil {
			t.Fatal(err)
		}
		return string(data)
	}
	for leaf, body := range map[string]string{"maps/stock.json": stock, "maps/current.json": current, "maps/current-settings.json": `{"snapmap-plus":{"format":2,"settings":{"render":"4;8192","bounds":"1;16000"}}}`, "maps/variables.json": vars(rows),
		"maps/published-stand-in.json": `{"snapmap-plus":{"format":2,"mods":[{"mod":"a1234567","objects":{},"unavailable":["mods/a1234567/tyrant"],"size":0}]}}`,
		"maps/ambiguous.json":          "{\"snapmap-plus\":{\"format\":1,\"settings\":{\"author\":\"me\"},\"mods\":[]}}",
		"prefabs/vanilla.json":         stock, "prefabs/vanilla.meta.json": "{\"format\":1,\"description\":\"my own notes\"}",
		"mods/snapmap-plus.json": "{\"format\":1,\"claims\":{\"a1234567\":{}},\"my-note\":3}",
		"config.json":            "{\"schema_version\":1,\"settings\":{\"overrides.user_enabled\":false}}",
		"overrides/generated/decls/material/stock/file.decl": "my disabled file"} {
		file := filepath.Join(r.data, leaf)
		writeF(t, file, body)
		keep(file)
	}
	for _, leaf := range []string{"rawmaps/vanilla.zip", "prefabs/current.zip"} {
		file := filepath.Join(r.data, leaf)
		migrationZip(t, file, stock)
		keep(file)
	}
	home := filepath.Join(os.Getenv("USERPROFILE"), "snaphak")
	unrelated := filepath.Join(home, "prefabs", "neighbor.json")
	writeF(t, unrelated, stock)
	keep(unrelated)
	cloud := filepath.Join(os.Getenv("LOCALAPPDATA"), "id Software", "DOOM", "base", "doomsnapmaps")
	for _, kind := range []string{"saved-maps", "published-maps"} {
		for name, body := range map[string]string{"vanilla": stock, "old": earlier} {
			file := filepath.Join(cloud, kind, name+".decl")
			writeF(t, file, body)
			keep(file)
		}
	}
	root, err := nativeSaveRoot()
	if err != nil {
		t.Fatal(err)
	}
	for id, body := range map[string]string{"a": stock, "b": earlier} {
		file := filepath.Join(root, "701", "SNAPMAPS0000000000000000000"+id, "map.decl")
		slotBody(t, file, body)
		keep(file)
		writeF(t, file+".verify", "native record")
		keep(file + ".verify")
	}
	mixed := filepath.Join(r.data, "rawmaps", "mixed.json")
	writeF(t, mixed, vars(append(rows, row("smp.bounds.v1", "1;90000"))))
	archive := filepath.Join(r.data, "prefabs", "modded.zip")
	migrationZip(t, archive, earlier)
	for tries := 0; tries < 2; tries++ {
		plan, done, err := preparePlayerMigration(r.doom, r.data, "")
		defer done()
		if err != nil {
			t.Fatal(err)
		}
		if tries == 0 && (plan.Maps != 2 || plan.Prefabs != 1) {
			t.Fatalf("planned %d maps and %d prefabs", plan.Maps, plan.Prefabs)
		}
		if (tries == 0) != plan.ready() {
			t.Fatalf("attempt %d needs changes: %v", tries, plan.Changes)
		}
		if err = plan.commit(); err != nil {
			t.Fatal(err)
		}
		for file, old := range saved {
			info, err := os.Stat(file)
			if err != nil || !info.ModTime().Equal(stamp) || old != readF(t, file) {
				t.Fatalf("changed %s: %v", file, err)
			}
		}
		if exists(filepath.Join(r.data, "backups")) || exists(migrationJournal(r.data)) {
			t.Fatal("transaction leftovers")
		}
		if exists(filepath.Join(r.data, "mods", "my-overrides")) {
			t.Fatal("activated a disabled loose override")
		}
		if !strings.Contains(readF(t, mixed), "\"format\":2") {
			t.Fatal("missed earlier map")
		}
		for _, n := range names {
			if !strings.Contains(readF(t, mixed), n) {
				t.Fatalf("lost author variable %q", n)
			}
		}
		if tries == 0 {
			keep(mixed)
			keep(archive)
		}
	}
	copies, err := filepath.Glob(filepath.Join(r.data, "rawmaps", "Updated saves", "*.json"))
	if err != nil || len(copies) != 1 || filepath.Base(copies[0]) != "Map b.json" {
		t.Fatalf("native exports %v: %v", copies, err)
	}
}

func TestMigrationConsentAndRecovery(t *testing.T) {
	data := t.TempDir()
	config := filepath.Join(data, "config.json")
	orig := "{\"schema_version\":1,\"settings\":{\"mods.upload_confirmed\":true,\"mine\":4}}"
	writeF(t, config, orig)
	file := filepath.Join(data, "rawmap.json")
	writeF(t, file, "{\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;32000\"}}}")
	plan, clean, err := preparePlayerMigration("", data, "")
	defer clean()
	if err != nil {
		t.Fatal(err)
	}
	oldInput := migrationInput
	t.Cleanup(func() { migrationInput = oldInput })
	for _, k := range []struct {
		input string
		yes   bool
	}{
		{"y\n", true}, {"Y\r\n", true}, {"n\n", false}, {"\r\n", false},
		{"maybe\nY\n", false}, {"yes\n", false}, {"y", false}, {"", false},
	} {
		migrationInput = func() io.ReadCloser { return io.NopCloser(strings.NewReader(k.input)) }
		if yes := plan.consent(false); yes != k.yes {
			t.Fatalf("answer %q: %v", k.input, yes)
		}
	}
	migrationInput = func() io.ReadCloser { return io.NopCloser(iotest.ErrReader(io.ErrClosedPipe)) }
	if plan.consent(false) {
		t.Fatal("accepted a failed keyboard read")
	}
	migrationInput = func() io.ReadCloser { return nil }
	if plan.consent(false) {
		t.Fatal("accepted migration without a keyboard")
	}
	if readF(t, config) != orig || exists(migrationJournal(data)) {
		t.Fatal("preflight modified player data")
	}
	if err := plan.persist(); err != nil {
		t.Fatal(err)
	}
	if err := copyMigrationFile(filepath.Join(plan.work, "new-0"), plan.Changes[0].Path, plan.Changes[0].Before); err != nil {
		t.Fatal(err)
	}
	oldFile := readF(t, file)
	writeF(t, file, "independent edit")
	resumed, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	if err := resumed.commit(); err == nil || readF(t, file) != "independent edit" {
		t.Fatal("overwrote concurrent edit")
	}
	writeF(t, file, oldFile)
	if err := resumed.commit(); err != nil {
		t.Fatal(err)
	}
	if exists(migrationJournal(data)) || strings.Contains(readF(t, config), "mods.upload_confirmed") || !strings.Contains(readF(t, config), "mine") {
		t.Fatal("incomplete recovery")
	}
}

func TestMigrationRequiresCompletePriorRowsAndExactExternalSource(t *testing.T) {
	data := t.TempDir()
	original := tinyFixture(t, data)
	file := filepath.Join(data, "rawmap.json")
	record := string(pinJSON(tinyPin("mods/alex.tiny/a")))
	body := fmt.Sprintf("{\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[{\"info\":{\"name\":\"smpmod1.0.2\",\"~type\":\"snapVarInfo_t\"},\"initialValue\":%s}]}}", pinJSON(record))
	writeF(t, file, body)
	plan, done, err := preparePlayerMigration("", data, "")
	done()
	if err == nil || plan != nil || !strings.Contains(err.Error(), "missing 1 of 2") || readF(t, file) != body || exists(filepath.Join(data, "cache")) {
		t.Fatalf("partial dependency preflight: %v", err)
	}
	if err = os.Remove(file); err != nil {
		t.Fatal(err)
	}
	notes := filepath.Join(original, ".build", "artist-notes.txt")
	writeF(t, notes, "these are mine")
	if err = convertEarlierData("", data); err != nil {
		t.Fatal(err)
	}
	if readF(t, notes) != "these are mine" {
		t.Fatal("discarded author extras")
	}
	p := tinyResolver(t, data, nil)
	out, err := p.row(tinyPin("mods/alex.tiny/a"))
	if err == nil || out != nil || !strings.Contains(err.Error(), tinyBuild) || exists(filepath.Join(data, "cache", "legacy-builds")) {
		t.Fatalf("later export without its exact original was accepted: %s %v", pinJSON(out), err)
	}
}

func TestMigrationDeclinesConflictingMyOverridesFolder(t *testing.T) {
	data := t.TempDir()
	current := filepath.Join(data, "mods", "my-overrides", "mod.json")
	writeF(t, current, `{"name":"This is my own unrelated mod"}`)
	loose := filepath.Join(data, "overrides", "generated", "decls", "material", "mine", "lamp.decl")
	writeF(t, loose, "original")
	p, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err == nil || p != nil || readF(t, loose) != "original" || readF(t, current) != `{"name":"This is my own unrelated mod"}` || exists(migrationJournal(data)) {
		t.Fatalf("unrelated mod collides: %v", err)
	}
}

func TestMigrationKeepsUnrecognizedConfigAndConvertsNeighbor(t *testing.T) {
	for _, cfg := range []string{"broken json", `{"schema_version":2,"settings":{"new.option":true}}`} {
		data := t.TempDir()
		file := filepath.Join(data, "config.json")
		writeF(t, file, cfg)
		loose := filepath.Join(data, "overrides", "generated", "decls", "material", "demo", "lamp.decl")
		writeF(t, loose, "held by existing settings")
		mapfile := filepath.Join(data, "rawmap.json")
		writeF(t, mapfile, `{"snapmap-plus":{"format":1,"settings":{"bounds":"1;32000"}}}`)
		if err := convertEarlierData("", data); err != nil {
			t.Fatal(err)
		}
		if readF(t, loose) != "held by existing settings" || readF(t, file) != cfg || !strings.Contains(readF(t, mapfile), `"format":2`) {
			t.Fatal("unknown preferences interfered with a valid modded map")
		}
	}
}

func TestMigrationRefusesConcurrentWriter(t *testing.T) {
	data := t.TempDir()
	file := filepath.Join(data, "rawmap.json")
	orig := `{"snapmap-plus":{"format":1,"settings":{"bounds":"1;32000"}}}`
	writeF(t, file, orig)
	p, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	unlock, err := lockMigration(data)
	if err != nil {
		t.Fatal(err)
	}
	blocked := p.commit()
	unlock()
	if blocked == nil || readF(t, file) != orig || exists(migrationJournal(data)) {
		t.Fatalf("duplicate writer: %v", blocked)
	}
	if err = p.commit(); err != nil {
		t.Fatal(err)
	}
}

func TestMigrationDoesNotChangeEmptyJournalBeforeConsent(t *testing.T) {
	data := t.TempDir()
	dir := migrationJournal(data)
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	original, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	unchanged := func() {
		t.Helper()
		current, err := os.Stat(dir)
		if err != nil || !os.SameFile(original, current) || !current.ModTime().Equal(original.ModTime()) {
			t.Fatal("preview changed the empty journal", err)
		}
	}
	file := filepath.Join(data, "stock.json")
	writeF(t, file, `{"~type":"idSnapMap","entities":[]}`)
	if err := convertWithConsent(data, file, true); err != nil {
		t.Fatal(err)
	}
	unchanged()
	config := filepath.Join(data, "config.json")
	writeF(t, config, `{"schema_version":1,"settings":{"mods.upload_confirmed":true,"theme":"dark"}}`)
	p, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil || !p.ready() {
		t.Fatal("expected one config update", err)
	}
	unchanged()
	if err := p.commit(); err != nil {
		t.Fatal(err)
	}
	if exists(dir) || readF(t, config) != `{"schema_version":1,"settings":{"theme":"dark"}}` {
		t.Fatal("confirmed cleanup failed")
	}
}

func TestResumingMigrationChecksTheSamePlanAfterConsent(t *testing.T) {
	data := t.TempDir()
	config := filepath.Join(data, "config.json")
	original := `{"schema_version":1,"settings":{"packages.embed_in_saved_maps":false}}`
	writeF(t, config, original)
	p, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	if err := p.persist(); err != nil {
		t.Fatal(err)
	}
	journal := filepath.Join(p.work, "plan.json")
	if entries, err := os.ReadDir(p.work); err != nil || len(entries) != 2 || exists(filepath.Join(p.work, "old-0")) {
		t.Fatal("journal should contain only the plan and its replacement", err)
	}
	resuming, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	body := readF(t, journal)
	writeF(t, journal, body+"\n")
	if err := resuming.commit(); err == nil || readF(t, config) != original || readF(t, journal) != body+"\n" {
		t.Fatal("resumed a different journal")
	}
	writeF(t, journal, body)
	// An interrupted earlier installer can still have its old copy.
	writeF(t, filepath.Join(p.work, "old-0"), original)
	if err := resuming.commit(); err != nil || exists(p.work) {
		t.Fatal("interrupted update did not finish", err)
	}
}

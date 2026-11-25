package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInstallEarlierContentAndRealPins(t *testing.T) {
	if len(earlierUnitBinary) == 0 {
		t.Skip("run the release installer checks with -tags earlier_units")
	}
	r := newUpdateRig(t, "dev")
	tinyFixture(t, r.data)
	original := `{"entities":[{"entityDef":{"inherit":"mods/alex.tiny/a"}}],"snapmap-plus":{"format":1,"mods":[` + string(pinJSON(tinyPin("mods/alex.tiny/a"))) + `]}}`
	doc := filepath.Join(r.data, "rawmap.json")
	writeF(t, doc, original)
	var embedded bytes.Buffer
	z := zip.NewWriter(&embedded)
	for name, body := range map[string]string{"package.json": `{"id":"local.door","name":"Door"}`, "assets/generated/decls/entitydef/mods/local.door/a.decl": `{ inherit = ""; }`} {
		w, e := z.Create(name)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = w.Write([]byte(body)); e != nil {
			t.Fatal(e)
		}
	}
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(embedded.Bytes())
	carrier := fmt.Sprintf(`{"entities":[{"entityDef":{"inherit":"mods/local.door/a"}}],"variables":{"allocCount":[0,0,0,0,1],"string":[{"info":{"name":"smpkg.local.door.0.1.%x","~type":"snapVarInfo_t"},"initialValue":"%s"}]}}`, hash[:8], base64.StdEncoding.EncodeToString(embedded.Bytes()))
	slotPath := filepath.Join(os.Getenv("USERPROFILE"), "Saved Games", "id Software", "DOOM", "base", "savegame.user", "fixture", "SNAPMAPSab01", "map.decl")
	native := slotBody(t, slotPath, carrier)
	writeF(t, slotPath+".verify", "test integrity")
	dist := synthDist(t, r.tmp, "upgrade")
	marker := "backend " + modBuildManifest
	writeF(t, filepath.Join(dist, currentProxy), marker)
	m := filepath.Join(dist, "MANIFEST.sha256")
	writeF(t, m, strings.Replace(readF(t, m), sha256Hex([]byte("backend-upgrade")), sha256Hex([]byte(marker)), 1))
	flags := flags{local: dist, doom: r.doom, yes: true, noSelf: true}
	if err := cmdInstall(flags); err != nil {
		t.Fatal(err)
	}
	upgraded := readF(t, doc)
	if !strings.Contains(upgraded, `"objects":{`) || !strings.Contains(upgraded, "mods/"+earlierSalt("alex.tiny")+"/a") || strings.Contains(upgraded, tinyBuild) {
		t.Fatalf("map kept wrong build: %s", upgraded)
	}
	copies, _ := filepath.Glob(filepath.Join(r.data, "rawmaps", "Updated saves", "*.json"))
	if len(copies) != 1 || strings.Contains(readF(t, copies[0]), "smpkg.") || !strings.Contains(readF(t, copies[0]), `"objects":{`) || readF(t, slotPath) != string(native) || readF(t, slotPath+".verify") != "test integrity" {
		t.Fatalf("native save copies: %v", copies)
	}
	before, _ := earlierTreeHash(filepath.Join(r.data, "backups"))
	if err := cmdUpdate(flags); err != nil {
		t.Fatal(err)
	}
	after, _ := earlierTreeHash(filepath.Join(r.data, "backups"))
	if before != after || readF(t, doc) != upgraded {
		t.Fatal("second update changed converted documents or backups")
	}
}

func TestEarlierVariableMap(t *testing.T) {
	variable := func(name, value string) map[string]any {
		return map[string]any{"info": map[string]any{"name": name, "~type": "snapVarInfo_t"}, "initialValue": value, "~type": "snapVarString_t"}
	}
	vars := map[string]any{"allocCount": []int{0, 0, 0, 0, 6}, "string": []any{
		variable("smp.bounds.v1", "1;32672"), variable("String 0", "bound by index"),
		variable("smpprod1.v0.2", ""), variable("smnav1.cat/room.monster48.1.2.b5d4045c3f466fa9.0123456789abcdef", "JD"),
		variable("smnav1.cat/room.monster48.0.2.b5d4045c3f466fa9.0123456789abcdef", "QU"),
		variable("smpmod1.0.1", `{"mod":"a1234567@`+strings.Repeat("0", 64)+`","names":[]}`),
	}}
	b, _ := json.Marshal(map[string]any{"entities": []any{}, "variables": vars})
	next, changed, err := convertEarlierJSON(b, nil)
	if err != nil || !changed {
		t.Fatal(err)
	}
	for _, want := range []string{`"allocCount":[0,0,0,0,2]`, `"initialValue":"bound by index"`, `"name":"String 0 (restored)"`, `"bounds":"1;32671"`, `"data":["QU","JD"]`, `"local":true`, `"format":2`} {
		if !bytes.Contains(next, []byte(want)) {
			t.Errorf("missing %s in %s", want, next)
		}
	}
	for _, prefix := range []string{"smp.bounds.", "smpmod1.", "smpprod1.", "smnav1."} {
		if bytes.Contains(next, []byte(prefix)) {
			t.Errorf("retained %s in %s", prefix, next)
		}
	}
	bad := bytes.ReplaceAll(b, []byte("b5d4045c3f466fa9"), []byte("b5d4045c3f466fa8"))
	if _, _, err := convertEarlierJSON(bad, nil); err == nil {
		t.Error("damaged navigation was silently kept")
	}
	data, root := t.TempDir(), t.TempDir()
	slotBody(t, filepath.Join(root, "profile", "SNAPMAPSabcd", "map.decl"), string(b))
	paths, err := convertEarlierNativeSaves(data, root)
	if err != nil || len(paths) != 1 {
		t.Fatalf("map slot with variables only: %v, %v", paths, err)
	}
	if !strings.Contains(readF(t, paths[0]), `"format":2`) {
		t.Fatal("no converted member in native map copy")
	}
}

func TestEarlierExport(t *testing.T) {
	data := t.TempDir()
	t.Setenv("LOCALAPPDATA", data)
	file := filepath.Join(t.TempDir(), "Dropped Room.zip")
	var input bytes.Buffer
	w := zip.NewWriter(&input)
	for name, value := range map[string]string{
		"Room.json":        `{"~type":"idSnapEntityPrefab","entities":[]}`,
		"prefab.meta.json": `{"desc":"A room","scripts":["mymodule.lua"]}`,
		"export.json":      `{"format":1,"bundles":[]}`, "files/preserved": "verbatim",
	} {
		s, err := w.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = s.Write([]byte(value)); err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	writeF(t, file, input.String())
	if err := runCommand(file, nil, false); err != nil {
		t.Fatal(err)
	}
	z, err := zip.OpenReader(file)
	if err != nil {
		t.Fatal(err)
	}
	if len(z.File) != 2 {
		t.Fatalf("archive: %+v", z.File)
	}
	for _, f := range z.File {
		value, err := earlierEntry(f)
		if err != nil {
			t.Fatal(err)
		}
		if f.Name == "Room.json" && (!bytes.Contains(value, []byte(`"description":"A room"`)) || !bytes.Contains(value, []byte(`"scripts":["mymodule.lua"]`))) {
			t.Errorf("document %s", value)
		}
		if f.Name == "files/preserved" && string(value) != "verbatim" {
			t.Errorf("unexpected file content %s", value)
		}
	}
	z.Close()
	before := readF(t, file)
	if err := runCommand("convert", []string{file}, false); err != nil || readF(t, file) != before {
		t.Fatalf("retry: %v", err)
	}
	backup, _ := filepath.Glob(filepath.Join(appDataDir(), "backups", "earlier-data", "*", "*.zip"))
	if len(backup) != 1 || readF(t, backup[0]) != input.String() {
		t.Fatal("missing original export")
	}
}

func TestEarlierMapAndPrefabFiles(t *testing.T) {
	data := t.TempDir()
	zero := strings.Repeat("0", 64)
	rawmap := filepath.Join(data, "rawmap.json")
	prefab := filepath.Join(data, "prefabs", "Room.json")
	meta := filepath.Join(data, "prefabs", "Room.meta.json")
	engine := "/* object */ \"entities\" : [  ]"
	row := fmt.Sprintf(`{"mod":"a1234567@%s","size":42,"names":["door"],"attached":true,"choices":{"key":"a1234567"}}`, zero)
	oldMap := "\ufeff{" + engine + `,"url":"https://example.test","snapmap-plus":{"format":1,"settings":{"bounds":"1;32000","render":"3;10000;0;0;0;1;1;1","other":"kept"},"mods":[` + row + `],"nav":[{"module":"room"}]}}`
	side := `{"desc":"An author's room","tags":["mine"],"mods":[` + row + `],"scripts":[{"content":"function onLoad() end"}],"thing":7}`
	writeF(t, rawmap, oldMap)
	writeF(t, prefab, "{"+engine+`,"~type":"idSnapMap"}`)
	writeF(t, meta, side)
	writeF(t, filepath.Join(data, "rawmaps", "stock.json"), `{"~type":"idSnapMap"}`)
	writeF(t, filepath.Join(data, "maps", "empty.json"), `{"snapmap-plus":{"format":1,"mods":[]}}`)
	if err := convertEarlierData("", data); err != nil {
		t.Fatal(err)
	}
	out := readF(t, rawmap)
	for _, keep := range []string{engine, `"format":2`, `"render":"4;0"`, `"bounds":"1;32671"`, `"other":"kept"`, `"attached":true`, `"mod":"a1234567"`, `"local":true`, `"nav":[{"module":"room"}]`} {
		if !strings.Contains(out, keep) {
			t.Errorf("missing %s in %s", keep, out)
		}
	}
	got := readF(t, prefab)
	if exists(meta) || !strings.Contains(got, `"description":"An author's room"`) || !strings.Contains(got, `"scripts":[{"content":"function onLoad() end"}]`) ||
		!strings.Contains(got, `"thing":7`) || !strings.Contains(got, engine) {
		t.Fatalf("prefab %s", got)
	}
	if !strings.Contains(readF(t, filepath.Join(data, "maps", "empty.json")), `"format":2`) {
		t.Fatal("empty format-1 map was not updated")
	}
	backups, _ := filepath.Glob(filepath.Join(data, "backups", "earlier-data", "*", "*"))
	if len(backups) != 4 {
		t.Errorf("backup count %d", len(backups))
	}
	// Resume after writing the inline document but before retiring its sidecar.
	writeF(t, meta, side)
	if err := convertEarlierData("", data); err != nil || exists(meta) || readF(t, prefab) != got || readF(t, rawmap) != out {
		t.Fatalf("resume: %v", err)
	}
}

func TestEarlierDocumentKeepsConflictsAndUnsupportedPins(t *testing.T) {
	data := t.TempDir()
	p := filepath.Join(data, "prefabs", "room.json")
	side := filepath.Join(data, "prefabs", "room.meta.json")
	body := `{"~type":"idSnapEntityPrefab","snapmapPlus":{"format":1,"description":"edited inline","tags":[],"mods":[]}}`
	writeF(t, p, body)
	writeF(t, side, `{"desc":"edited separately"}`)
	if err := convertEarlierData("", data); err == nil || readF(t, p) != body || !exists(side) {
		t.Errorf("conflict: %v", err)
	}
	raw := filepath.Join(data, "rawmap.json")
	pinned := `{"snapmap-plus":{"format":1,"mods":[{"mod":"a1234567@` + strings.Repeat("f", 64) + `","size":1}]}}`
	writeF(t, raw, pinned)
	if err := convertEarlierDocument(data, raw, false, nil); err == nil || readF(t, raw) != pinned {
		t.Errorf("missing pin: %v", err)
	}
	_, err := os.Stat(filepath.Join(data, "backups"))
	if !os.IsNotExist(err) {
		t.Fatalf("unexpected backup for a refused change: %v", err)
	}
}

func TestEarlierSettings(t *testing.T) {
	for _, c := range []struct{ key, old, want string }{
		{"bounds", "1;16000.8", ""}, {"bounds", "1;99999", "1;32671"}, {"render", "4;8192", ""},
		{"render", "1;0;0;0;0;0;0;0", "4;0"}, {"render", "2;0;16000;1;2;3;4;5;6", ""},
		{"render", "2;1;8192;1;2;3;4;5;6", ""}, {"render", "4;224", "ERROR"}, {"render", "4;NaN", "ERROR"},
	} {
		value, err := earlierSetting(c.key, c.old)
		if c.want == "ERROR" && err == nil || c.want != "ERROR" && (err != nil || value != c.want) {
			t.Errorf("%s %s -> %s, %v", c.key, c.old, value, err)
		}
	}
	current := []byte(`{ "snapmap-plus": {"format":2,"mods":[],"settings":{"bounds":"1;32671","render":"4;0"}} }`)
	out, changed, err := convertEarlierJSON(current, nil)
	if err != nil || changed || !bytes.Equal(out, current) || !json.Valid(out) {
		t.Fatal(err)
	}
}

func TestEarlierResolvedMapNamespaces(t *testing.T) {
	src := []byte(`{"note":"myothermods/alex.old/leave","entities":[{"inherit":"mods/alex.old/npc","moduleName":"maps/modules/mods/alex.old/room"}],"snapmap-plus":{"format":1,"mods":[{"id":"alex.old","bundle":"retained"}]}}`)
	resolve := func(row earlierObject) (earlierObject, error) {
		return earlierObject{"mod": json.RawMessage(`"a1234567"`), "whole": json.RawMessage(`"checked"`)}, nil
	}
	out, changed, err := convertEarlierJSONWithRows(src, nil, resolve)
	if err != nil || !changed || !bytes.Contains(out, []byte(`"mods/a1234567/npc"`)) || !bytes.Contains(out, []byte(`"maps/modules/mods/a1234567/room"`)) || !bytes.Contains(out, []byte(`"myothermods/alex.old/leave"`)) {
		t.Fatalf("map namespace %s: %v", out, err)
	}
}

package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func placedBetaDoc(typ, actor string) string {
	return string(pinJSON(map[string]any{"~type": typ, "entities": []any{map[string]any{"~type": "idSnapEntity", "displayName": "boss", "entityDef": map[string]any{"~type": "idDeclEntityDef", "targetType": "idDeclEntityDef", "inherit": actor, "className": "idAI2", "state": map[string]any{"edit": map[string]any{"spawnPosition": map[string]any{"x": 10}}}}}}}))
}

func TestUnembeddedEarlierPlacedEntity(t *testing.T) {
	useEarlierUnitTool(t)
	r := newUpdateRig(t, "dev")
	oneStockRequest(t, r.doom, "generated/decls/entitydef/stock/actor.decl")
	// The beta.19 old-boss fixture supplied a global decls/entitydef/boss.decl.
	src := filepath.Join(r.data, "overrides", "old-boss")
	writeF(t, filepath.Join(src, "package.json"), "{\"id\":\"old-boss\",\"name\":\"Older actor\"}")
	writeF(t, filepath.Join(src, "decls/entitydef/boss.decl"), "{ class = \"idAI2\"; edit = {} }")
	writeF(t, filepath.Join(src, "decls/entitydef/stock/actor.decl"), "{ class = \"idAI2\"; edit = {} }")
	writeF(t, filepath.Join(r.data, "overrides/generated/decls/entitydef/member/loose.decl"), "{ class = \"idAI2\"; edit = {} }")
	raw := filepath.Join(r.data, "rawmaps", "BetaBoss.json")
	body := placedBetaDoc("idSnapMap", "boss")
	writeF(t, raw, body)
	prefab := filepath.Join(r.data, "prefabs", "Loose.json")
	writeF(t, prefab, placedBetaDoc("idSnapEntityPrefab", "member/loose"))
	noChange := map[string]string{}
	stamp := time.Unix(1600100000, 0)
	for k, val := range map[string]string{"stock": placedBetaDoc("idSnapMap", "stock/actor"), "lookalike": placedBetaDoc("idSnapMap", "boss-extra"), "untyped": strings.ReplaceAll(body, "idSnapEntity", "customPlayerMetadata"), "current": strings.Replace(body, "{", "{\"snapmap-plus\":{\"format\":2,\"mods\":[]},", 1)} {
		f := filepath.Join(r.data, "rawmaps", k+".json")
		writeF(t, f, val)
		noChange[f] = val
		if err := os.Chtimes(f, stamp, stamp); err != nil {
			t.Fatal(err)
		}
	}
	root, err := nativeSaveRoot()
	if err != nil {
		t.Fatal(err)
	}
	slot := filepath.Join(root, "test", "SNAPMAPS321", "map.decl")
	original := slotBody(t, slot, body)
	writeF(t, slot+".verify", "unaltered")
	published := filepath.Join(os.Getenv("LOCALAPPDATA"), "id Software", "DOOM", "base", "doomsnapmaps", "published-maps", "Early.decl")
	writeF(t, published, body)
	earlyDrop := filepath.Join(t.TempDir(), "first-drop.json")
	writeF(t, earlyDrop, body)
	selection, clean, e := preparePlayerMigration(r.doom, r.data, earlyDrop)
	if e != nil {
		t.Fatal(e)
	}
	if e = selection.commit(); e != nil {
		t.Fatal(e)
	}
	clean()
	if !exists(filepath.Join(src, "decls/entitydef/boss.decl")) || exists(filepath.Join(r.data, "mods/old-boss")) || !strings.Contains(readF(t, earlyDrop), earlierSalt("old-boss")) {
		t.Fatal("first drop changed the library or missed the actor")
	}
	plan, done, err := preparePlayerMigration(r.doom, r.data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	if err = plan.commit(); err != nil {
		t.Fatal(err)
	}
	check := func(text, kind, id, leaf string) {
		t.Helper()
		var doc earlierObject
		if err := json.Unmarshal([]byte(text), &doc); err != nil {
			t.Fatal(err)
		}
		var b struct {
			Format int
			Mods   []earlierObject
		}
		if err := json.Unmarshal(doc[kind], &b); err != nil || len(b.Mods) != 1 {
			t.Fatalf("unexpected member: %s", text)
		}
		var objects map[string]string
		r := b.Mods[0]
		if err := json.Unmarshal(r["objects"], &objects); err != nil || len(objects) != 1 || !pinHash(objects["entities/"+leaf]) || pinString(r, "mod") != earlierSalt(id) || r["whole"] != nil {
			t.Fatalf("unexpected pin: %s", pinJSON(r))
		}
		if !strings.Contains(text, "\"inherit\":\"mods/"+earlierSalt(id)+"/"+leaf+"\"") || !strings.Contains(text, "\"displayName\":\"boss\"") {
			t.Fatal(text)
		}
	}
	check(readF(t, raw), "snapmap-plus", "old-boss", "boss")
	check(readF(t, prefab), "snapmapPlus", "my-overrides", "member/loose")
	check(readF(t, filepath.Join(r.data, "rawmaps", "Updated saves", "Map 321.json")), "snapmap-plus", "old-boss", "boss")
	check(readF(t, filepath.Join(r.data, "rawmaps", "Updated saves", "Early.json")), "snapmap-plus", "old-boss", "boss")
	if !bytes.Equal([]byte(readF(t, slot)), original) || readF(t, slot+".verify") != "unaltered" || readF(t, published) != body {
		t.Fatal("native source changed")
	}
	for f, v := range noChange {
		st, e := os.Stat(f)
		if e != nil || !st.ModTime().Equal(stamp) || readF(t, f) != v {
			t.Fatalf("unrelated file changed %s", f)
		}
	}
	if !exists(filepath.Join(r.data, "mods/old-boss/native/generated/decls/entitydef/stock/actor.decl")) {
		t.Fatal("genuine game shadow moved")
	}
	// A stale mapping cannot take a currently installed game name away from a vanilla placement.
	inv := filepath.Join(r.data, "cache/legacy-placements.json")
	originalInv := readF(t, inv)
	var cat struct {
		Format int
		Owners []earlierOwner
	}
	if err = json.Unmarshal([]byte(originalInv), &cat); err != nil {
		t.Fatal(err)
	}
	for i := range cat.Owners {
		if cat.Owners[i].Salt == earlierSalt("old-boss") {
			cat.Owners[i].Entities["stock/actor"] = "entities/boss"
		}
	}
	writeF(t, inv, string(pinJSON(cat)))
	vanilla := filepath.Join(t.TempDir(), "vanilla.json")
	writeF(t, vanilla, placedBetaDoc("idSnapMap", "stock/actor"))
	st, _ := os.Stat(vanilla)
	ctrl, finish, e := preparePlayerMigration(r.doom, r.data, vanilla)
	if e != nil || ctrl.ready() {
		t.Fatalf("stale mapping changed a stock placement: %v", e)
	}
	finish()
	end, _ := os.Stat(vanilla)
	if !end.ModTime().Equal(st.ModTime()) {
		t.Fatal("rewrote vanilla source")
	}
	writeF(t, inv, originalInv)
	for _, kind := range []string{"idSnapMap", "idSnapEntityPrefab"} {
		file := filepath.Join(t.TempDir(), "late.zip")
		writeF(t, file, string(zipFiles(t, map[string]string{"object.json": placedBetaDoc(kind, "boss")})))
		later, close, e := preparePlayerMigration(r.doom, r.data, file)
		if e != nil {
			t.Fatal(e)
		}
		if e = later.commit(); e != nil {
			t.Fatal(e)
		}
		close()
		z, e := zip.OpenReader(file)
		if e != nil {
			t.Fatal(e)
		}
		found := false
		content := 0
		for _, entry := range z.File {
			if entry.Name == "object.json" {
				v, e := earlierEntry(entry)
				if e != nil {
					t.Fatal(e)
				}
				member := "snapmap-plus"
				if kind == "idSnapEntityPrefab" {
					member = "snapmapPlus"
				}
				check(string(v), member, "old-boss", "boss")
				found = true
			}
			if strings.HasPrefix(entry.Name, "units/") || strings.HasPrefix(entry.Name, "files/") {
				content++
			}
		}
		z.Close()
		if !found || content < 2 {
			t.Fatalf("incomplete portable zip: %d", content)
		}
		again, stop, e := preparePlayerMigration(r.doom, r.data, file)
		if e != nil || again.ready() {
			t.Fatalf("second archive changed: %v", e)
		}
		stop()
	}
	later := filepath.Join(t.TempDir(), "late.json")
	writeF(t, later, body)
	again, close, e := preparePlayerMigration(r.doom, r.data, later)
	if e != nil {
		t.Fatal(e)
	}
	if e = again.commit(); e != nil {
		t.Fatal(e)
	}
	close()
	check(readF(t, later), "snapmap-plus", "old-boss", "boss")
	steady, stop, e := preparePlayerMigration(r.doom, r.data, "")
	if e != nil || steady.ready() {
		t.Fatalf("second pass has changes: %v %+v", e, steady)
	}
	stop()
}

func TestAmbiguousUnembeddedPlacementRetainsBothOwners(t *testing.T) {
	useEarlierUnitTool(t)
	r := newUpdateRig(t, "dev")
	oneStockRequest(t, r.doom, "generated/decls/entitydef/stock/actor.decl")
	for _, id := range []string{"owner-1", "owner-2"} {
		d := filepath.Join(r.data, "overrides", id)
		writeF(t, filepath.Join(d, "package.json"), "{\"name\":\"Old\"}")
		writeF(t, filepath.Join(d, "decls/entitydef/boss.decl"), "{ class = \"idAI2\"; edit = {} }")
	}
	raw := filepath.Join(r.data, "rawmaps", "boss.json")
	original := placedBetaDoc("idSnapMap", "boss")
	writeF(t, raw, original)
	plan, done, err := preparePlayerMigration(r.doom, r.data, "")
	defer done()
	if err == nil || !strings.Contains(err.Error(), "multiple earlier mods") || plan != nil {
		t.Fatalf("ambiguous owner: %v", err)
	}
	if readF(t, raw) != original || !exists(filepath.Join(r.data, "overrides/owner-1/decls/entitydef/boss.decl")) || !exists(filepath.Join(r.data, "overrides/owner-2/decls/entitydef/boss.decl")) || exists(filepath.Join(r.data, "cache", "legacy-placements.json")) {
		t.Fatal("ambiguous preflight changed a source")
	}
}

func TestMissingExactBuildSkipsOnlyDependentDocuments(t *testing.T) {
	r := newUpdateRig(t, "dev")
	rows := []earlierObject{
		{"id": pinJSON("one.old"), "version": pinJSON("1"), "bundle": pinJSON(strings.Repeat("1", 64))},
		{"id": pinJSON("two.old"), "version": pinJSON("2"), "bundle": pinJSON(strings.Repeat("2", 64))},
	}
	var vars []map[string]any
	for i, row := range rows {
		vars = append(vars, map[string]any{"info": map[string]any{"~type": "snapVarInfo_t", "name": fmt.Sprintf("smpmod1.%d.2", i)}, "initialValue": string(pinJSON(row))})
	}
	missing := string(pinJSON(map[string]any{"~type": "idSnapMap", "variables": map[string]any{"string": vars}}))
	healthy := "{\"~type\":\"idSnapMap\",\"snapmap-plus\":{\"format\":1,\"settings\":{\"bounds\":\"1;90000\"}}}"
	stock := placedBetaDoc("idSnapMap", "ai/demon/baron_coop")
	root, err := nativeSaveRoot()
	if err != nil {
		t.Fatal(err)
	}
	for id, doc := range map[string]string{"aaa": missing, "bbb": healthy, "ccc": stock} {
		source := filepath.Join(root, "user", "SNAPMAPS"+id, "map.decl")
		slotBody(t, source, doc)
		writeF(t, source+".verify", "game verification")
	}
	raw := filepath.Join(r.data, "rawmaps", "Unavailable.json")
	writeF(t, raw, missing)
	archive := filepath.Join(r.data, "rawmaps", "Unavailable.zip")
	writeF(t, archive, string(zipFiles(t, map[string]string{"old.json": missing})))
	neighbor := filepath.Join(r.data, "rawmaps", "Neighbor.json")
	writeF(t, neighbor, healthy)
	before := map[string]string{}
	times := map[string]time.Time{}
	for _, f := range append([]string{raw, archive}, filepath.Join(root, "user", "SNAPMAPSaaa", "map.decl"), filepath.Join(root, "user", "SNAPMAPSaaa", "map.decl.verify"), filepath.Join(root, "user", "SNAPMAPSccc", "map.decl")) {
		before[f] = readF(t, f)
		st, _ := os.Stat(f)
		times[f] = st.ModTime()
	}
	plan, done, err := preparePlayerMigration(r.doom, r.data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	if len(plan.Deferred) != 3 {
		t.Fatalf("unreported missing documents: %v", plan.Deferred)
	}
	for _, notice := range plan.Deferred {
		for _, row := range rows {
			if !strings.Contains(notice, pinString(row, "id")+" @ "+pinString(row, "bundle")) {
				t.Fatal(notice)
			}
		}
	}
	if err = plan.commit(); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(readF(t, neighbor), "\"format\":2") || !exists(filepath.Join(r.data, "rawmaps", "Updated saves", "Map bbb.json")) {
		t.Fatal("healthy neighbors blocked")
	}
	for f, old := range before {
		st, e := os.Stat(f)
		if e != nil || !st.ModTime().Equal(times[f]) || readF(t, f) != old {
			t.Fatalf("mutated %s", f)
		}
	}
	if exists(filepath.Join(r.data, "rawmaps", "Updated saves", "Map aaa.json")) || exists(filepath.Join(r.data, "rawmaps", "Updated saves", "Map ccc.json")) {
		t.Fatal("unexpected native copy")
	}
	for _, f := range []string{raw, archive} {
		one, close, e := preparePlayerMigration(r.doom, r.data, f)
		close()
		var absent *missingEarlierPins
		if one != nil || !errors.As(e, &absent) || len(absent.items) != 2 {
			t.Fatalf("explicit request must report both missing pins: %v", e)
		}
	}
}

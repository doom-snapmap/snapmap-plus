package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func earlierTestMap(rows ...earlierObject) string {
	return string(pinJSON(map[string]any{"~type": "idSnapMap", "snapmap-plus": map[string]any{"format": 1, "mods": rows}}))
}

func archiveNames(t *testing.T, path string) map[string]bool {
	t.Helper()
	z, err := zip.OpenReader(path)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	names := map[string]bool{}
	for _, f := range z.File {
		if names[f.Name] {
			t.Fatalf("duplicate %s", f.Name)
		}
		names[f.Name] = true
		data, err := earlierEntry(f)
		if err != nil {
			t.Fatal(err)
		}
		if strings.HasPrefix(f.Name, "files/") || strings.HasPrefix(f.Name, "units/") {
			if fmt.Sprintf("%x", sha256.Sum256(data)) != strings.TrimSuffix(filepath.Base(f.Name), ".json") {
				t.Fatalf("wrong digest for %s", f.Name)
			}
		}
	}
	return names
}

func TestHistoryRetiresOnlyVerifiedSnapshotAndSelectedUnit(t *testing.T) {
	useEarlierUnitTool(t)
	data := t.TempDir()
	snap, _ := frozenTiny(t, data)
	note := filepath.Join(snap, "author notes.txt")
	writeF(t, note, "keep")
	cache := filepath.Join(data, "cache", "legacy-builds", tinyBuild)
	writeF(t, filepath.Join(cache, "bundle.canonical.json"), readF(t, "testdata/earlier-tiny/bundle.canonical.json"))
	unknown := filepath.Join(data, "mods", ".builds", "my copy", "bundle.json")
	writeF(t, unknown, "user's other tool")
	stock := filepath.Join(data, "rawmaps", "vanilla.json")
	writeF(t, stock, "{ \"~type\":\"idSnapMap\" }")
	before, err := os.Stat(stock)
	if err != nil {
		t.Fatal(err)
	}
	selected := filepath.Join(data, "rawmaps", "room.json")
	original := earlierTestMap(tinyPin("mods/alex.tiny/a"))
	writeF(t, selected, original)
	collision := companionPath(selected, 0)
	writeF(t, collision, "unrelated existing export")
	plan, done, err := preparePlayerMigration("", data, "")
	defer done()
	if err != nil {
		t.Fatal(err)
	}
	if !isRegular(filepath.Join(snap, "bundle.json")) || readF(t, selected) != original || !isRegular(filepath.Join(cache, "bundle.canonical.json")) {
		t.Fatal("preflight changed inputs")
	}
	if err = plan.commit(); err != nil {
		t.Fatal(err)
	}
	if readF(t, note) != "keep" || readF(t, unknown) != "user's other tool" || readF(t, collision) != "unrelated existing export" || isRegular(filepath.Join(snap, "bundle.json")) || earlierPathExists(cache) {
		t.Fatal("unexpected snapshot or collision disposition")
	}
	after, err := os.Stat(stock)
	if err != nil || after.ModTime() != before.ModTime() || readF(t, stock) != "{ \"~type\":\"idSnapMap\" }" {
		t.Fatal("vanilla changed")
	}
	var block struct {
		Mods []struct {
			Objects map[string]string
			Size    int
		}
	}
	var root earlierObject
	if json.Unmarshal([]byte(readF(t, selected)), &root) != nil || json.Unmarshal(root["snapmap-plus"], &block) != nil || len(block.Mods) != 1 || block.Mods[0].Size != 17 {
		t.Fatalf("unexpected selected row: %s", readF(t, selected))
	}
	var unit string
	for _, h := range block.Mods[0].Objects {
		unit = h
	}
	contents := archiveNames(t, companionPath(selected, 1))
	if len(contents) != 3 || !contents["room.json"] || !contents["units/"+unit+".json"] || !contents["files/0e9e6000b10994ecc327e98e5bb149860c3e5823d420fef5935ef5c311bb9068"] {
		t.Fatalf("selected archive: %v", contents)
	}
	repeat, finish, err := preparePlayerMigration("", data, "")
	defer finish()
	if err != nil || len(repeat.Changes) != 0 {
		t.Fatalf("repeat: %v %+v", err, repeat)
	}
}

func TestHistoryPartialPeersRetryAfterOriginalRetires(t *testing.T) {
	useEarlierUnitTool(t)
	data := t.TempDir()
	snap, _ := frozenTiny(t, data)
	canon := strings.ReplaceAll(readF(t, "testdata/earlier-tiny/bundle.canonical.json"), "alex.tiny", "alex.peer")
	peerHash := fmt.Sprintf("%x", sha256.Sum256([]byte(canon)))
	peer := tinyPin("mods/alex.peer/b")
	peer["id"], peer["bundle"] = pinJSON("alex.peer"), pinJSON(peerHash)
	mapFile := filepath.Join(data, "rawmaps", "mixed.json")
	first := earlierTestMap(tinyPin("mods/alex.tiny/a"), peer)
	writeF(t, mapFile, first)
	if err := convertEarlierData("", data); err != nil {
		t.Fatal(err)
	}
	retryDir := filepath.Join(data, "cache", "migration-units")
	keep, err := os.ReadDir(retryDir)
	if err != nil || len(keep) != 1 || readF(t, mapFile) != first || earlierPathExists(snap) || earlierPathExists(companionPath(mapFile, 0)) {
		t.Fatalf("incomplete peer dispositions: %v %v", keep, err)
	}
	if earlierPathExists(filepath.Join(data, "cache", "content", "units")) {
		t.Fatal("partial map leaked duplicate staged units")
	}
	contents := archiveNames(t, filepath.Join(retryDir, keep[0].Name()))
	if len(contents) != 3 || !contents["pin.json"] || !contents["files/0e9e6000b10994ecc327e98e5bb149860c3e5823d420fef5935ef5c311bb9068"] {
		t.Fatalf("retry stored unrelated content: %v", contents)
	}
	// Deleting the last map that needs this archive makes it safe to remove.
	orphan := t.TempDir()
	orphanFile := filepath.Join(orphan, "cache", "migration-units", keep[0].Name())
	writeF(t, orphanFile, readF(t, filepath.Join(retryDir, keep[0].Name())))
	unrelated := filepath.Join(filepath.Dir(orphanFile), "notes.zip")
	writeF(t, unrelated, "other content")
	if err := convertEarlierData("", orphan); err != nil || earlierPathExists(orphanFile) || readF(t, unrelated) != "other content" {
		t.Fatalf("retiring an orphan: %v", err)
	}
	next := filepath.Join(data, "mods", ".builds", "alex.peer@"+peerHash[:16])
	writeF(t, filepath.Join(next, "bundle.canonical.json"), canon)
	for _, name := range []string{"a.decl", "b.decl"} {
		rel := "assets/generated/decls/entitydef/mods/"
		writeF(t, filepath.Join(next, rel, "alex.peer", name), readF(t, filepath.Join("testdata/earlier-tiny", rel, "alex.tiny", name)))
	}
	if err := convertEarlierData("", data); err != nil {
		t.Fatal(err)
	}
	if readF(t, mapFile) == first || earlierPathExists(next) || earlierPathExists(retryDir) {
		t.Fatal("resolved peer did not convert/release recovery")
	}
	contents = archiveNames(t, companionPath(mapFile, 0))
	if len(contents) != 5 || !contents["mixed.json"] || !contents["files/097992204f8099b0f29fbd9d4da5f343b563fec09de1c1052819be9caadc2d0f"] {
		t.Fatalf("recovered map content: %v", contents)
	}
}

func TestHistoryRetainsEditedAndConcurrentInputs(t *testing.T) {
	for _, when := range []string{"before", "after preflight"} {
		t.Run(when, func(t *testing.T) {
			data := t.TempDir()
			snap, _ := frozenTiny(t, data)
			file := filepath.Join(snap, "assets/generated/decls/entitydef/mods/alex.tiny/a.decl")
			if when == "before" {
				writeF(t, file, "edited")
			}
			plan, done, err := preparePlayerMigration("", data, "")
			defer done()
			if err != nil {
				t.Fatal(err)
			}
			if when == "before" {
				if len(plan.Changes) != 0 {
					t.Fatal("planned to remove edited tree")
				}
			} else {
				writeF(t, file, "edited")
				if err = plan.commit(); err == nil {
					t.Fatal("accepted concurrent change")
				}
			}
			if readF(t, file) != "edited" || !isRegular(filepath.Join(snap, "bundle.json")) {
				t.Fatal("input changed")
			}
		})
	}
}

func TestHistoryArchiveVerifiesBlobBytesAgainstManifest(t *testing.T) {
	contents := []byte("expected")
	hash := fmt.Sprintf("%x", sha256.Sum256(contents))
	path := filepath.Join(t.TempDir(), hash)
	writeF(t, path, "different")
	var archive bytes.Buffer
	if err := writeUnitArchive(&archive, "map.json", []byte("{}"), map[string]string{"files/" + hash: path}); err == nil {
		t.Fatal("accepted a mismatched cached blob")
	}
}

func TestHistoryNativeCopyChecksBothDestinationsAndSlot(t *testing.T) {
	useEarlierUnitTool(t)
	data := t.TempDir()
	_, _ = frozenTiny(t, data)
	slot := filepath.Join(t.TempDir(), "slotA", "game.details")
	source := earlierTestMap(tinyPin("mods/alex.tiny/a"))
	hash := fmt.Sprintf("%x", sha256.Sum256([]byte(source)))
	plan, finish, err := preparePlayerMigration("", data, "")
	defer finish()
	if err != nil {
		t.Fatal(err)
	}
	pins := tinyResolver(t, data, nil)
	row, err := pins.row(tinyPin("mods/alex.tiny/a"))
	if err != nil {
		t.Fatal(err)
	}
	plan.pins = pins
	out := pinJSON(map[string]any{"~type": "idSnapMap", "snapmap-plus": map[string]any{"format": 2, "mods": []earlierObject{row}}})
	if err = plan.nativeCopy(slot, hash, "Pinned", out); err != nil {
		t.Fatal(err)
	}
	if err = plan.commit(); err != nil {
		t.Fatal(err)
	}
	assertKnown := func(slot string, want bool) {
		t.Helper()
		ok, err := plan.knownNativeCopy(slot, hash)
		if err != nil || ok != want {
			t.Fatalf("verified=%v want %v: %v", ok, want, err)
		}
	}
	assertKnown(slot, true)
	assertKnown(filepath.Join(filepath.Dir(slot), "other-slot.details"), false)
	output := filepath.Join(data, "rawmaps", "Updated saves", "Pinned.json")
	archive := companionPath(output, 0)
	old := readF(t, archive)
	writeF(t, archive, "changed")
	assertKnown(slot, false)
	writeF(t, archive, old)
	assertKnown(slot, true)
	writeF(t, output, "changed")
	assertKnown(slot, false)
}

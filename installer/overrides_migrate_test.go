package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const oldOverrideMarker = `{"schema":"snapmap-plus.override-package.v1","name":"Personal changes","version":1}`

func TestOverrideMigrationFormatsAndPreservation(t *testing.T) {
	data := t.TempDir()
	root := filepath.Join(data, "overrides")
	files := map[string]string{
		"group/old/package.json":                                  oldOverrideMarker,
		"group/old/assets/decls/snapeditorentitydef/fixture.decl": "{ edit = { value = 7; } }\r\n",
		"group/old/decls/entitydef/test.decl":                     "{ edit = { health = 10; } }",
		"group/old/images/icons/boss.bimage":                      "\x00\x01binary image",
		"group/old/shaders/generated/spirv/test.fspv":             "binary shader",
		"group/old/generated/model/test.bmodel":                   "binary model",
		"group/old/requirements/boss.requirements":                "# comment\r\ncvar\tg_useResourceBlackList\t0\r\ncvar\tg_useImageBlackList\t0\r\n",
		"group/old/strings/arbitrary-name.json":                   `{"#BOSS":"Boss"}`,
		"group/old/hud/weapons.json":                              `{"schema":"snapmap-plus.weapon-hud.v1","weapons":{"pistol":{"icon":"pistol"}}}`,
		"group/old/assets/decls/disabled.decl.backup":             "disabled content",
		"group/old/README.txt":                                    "keep author notes",
		"group/old/child/package.json":                            "{\n \"id\":\"child\", \"name\":\"Child\"\n}\n",
		"group/old/child/assets/opaque.bin":                       "unchanged child data",
		"current/package.json":                                    "{ \"id\": \"current\", \"name\": \"Current\", \"extra\": 2 }\n",
		"current/assets/generated/decls/entitydef/current.decl":   "{}",
	}
	for p, b := range files {
		writeF(t, filepath.Join(root, filepath.FromSlash(p)), b)
	}
	if err := os.MkdirAll(filepath.Join(root, "group/old/empty"), 0755); err != nil {
		t.Fatal(err)
	}
	oldHash, err := overrideSnapshot(filepath.Join(root, "group/old"))
	if err != nil {
		t.Fatal(err)
	}
	currentHash, _ := overrideSnapshot(filepath.Join(root, "current"))
	report, err := migrateOverrides(data, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if report.Converted != 1 || report.Unchanged != 1 || len(report.Rejected) != 0 || len(report.Backups) != 1 {
		t.Fatalf("report: %+v", report)
	}
	original := filepath.Join(report.Backups[0], "original/group/old")
	backupHash, err := overrideSnapshot(original)
	if err != nil || backupHash != oldHash {
		t.Fatalf("original not faithfully retained: %v", err)
	}
	for from, to := range map[string]string{"assets/decls/snapeditorentitydef/fixture.decl": "assets/generated/decls/snapeditorentitydef/fixture.decl", "decls/entitydef/test.decl": "assets/generated/decls/entitydef/test.decl", "images/icons/boss.bimage": "assets/generated/image/icons/boss.bimage", "shaders/generated/spirv/test.fspv": "assets/generated/spirv/test.fspv", "generated/model/test.bmodel": "assets/generated/model/test.bmodel", "assets/decls/disabled.decl.backup": "backups/assets/decls/disabled.decl.backup", "child/package.json": "child/package.json", "child/assets/opaque.bin": "child/assets/opaque.bin"} {
		if got := readF(t, filepath.Join(root, "group/old", filepath.FromSlash(to))); got != files["group/old/"+from] {
			t.Errorf("changed bytes from %s to %s", from, to)
		}
	}
	d, err := overrideJSON([]byte(readF(t, filepath.Join(root, "group/old/package.json"))))
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := d["schema"]; ok {
		t.Fatal("legacy schema retained")
	}
	if _, ok := d["version"]; ok {
		t.Fatal("legacy version retained")
	}
	if d["strings"].(map[string]any)["en"].(map[string]any)["#BOSS"] != "Boss" {
		t.Fatal("strings missing")
	}
	if d["requirements"].(map[string]any)["cvars"].(map[string]any)["g_useImageBlackList"] != json.Number("0") {
		t.Fatal("requirement missing")
	}
	if _, err := os.Stat(filepath.Join(root, "group/old/empty")); err != nil {
		t.Fatal("empty directory lost")
	}
	next, err := migrateOverrides(data, t.TempDir())
	if err != nil || next.Converted != 0 || next.Unchanged != 2 {
		t.Fatalf("not idempotent: %+v %v", next, err)
	}
	after, _ := overrideSnapshot(filepath.Join(root, "current"))
	if after != currentHash {
		t.Fatal("current package rewritten")
	}
}

func TestOverrideMigrationLooseAndUnicodeLongPaths(t *testing.T) {
	data := t.TempDir()
	root := filepath.Join(data, "overrides")
	long := strings.Repeat("subfolder/", 35) + "resource.decl"
	writeF(t, filepath.Join(root, "generated/decls/entitydef", filepath.FromSlash(long)), "{ edit = {} }")
	writeF(t, filepath.Join(root, "images/icons/fixture.bimage"), "image")
	writeF(t, filepath.Join(root, "requirements/legacy.requirements"), "cvar\tg_useImageBlackList\t0\n")
	writeF(t, filepath.Join(root, "Grüppé/角色/decls/entitydef/test.decl"), "{}")
	writeF(t, filepath.Join(data, "strings/strids.json"), `{"global":"untouched"}`)
	r, err := migrateOverrides(data, t.TempDir())
	if err != nil || r.Converted != 2 || len(r.Rejected) != 0 {
		t.Fatalf("%+v %v", r, err)
	}
	if readF(t, filepath.Join(root, "legacy-overrides/assets/generated/decls/entitydef", filepath.FromSlash(long))) != "{ edit = {} }" {
		t.Fatal("long path lost")
	}
	if readF(t, filepath.Join(root, "Grüppé/角色/assets/generated/decls/entitydef/test.decl")) != "{}" {
		t.Fatal("Unicode package lost")
	}
	if readF(t, filepath.Join(data, "strings/strids.json")) != `{"global":"untouched"}` {
		t.Fatal("global strings changed")
	}
	next, err := migrateOverrides(data, t.TempDir())
	if err != nil || next.Converted != 0 {
		t.Fatalf("second conversion: %+v %v", next, err)
	}
}

func TestOverrideMigrationRejectsOnlyBadOuterUnit(t *testing.T) {
	for _, problem := range []struct{ name, path, body string }{
		{"invalid JSON", "package.json", "{"},
		{"duplicate JSON", "package.json", `{"name":"a","NAME":"b"}`},
		{"nested invalid", "child/package.json", "{"},
		{"collision", "assets/generated/decls/entitydef/test.decl", "different"},
		{"unsupported policy", "requirements/test.requirements", "cvar\tg_godmode\t1"},
		{"invalid requirement object", "package.json", `{"id":"bad","name":"Bad","requirements":{"cvars":{"g_useImageBlackList":{}}}}`},
		{"conflicting strings", "strings/b.json", `{"k":"different"}`},
	} {
		t.Run(problem.name, func(t *testing.T) {
			data := t.TempDir()
			root := filepath.Join(data, "overrides")
			writeF(t, filepath.Join(root, "healthy/package.json"), oldOverrideMarker)
			writeF(t, filepath.Join(root, "healthy/decls/entitydef/test.decl"), "{}")
			writeF(t, filepath.Join(root, "broken/package.json"), oldOverrideMarker)
			writeF(t, filepath.Join(root, "broken/decls/entitydef/test.decl"), "{}")
			writeF(t, filepath.Join(root, "broken/strings/a.json"), `{"k":"original"}`)
			writeF(t, filepath.Join(root, "broken", problem.path), problem.body)
			before, _ := overrideSnapshot(filepath.Join(root, "broken"))
			r, err := migrateOverrides(data, t.TempDir())
			if err != nil || r.Converted != 1 || len(r.Rejected) != 1 {
				t.Fatalf("%+v %v", r, err)
			}
			after, _ := overrideSnapshot(filepath.Join(root, "broken"))
			if before != after {
				t.Fatal("rejected original changed")
			}
		})
	}
}

func TestOverrideMigrationRecovery(t *testing.T) {
	for _, point := range []string{"staged", "moved:decls", "moved:images", "published"} {
		t.Run(point, func(t *testing.T) {
			data := t.TempDir()
			root := filepath.Join(data, "overrides")
			backups := filepath.Join(data, "override-backups")
			os.MkdirAll(backups, 0755)
			writeF(t, filepath.Join(root, "decls/entitydef/a.decl"), "{}")
			writeF(t, filepath.Join(root, "images/a.bimage"), "original")
			before, _ := overrideSnapshot(root)
			_, transaction, err := migrateOverrideUnit(root, backups, overrideUnit{[]string{"decls", "images"}, "legacy-overrides", true}, &overrideCatalog{}, func(at string) error {
				if at == point {
					return errors.New("simulated interruption")
				}
				return nil
			})
			if err == nil || transaction == "" {
				t.Fatal("interruption not reached")
			}
			if err := recoverOverrideMigration(root, transaction); err != nil {
				t.Fatal(err)
			}
			if point == "published" {
				if readF(t, filepath.Join(root, "legacy-overrides/assets/generated/image/a.bimage")) != "original" {
					t.Fatal("publication incomplete")
				}
			} else {
				after, _ := overrideSnapshot(root)
				if after != before {
					t.Fatal("rollback did not restore originals")
				}
			}
			if err := recoverOverrideMigration(root, transaction); err != nil {
				t.Fatal("recovery is not idempotent", err)
			}
			r, err := migrateOverrides(data, t.TempDir())
			if err != nil || len(r.Rejected) != 0 {
				t.Fatalf("retry: %+v %v", r, err)
			}
		})
	}
}

func TestOverrideMigrationRecoveryDoesNotOverwrite(t *testing.T) {
	data := t.TempDir()
	root := filepath.Join(data, "overrides")
	backups := filepath.Join(data, "override-backups")
	os.MkdirAll(backups, 0755)
	writeF(t, filepath.Join(root, "old/package.json"), oldOverrideMarker)
	_, transaction, _ := migrateOverrideUnit(root, backups, overrideUnit{[]string{"old"}, "old", false}, &overrideCatalog{}, func(at string) error {
		if strings.HasPrefix(at, "moved:") {
			return errors.New("interrupt")
		}
		return nil
	})
	writeF(t, filepath.Join(root, "old/package.json"), "new external edit")
	if err := recoverOverrideMigration(root, transaction); err == nil {
		t.Fatal("overwrote recovery collision")
	}
	if readF(t, filepath.Join(root, "old/package.json")) != "new external edit" || readF(t, filepath.Join(transaction, "original/old/package.json")) != oldOverrideMarker {
		t.Fatal("recovery lost a copy")
	}
}

func syntheticOverrideCatalog(t *testing.T, doom string, rows []overrideRecord, payload string) {
	t.Helper()
	base := filepath.Join(doom, "base")
	os.MkdirAll(base, 0755)
	for _, stem := range []string{"gameresources", "snap_gameresources"} {
		b := make([]byte, 36)
		copy(b, []byte{5, 'S', 'E', 'R'})
		if stem == "gameresources" {
			binary.BigEndian.PutUint32(b[32:], uint32(len(rows)))
			for i, r := range rows {
				ordinal := make([]byte, 4)
				binary.BigEndian.PutUint32(ordinal, uint32(i))
				b = append(b, ordinal...)
				for _, s := range []string{r.kind, r.name, r.path} {
					size := make([]byte, 4)
					binary.LittleEndian.PutUint32(size, uint32(len(s)))
					b = append(b, size...)
					b = append(b, s...)
				}
				row := make([]byte, 21)
				binary.BigEndian.PutUint64(row, r.offset)
				binary.BigEndian.PutUint32(row[8:], r.size)
				binary.BigEndian.PutUint32(row[12:], r.stored)
				b = append(b, row...)
			}
		}
		binary.BigEndian.PutUint32(b[4:], uint32(len(b)-32))
		if err := os.WriteFile(filepath.Join(base, stem+".pindex"), b, 0644); err != nil {
			t.Fatal(err)
		}
		writeF(t, filepath.Join(base, stem+".resources"), payload)
	}
}

func TestOverrideMigrationManifestPermutationsAndIncludes(t *testing.T) {
	data, doom := t.TempDir(), t.TempDir()
	root := filepath.Join(data, "overrides")
	var compressed bytes.Buffer
	w, _ := flate.NewWriter(&compressed, flate.DefaultCompression)
	w.Write([]byte("compressed fixture"))
	w.Close()
	payload := "oneTWO" + compressed.String()
	rows := []overrideRecord{
		{kind: "shader", name: "same", path: "generated/spirv/a.fspv", size: 3, stored: 3},
		{kind: "shader", name: "same", path: "generated/spirv/b.fspv", offset: 3, size: 3, stored: 3},
		{kind: "data", name: "packed", path: "generated/data/packed.bin", offset: 6, size: 18, stored: uint32(compressed.Len())},
		{kind: "include", name: "inc", path: "renderprogs/includes/shared.inc", size: 3, stored: 3},
	}
	syntheticOverrideCatalog(t, doom, rows, payload)
	writeF(t, filepath.Join(root, "old/package.json"), oldOverrideMarker)
	writeF(t, filepath.Join(root, "old/resources/boss.manifest"), "shader\tsame\tgenerated/spirv/a.fspv\nshader\tsame\tgenerated/spirv/b.fspv\ndata\tpacked\tgenerated/data/packed.bin\n")
	writeF(t, filepath.Join(root, "old/shader_includes/includes/shared.inc"), "authored include")
	r, err := migrateOverrides(data, doom)
	if err != nil || r.Converted != 1 || len(r.Rejected) > 0 {
		t.Fatalf("%+v %v", r, err)
	}
	for path, want := range map[string]string{"generated/spirv/a.fspv": "one", "generated/spirv/b.fspv": "TWO", "generated/data/packed.bin": "compressed fixture", "renderprogs/includes/shared.inc": "authored include"} {
		if readF(t, filepath.Join(root, "old/assets", path)) != want {
			t.Fatalf("incorrect payload for %s", path)
		}
	}
}

func TestOverrideCatalogRejectsAmbiguousAndOutOfBounds(t *testing.T) {
	doom := t.TempDir()
	rows := []overrideRecord{{kind: "data", name: "a", path: "same", size: 1, stored: 1}, {kind: "data", name: "a", path: "same", offset: 1, size: 1, stored: 1}}
	syntheticOverrideCatalog(t, doom, rows, "ab")
	c := overrideCatalog{base: filepath.Join(doom, "base")}
	if _, err := c.resolve("data", "a", "same"); err == nil {
		t.Fatal("ambiguous row accepted")
	}
	if _, err := c.resolve("data", "missing", "same"); err == nil {
		t.Fatal("missing row accepted")
	}
	row := c.rows[0]
	row.offset = 99
	if _, err := row.read(); err == nil {
		t.Fatal("out-of-bounds payload accepted")
	}
}

func TestOverrideMigrationRetainsMalformedDeclarationBytes(t *testing.T) {
	data := t.TempDir()
	path := filepath.Join(data, "overrides/old")
	writeF(t, filepath.Join(path, "package.json"), oldOverrideMarker)
	writeF(t, filepath.Join(path, "decls/entitydef/bad.decl"), "{} }")
	r, err := migrateOverrides(data, t.TempDir())
	if err != nil || r.Converted != 1 {
		t.Fatalf("%+v %v", r, err)
	}
	if readF(t, filepath.Join(path, "assets/generated/decls/entitydef/bad.decl")) != "{} }" {
		t.Fatal("converter silently repaired authored declaration")
	}
	// Native compiler tests establish rejection; conversion must not invent edits.
}

package main

import (
	"errors"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

const legacyMarker = `{"schema":"snapmap-plus.override-package.v1","name":"my-overrides","version":"1.0.0","description":"Your own overrides."}`

type overrideLibrary struct {
	t    *testing.T
	data string
	root string
	doom string
}

func newOverrideLibrary(t *testing.T, records ...fakeRecord) *overrideLibrary {
	t.Helper()
	lib := &overrideLibrary{t: t, data: t.TempDir()}
	lib.root = filepath.Join(lib.data, "overrides")
	mkdirAll(t, lib.root)
	if records != nil {
		lib.doom = t.TempDir()
		writeFakeCatalog(t, lib.doom, records)
	}
	return lib
}

func (l *overrideLibrary) path(rel string) string {
	return filepath.Join(l.root, filepath.FromSlash(rel))
}

func (l *overrideLibrary) write(files map[string]string) {
	l.t.Helper()
	for rel, body := range files {
		if strings.HasSuffix(rel, "/") {
			mkdirAll(l.t, l.path(rel))
			continue
		}
		writeF(l.t, l.path(rel), body)
	}
}

func (l *overrideLibrary) read(rel string) string {
	l.t.Helper()
	return readF(l.t, l.path(rel))
}

func (l *overrideLibrary) missing(rel string) bool {
	_, err := os.Lstat(l.path(rel))
	return os.IsNotExist(err)
}

func (l *overrideLibrary) migrate() overrideMigration {
	l.t.Helper()
	report, err := migrateOverrides(l.data, l.doom)
	if err != nil {
		l.t.Fatalf("migration failed: %v (%+v)", err, report)
	}
	return report
}

// tree lists every file with its bytes and every folder, relative to base.
func treeOf(t *testing.T, base string) map[string]string {
	t.Helper()
	out := map[string]string{}
	err := filepath.WalkDir(base, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(base, path)
		rel = filepath.ToSlash(rel)
		if rel == "." {
			return nil
		}
		if d.IsDir() {
			out[rel+"/"] = ""
			return nil
		}
		b, err := os.ReadFile(path)
		out[rel] = string(b)
		return err
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func sameTree(t *testing.T, got, want map[string]string, context string) {
	t.Helper()
	var problems []string
	for k, v := range want {
		if g, ok := got[k]; !ok {
			problems = append(problems, "missing "+k)
		} else if g != v {
			problems = append(problems, "changed "+k)
		}
	}
	for k := range got {
		if _, ok := want[k]; !ok {
			problems = append(problems, "extra "+k)
		}
	}
	sort.Strings(problems)
	if len(problems) > 0 {
		t.Fatalf("%s: %s", context, strings.Join(problems, "; "))
	}
}

func transactions(t *testing.T, data string) []string {
	t.Helper()
	entries, err := os.ReadDir(filepath.Join(data, "override-backups"))
	if err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	var out []string
	for _, e := range entries {
		out = append(out, filepath.Join(data, "override-backups", e.Name()))
	}
	return out
}

func descriptorOf(t *testing.T, body string) jsonValue {
	t.Helper()
	if err := validateDescriptor([]byte(body)); err != nil {
		t.Fatalf("converted descriptor is not loadable: %v\n%s", err, body)
	}
	v, err := parseNativeObject([]byte(body), descriptorDepth)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestOverrideMigrationLeavesCurrentPackagesUntouched(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{
		"boss-demons/package.json":                                  `{"id":"tests.boss","name":"Boss"}`,
		"boss-demons/assets/generated/decls/entitydef/boss.decl":    "{ edit = { health = 12000; } }",
		"boss-demons/cyberdemon/package.json":                       `{"id":"tests.boss.cyber","name":"Cyber","requirements":{"cvars":{"g_useImageBlackList":0}}}`,
		"boss-demons/cyberdemon/assets/generated/image/icon.bimage": "\x00\x01",
		"boss-demons/strings/draft.json":                            `{"notes":{"nested":true}}`,
		"boss-demons/empty/":                                        "",
		"boss-demons/assets/package.json":                           "engine data named package.json",
		"boss-demons/assets/strings/en.json":                        `{"boss_name":"Engine asset"}`,
		"boss-demons/assets/hud/weapons.json":                       `{"schema":"snapmap-plus.weapon-hud.v1","weapons":{}}`,
		"boss-demons/assets/requirements/boss.requirements":         "cvar\tg_useImageBlackList\t0\n",
		"boss-demons/assets/resources/boss.manifest":                "image\t_stock\t\n",
		"boss-demons/assets/decls/entitydef/engine.decl":            "{ engine path }",
		"boss-demons/assets/smpkg.digest":                           "0123456789abcdef",
		"generated/package.json":                                    `{"id":"tests.generated","name":"Named generated"}`,
		"generated/assets/generated/decls/entitydef/g.decl":         "{}",
		"assets/package.json":                                       `{"id":"tests.assets","name":"Named assets"}`,
		"group/lifts/package.json":                                  `{"id":"tests.lifts","name":"Lifts"}`,
	})
	before := treeOf(t, lib.root)
	report := lib.migrate()
	if len(report.Converted) != 0 || len(report.Rejected) != 0 || report.Unchanged != 4 {
		t.Fatalf("report: %+v", report)
	}
	sameTree(t, treeOf(t, lib.root), before, "current library")
	if len(transactions(t, lib.data)) != 0 {
		t.Fatal("current packages were staged or backed up")
	}
}

func TestOverrideMigrationConvertsMarkedLegacyPackage(t *testing.T) {
	lib := newOverrideLibrary(t,
		fakeRecord{kind: "entityDef", name: "campaign/thing", path: "generated/decls/entitydef/campaign/thing.decl", body: "{ campaign }", mode: "sync"},
		fakeRecord{kind: "image", name: "_stock", body: "stock image", mode: "final"},
		fakeRecord{kind: "image", name: "_stock", body: "stock image", snap: true},
		fakeRecord{kind: "model", name: "shared", path: "models/shared.bmodel", body: "campaign model"},
		fakeRecord{kind: "model", name: "shared", path: "models/shared.bmodel", body: "snapmap model", snap: true},
	)
	files := map[string]string{
		"old/package.json": `{"schema":"snapmap-plus.override-package.v1","name":"Old package","version":"1.0.0","priority":5,` +
			`"description":"keep me","contents":{"decls/entitydef/test.decl":"abc"},"restart_required":true,"author":{"name":"someone"}}`,
		"old/decls/entitydef/test.decl":                              "{ edit = { value = 7; } }\r\n",
		"old/decls/entitydef/test.decl.backup":                       "disabled",
		"old/decls/readme.txt":                                       "notes",
		"old/decls/emptytype/":                                       "",
		"old/images/icons/boss.bimage":                               "\x00image",
		"old/shaders/generated/spirv/test.fspv":                      "spirv",
		"old/shaders/generated/renderprogs/permutations/test_1.decl": "{ permutation }",
		"old/shaders/notes.txt":                                      "shader notes",
		"old/resources/boss.manifest":                                "# records\r\nentityDef\tcampaign/thing\tgenerated/decls/entitydef/campaign/thing.decl\r\nimage\t_stock\t\nmodel\tshared\tmodels/shared.bmodel\n",
		"old/resources/readme.md":                                    "manifest notes",
		"old/requirements/boss.requirements":                         "# requirements\r\ncvar\tg_useResourceBlackList\t0\r\ncvar\tg_useImageBlackList\t0\r\n",
		"old/strings/boss.json":                                      "\xef\xbb\xbf{\"boss_name\":\"Boss\",\"boss_desc\":\"D\\u00e9mon\"}",
		"old/strings/more.json":                                      `{"BOSS_NAME":"Boss"}`,
		"old/hud/weapons.json":                                       `{"schema":"snapmap-plus.weapon-hud.v1","weapons":{"weapon/zion/player/sp/pistol":{"ammo_display":"weapon"}}}`,
		"old/smpkg.digest":                                           "0123456789abcdef",
		"old/README.txt":                                             "author notes",
		"old/empty/":                                                 "",
		"old/child/package.json":                                     `{"id":"tests.child","name":"Child"}`,
		"old/child/assets/opaque.bin":                                "unchanged child",
	}
	lib.write(files)
	original := treeOf(t, lib.path("old"))
	report := lib.migrate()
	if len(report.Converted) != 1 || len(report.Rejected) != 0 {
		t.Fatalf("report: %+v", report)
	}
	got := treeOf(t, lib.path("old"))
	descriptor := got["package.json"]
	delete(got, "package.json")
	sameTree(t, got, map[string]string{
		"assets/":                                               "",
		"assets/generated/":                                     "",
		"assets/generated/decls/":                               "",
		"assets/generated/decls/entitydef/":                     "",
		"assets/generated/decls/entitydef/test.decl":            files["old/decls/entitydef/test.decl"],
		"assets/generated/decls/entitydef/campaign/":            "",
		"assets/generated/decls/entitydef/campaign/thing.decl":  "{ campaign }",
		"assets/generated/decls/emptytype/":                     "",
		"assets/generated/image/":                               "",
		"assets/generated/image/icons/":                         "",
		"assets/generated/image/icons/boss.bimage":              "\x00image",
		"assets/generated/spirv/":                               "",
		"assets/generated/spirv/test.fspv":                      "spirv",
		"assets/generated/renderprogs/":                         "",
		"assets/generated/renderprogs/permutations/":            "",
		"assets/generated/renderprogs/permutations/test_1.decl": "{ permutation }",
		"assets/models/":                                        "",
		"assets/models/shared.bmodel":                           "campaign model",
		"decls/":                                                "",
		"decls/entitydef/":                                      "",
		"decls/entitydef/test.decl.backup":                      "disabled",
		"decls/readme.txt":                                      "notes",
		"shaders/":                                              "",
		"shaders/notes.txt":                                     "shader notes",
		"resources/":                                            "",
		"resources/readme.md":                                   "manifest notes",
		"README.txt":                                            "author notes",
		"empty/":                                                "",
		"child/":                                                "",
		"child/package.json":                                    files["old/child/package.json"],
		"child/assets/":                                         "",
		"child/assets/opaque.bin":                               "unchanged child",
	}, "converted package")
	d := descriptorOf(t, descriptor)
	if d.member("id").text != "local.old" || d.member("name").text != "Old package" ||
		d.member("description").text != "keep me" || d.member("author") == nil {
		t.Fatalf("identity or metadata lost:\n%s", descriptor)
	}
	for _, retired := range retiredDescriptorFields {
		if d.member(retired) != nil {
			t.Fatalf("retired field %s kept:\n%s", retired, descriptor)
		}
	}
	cvars := d.member("requirements").member("cvars")
	en := d.member("strings").member("en")
	if len(cvars.members) != 2 || len(en.members) != 2 || en.member("boss_desc").text != "Démon" ||
		d.member("hud").member("weapons").member("weapon/zion/player/sp/pistol") == nil {
		t.Fatalf("policy not folded:\n%s", descriptor)
	}
	backup := filepath.Join(report.Converted[0].Backup, "original", "old")
	sameTree(t, treeOf(t, backup), original, "retained original")
	if !strings.Contains(strings.Join(report.Notes, "\n"), "retired priority 5") {
		t.Fatalf("priority change not reported: %v", report.Notes)
	}
	again := lib.migrate()
	if len(again.Converted) != 0 || again.Unchanged != 1 {
		t.Fatalf("second run changed the library: %+v", again)
	}
}

func TestOverrideMigrationConvertsPartiallyMovedAssetsLayout(t *testing.T) {
	lib := newOverrideLibrary(t,
		fakeRecord{kind: "renderProg", name: "prog", path: "decls/renderprogs/includes/vertex.inc", body: "include"})
	malformed := "{\n\tedit = {\n\t\tvalue = 1;\n\t}\n}\n}\n"
	lib.write(map[string]string{
		"my-overrides/package.json": legacyMarker,
		"my-overrides/assets/decls/snapeditorentitydef/non_editor/volume/hazard/snap_dot_fire.decl": malformed,
		"my-overrides/assets/decls/snapeditorentitydef/volume/blocking.decl.backup":                 "disabled blocking",
		"my-overrides/assets/decls/renderprogs/includes/custom.inc":                                 "engine include",
		"my-overrides/assets/generated/decls/snapeditorentitydef/already.decl":                      "{}",
	})
	report := lib.migrate()
	if len(report.Converted) != 1 {
		t.Fatalf("report: %+v", report)
	}
	got := treeOf(t, lib.path("my-overrides"))
	if got["assets/generated/decls/snapeditorentitydef/non_editor/volume/hazard/snap_dot_fire.decl"] != malformed {
		t.Fatal("authored declaration bytes changed; malformed content must be kept for diagnosis")
	}
	if got["decls/snapeditorentitydef/volume/blocking.decl.backup"] != "disabled blocking" {
		t.Fatal("disabled file was not kept outside assets")
	}
	if got["assets/decls/renderprogs/includes/custom.inc"] != "engine include" ||
		got["assets/generated/decls/snapeditorentitydef/already.decl"] != "{}" {
		t.Fatal("engine content in the current layout was moved")
	}
	for rel := range got {
		if strings.HasPrefix(rel, "assets/decls/snapeditorentitydef") {
			t.Fatalf("wrapped legacy path remains active: %s", rel)
		}
	}
	d := descriptorOf(t, got["package.json"])
	if d.member("id").text != "local.my-overrides" || d.member("description").text != "Your own overrides." {
		t.Fatalf("descriptor:\n%s", got["package.json"])
	}
}

func TestOverrideMigrationConvertsLooseLayouts(t *testing.T) {
	lib := newOverrideLibrary(t,
		fakeRecord{kind: "renderProg", name: "prog", path: "decls/renderprogs/includes/vertex.inc", body: "stock include"},
		fakeRecord{kind: "mapEntities", name: "maps/game/sp/intro", path: "maps/game/sp/intro.entities", body: "entities"},
		fakeRecord{kind: "image", name: "icon", path: "generated/image/icon.bimage", body: "campaign icon"},
	)
	lib.write(map[string]string{
		"generated/decls/entitydef/loose.decl":            "{ loose }",
		"generated/decls/notes.txt":                       "notes",
		"generated/image/icon.bimage":                     "authored icon",
		"generated/spirv/a.fspv":                          "LOOSE",
		"generated/shaders/generated/spirv/a.fspv":        "PACKAGE",
		"generated/shaders/generated/spirv/b.fspv":        "B",
		"generated/requirements/legacy.requirements":      "cvar\tg_useImageBlackList\t0\n",
		"generated/resources/icon.manifest":               "image\ticon\tgenerated/image/icon.bimage\n",
		"generated/includes/x.inc":                        "never served by exact path",
		"shader_includes/renderprogs/includes/vertex.inc": "authored include",
		"shader_includes/unknown/new.inc":                 "no installed alias",
		"assets/generated/decls/entitydef/hybrid.decl":    "{ hybrid }",
		"maps/game/sp/intro.entities":                     "authored entities",
		"maps/game/sp/new.bin":                            "new map data",
		"demons/imp/package.json":                         `{"id":"tests.imp","name":"Imp"}`,
		"demons/readme.txt":                               "group notes",
		"legacy-overrides/package.json":                   `{"id":"tests.existing","name":"Existing"}`,
		"readme.txt":                                      "root notes",
	})
	report := lib.migrate()
	if len(report.Converted) != 1 || report.Converted[0].Package != "legacy-overrides-2" || len(report.Rejected) != 0 {
		t.Fatalf("report: %+v", report)
	}
	got := treeOf(t, lib.path("legacy-overrides-2"))
	for rel, want := range map[string]string{
		"assets/generated/decls/entitydef/loose.decl":  "{ loose }",
		"generated/decls/notes.txt":                    "notes",
		"assets/generated/image/icon.bimage":           "authored icon",
		"assets/generated/spirv/a.fspv":                "LOOSE",
		"generated/shaders/generated/spirv/a.fspv":     "PACKAGE",
		"assets/generated/spirv/b.fspv":                "B",
		"generated/includes/x.inc":                     "never served by exact path",
		"assets/decls/renderprogs/includes/vertex.inc": "authored include",
		"shader_includes/unknown/new.inc":              "no installed alias",
		"assets/generated/decls/entitydef/hybrid.decl": "{ hybrid }",
		"assets/maps/game/sp/intro.entities":           "authored entities",
		"assets/maps/game/sp/new.bin":                  "new map data",
	} {
		if got[rel] != want {
			t.Errorf("%s = %q, want %q", rel, got[rel], want)
		}
	}
	d := descriptorOf(t, got["package.json"])
	if d.member("id").text != "local.legacy-overrides-2" || d.member("name").text != "Legacy overrides" ||
		d.member("requirements").member("cvars").member("g_useImageBlackList") == nil {
		t.Fatalf("descriptor:\n%s", got["package.json"])
	}
	for _, rel := range []string{"generated", "shader_includes", "assets", "maps"} {
		if !lib.missing(rel) {
			t.Errorf("loose source %s was not moved to the backup", rel)
		}
	}
	if lib.read("demons/readme.txt") != "group notes" || lib.read("readme.txt") != "root notes" ||
		lib.read("legacy-overrides/package.json") != `{"id":"tests.existing","name":"Existing"}` {
		t.Fatal("group or unrelated files changed")
	}
	notes := strings.Join(report.Notes, "\n")
	if !strings.Contains(notes, "matches no installed shader include") || !strings.Contains(notes, "was shadowed by") {
		t.Fatalf("inactive content not reported: %s", notes)
	}
	if again := lib.migrate(); len(again.Converted) != 0 {
		t.Fatalf("second run converted again: %+v", again)
	}
}

func TestOverrideMigrationConvertsUnmarkedPackageInsideGroup(t *testing.T) {
	lib := newOverrideLibrary(t, fakeRecord{kind: "image", name: "unrelated", path: "textures/x.bimage", body: "x"})
	lib.write(map[string]string{
		"group/current/package.json":            `{"id":"tests.current","name":"Current"}`,
		"group/unmarked/decls/entitydef/u.decl": "{ u }",
		"group/unmarked/strings/names.json":     `{"u_name":"U"}`,
		"group/notes.txt":                       "group notes",
	})
	report := lib.migrate()
	if len(report.Converted) != 1 || report.Converted[0].Package != "group/unmarked" || report.Unchanged != 1 {
		t.Fatalf("report: %+v", report)
	}
	d := descriptorOf(t, lib.read("group/unmarked/package.json"))
	if d.member("id").text != "local.unmarked" || d.member("name").text != "unmarked" ||
		lib.read("group/unmarked/assets/generated/decls/entitydef/u.decl") != "{ u }" {
		t.Fatal("unmarked package was not normalized in place")
	}
	if lib.read("group/notes.txt") != "group notes" || lib.read("group/current/package.json") != `{"id":"tests.current","name":"Current"}` {
		t.Fatal("group content changed")
	}
}

func TestOverrideMigrationKeepsOuterBoundariesAndNestedComponents(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{
		"outer/package.json":                 `{"id":"tests.outer","name":"Outer"}`,
		"outer/assets/a.bin":                 "outer data",
		"outer/child/package.json":           `{"name":"Child"}`,
		"outer/child/decls/entitydef/c.decl": "{ child }",
		"legacy/package.json":                "",
		"legacy/decls/entitydef/l.decl":      "{ legacy }",
		"legacy/modern/package.json":         `{"id":"tests.modern","name":"Modern"}`,
		"legacy/modern/assets/m.bin":         "modern data",
		"legacy/modern/docs/decls/x.decl":    "modern auxiliary data stays",
	})
	outerDescriptor, _ := os.Stat(lib.path("outer/package.json"))
	report := lib.migrate()
	if len(report.Converted) != 2 {
		t.Fatalf("report: %+v", report)
	}
	after, _ := os.Stat(lib.path("outer/package.json"))
	if !os.SameFile(outerDescriptor, after) || lib.read("outer/assets/a.bin") != "outer data" {
		t.Fatal("unchanged outer component was replaced")
	}
	child := descriptorOf(t, lib.read("outer/child/package.json"))
	if child.member("id").text != "local.outer.child" || lib.read("outer/child/assets/generated/decls/entitydef/c.decl") != "{ child }" {
		t.Fatal("legacy nested component was not converted in place")
	}
	legacy := descriptorOf(t, lib.read("legacy/package.json"))
	if legacy.member("id").text != "local.legacy" || legacy.member("name").text != "legacy" ||
		lib.read("legacy/modern/package.json") != `{"id":"tests.modern","name":"Modern"}` ||
		lib.read("legacy/modern/docs/decls/x.decl") != "modern auxiliary data stays" {
		t.Fatal("current nested component inside a converted package changed")
	}
}

func TestOverrideMigrationRejectsOnlyTheBadPackage(t *testing.T) {
	for _, problem := range []struct{ name, path, body string }{
		{"invalid JSON", "package.json", "{"},
		{"duplicate key", "package.json", `{"name":"a","name":"b"}`},
		{"invalid id", "package.json", `{"id":"Bad Id","name":"Bad"}`},
		{"current descriptor with bad policy", "package.json", `{"id":"broken","name":"Broken","requirements":{"cvars":{"g_godmode":1}}}`},
		{"package marker inside a legacy namespace", "decls/sub/package.json", "{}"},
		{"different bytes for one engine path", "assets/generated/decls/entitydef/b.decl", "different"},
		{"invalid declaration path", "assets/generated/decls/notype.decl", "{}"},
		{"unsupported requirement", "requirements/bad.requirements", "cvar\tg_godmode\t1\n"},
		{"requirement with padding", "requirements/bad.requirements", "cvar\tg_useImageBlackList\t0 \n"},
		{"conflicting strings", "strings/z.json", `{"K":"two"}`},
		{"strings that are not text", "strings/bad.json", `{"k":5}`},
		{"invalid HUD rule", "hud/weapons.json", `{"schema":"snapmap-plus.weapon-hud.v1","weapons":{"Pistol":{"ammo_display":"weapon"}}}`},
		{"unresolved manifest", "resources/bad.manifest", "image\tmissing\tgenerated/image/missing.bimage\n"},
		{"manifest claims one path twice", "resources/bad.manifest", "image\ta\tgenerated/image/x.bimage\nimage\tb\tgenerated/image/x.bimage\n"},
	} {
		t.Run(problem.name, func(t *testing.T) {
			lib := newOverrideLibrary(t,
				fakeRecord{kind: "image", name: "a", path: "generated/image/x.bimage", body: "a"},
				fakeRecord{kind: "image", name: "b", path: "generated/image/x.bimage", body: "b"})
			lib.write(map[string]string{
				"healthy/package.json":           legacyMarker,
				"healthy/decls/entitydef/h.decl": "{}",
				"broken/package.json":            legacyMarker,
				"broken/decls/entitydef/b.decl":  "{}",
				"broken/strings/a.json":          `{"k":"one"}`,
			})
			lib.write(map[string]string{"broken/" + problem.path: problem.body})
			before := treeOf(t, lib.path("broken"))
			report := lib.migrate()
			if len(report.Converted) != 1 || report.Converted[0].Package != "healthy" || len(report.Rejected) != 1 ||
				!strings.Contains(report.Rejected[0], "broken") {
				t.Fatalf("report: %+v", report)
			}
			sameTree(t, treeOf(t, lib.path("broken")), before, "rejected package")
		})
	}
}

// crashAt stops a migration at a transaction boundary, like process death.
func crashAt(t *testing.T, lib *overrideLibrary, point string) (reached bool) {
	t.Helper()
	overrideMigrationCheckpoint = func(at string) {
		if at == point {
			panic(errMigrationCrash)
		}
	}
	defer func() {
		overrideMigrationCheckpoint = nil
		if r := recover(); r != nil {
			if r != errMigrationCrash {
				panic(r)
			}
			reached = true
		}
	}()
	migrateOverrides(lib.data, lib.doom)
	return false
}

var errMigrationCrash = errors.New("simulated crash")

func TestOverrideMigrationRecoversFromEveryBoundary(t *testing.T) {
	files := map[string]string{
		"generated/decls/entitydef/a.decl": "{ a }",
		"maps/game/x.entities":             "entities",
		"maps/empty/":                      "",
	}
	for _, point := range []string{"journaled", "moved:generated", "moved:maps", "verified", "published:legacy-overrides"} {
		t.Run(point, func(t *testing.T) {
			lib := newOverrideLibrary(t, fakeRecord{kind: "mapEntities", name: "x", path: "maps/game/x.entities", body: "stock"})
			lib.write(files)
			original := treeOf(t, lib.root)
			if !crashAt(t, lib, point) {
				t.Fatal("checkpoint not reached")
			}
			report := lib.migrate()
			if len(report.Rejected) != 0 {
				t.Fatalf("report after recovery: %+v", report)
			}
			got := treeOf(t, lib.root)
			if got["legacy-overrides/assets/generated/decls/entitydef/a.decl"] != "{ a }" ||
				got["legacy-overrides/assets/maps/game/x.entities"] != "entities" {
				t.Fatalf("conversion incomplete after recovery: %v", got)
			}
			if _, ok := got["legacy-overrides/assets/maps/empty/"]; !ok {
				t.Fatal("empty folder lost")
			}
			var originals int
			for _, transaction := range transactions(t, lib.data) {
				if !exists(filepath.Join(transaction, "complete.json")) || exists(filepath.Join(transaction, "stage")) {
					t.Fatalf("unfinished transaction left behind: %s", transaction)
				}
				if exists(filepath.Join(transaction, "original")) {
					originals++
					sameTree(t, treeOf(t, filepath.Join(transaction, "original")), original, "backup")
				}
			}
			if originals != 1 {
				t.Fatalf("%d complete backups, want 1", originals)
			}
		})
	}
}

func TestOverrideMigrationRecoveryNeverOverwrites(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{"old/package.json": legacyMarker, "old/decls/entitydef/a.decl": "{ original }"})
	if !crashAt(t, lib, "moved:old") {
		t.Fatal("checkpoint not reached")
	}
	lib.write(map[string]string{"old/package.json": "a new folder the user created"})
	if _, err := migrateOverrides(lib.data, lib.doom); err == nil || !strings.Contains(err.Error(), "exists again") {
		t.Fatalf("recovery overwrote or ignored a collision: %v", err)
	}
	backups := transactions(t, lib.data)
	if lib.read("old/package.json") != "a new folder the user created" || len(backups) != 1 ||
		readF(t, filepath.Join(backups[0], "original", "old", "decls", "entitydef", "a.decl")) != "{ original }" {
		t.Fatal("recovery lost one of the copies")
	}
	if err := os.RemoveAll(lib.path("old")); err != nil {
		t.Fatal(err)
	}
	report := lib.migrate()
	if len(report.Converted) != 1 || lib.read("old/assets/generated/decls/entitydef/a.decl") != "{ original }" {
		t.Fatalf("recovery did not resume: %+v", report)
	}
}

func TestOverrideMigrationDetectsChangesBeforePublication(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{"old/package.json": legacyMarker, "old/decls/entitydef/a.decl": "{ before }"})
	overrideMigrationCheckpoint = func(point string) {
		if point == "journaled" {
			overrideMigrationCheckpoint = nil
			writeF(t, lib.path("old/decls/entitydef/a.decl"), "{ edited meanwhile }")
		}
	}
	defer func() { overrideMigrationCheckpoint = nil }()
	report, err := migrateOverrides(lib.data, lib.doom)
	if err == nil || !strings.Contains(err.Error(), "changed during conversion") || len(report.Converted) != 0 {
		t.Fatalf("changed source published: %v %+v", err, report)
	}
	if lib.read("old/decls/entitydef/a.decl") != "{ edited meanwhile }" || lib.read("old/package.json") != legacyMarker {
		t.Fatal("the edited original was not restored")
	}
	report = lib.migrate()
	if len(report.Converted) != 1 || lib.read("old/assets/generated/decls/entitydef/a.decl") != "{ edited meanwhile }" {
		t.Fatalf("retry did not convert the edited source: %+v", report)
	}
}

func TestOverrideMigrationLongAndUnicodePaths(t *testing.T) {
	lib := newOverrideLibrary(t)
	deep := strings.Repeat("subfolder/", 30) + "resource.decl"
	lib.write(map[string]string{
		"Grüppé/角色/package.json":                             legacyMarker,
		"Grüppé/角色/decls/entitydef/" + deep:                  "{ deep }",
		"Grüppé/角色/images/" + strings.Repeat("ü/", 60) + "i": "image",
	})
	report := lib.migrate()
	if len(report.Converted) != 1 || len(report.Rejected) != 0 {
		t.Fatalf("report: %+v", report)
	}
	if lib.read("Grüppé/角色/assets/generated/decls/entitydef/"+deep) != "{ deep }" ||
		lib.read("Grüppé/角色/assets/generated/image/"+strings.Repeat("ü/", 60)+"i") != "image" {
		t.Fatal("long or Unicode path lost")
	}
	unicode := descriptorOf(t, lib.read("Grüppé/角色/package.json"))
	if id := unicode.member("id").text; id != "local.package" {
		t.Fatalf("non-ASCII folder id = %s", id)
	}
	if again := lib.migrate(); len(again.Converted) != 0 {
		t.Fatalf("second run changed the library: %+v", again)
	}
}

func TestOverrideMigrationAssignsUniqueIDs(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{
		"a/my-overrides/package.json":           legacyMarker,
		"a/my-overrides/decls/entitydef/a.decl": "{}",
		"z-current/package.json":                `{"id":"local.my-overrides","name":"Starter"}`,
	})
	report := lib.migrate()
	if len(report.Converted) != 1 {
		t.Fatalf("report: %+v", report)
	}
	converted := descriptorOf(t, lib.read("a/my-overrides/package.json"))
	if id := converted.member("id").text; id != "local.my-overrides-2" {
		t.Fatalf("id = %s", id)
	}
}

func TestOverrideMigrationRefusesMissingCatalogOnlyWhenNeeded(t *testing.T) {
	lib := newOverrideLibrary(t)
	lib.write(map[string]string{"old/package.json": legacyMarker, "old/decls/entitydef/a.decl": "{}"})
	if report := lib.migrate(); len(report.Converted) != 1 {
		t.Fatalf("catalog-free conversion failed: %+v", report)
	}
	lib.write(map[string]string{"maps/x.entities": "loose"})
	if _, err := migrateOverrides(lib.data, ""); err == nil || !strings.Contains(err.Error(), "DOOM installation is required") {
		t.Fatalf("loose files classified without the installed catalog: %v", err)
	}
	if lib.read("maps/x.entities") != "loose" {
		t.Fatal("loose file changed after a refused classification")
	}
}

func TestMigrateOverridesCommandReportsPackagesNeedingAttention(t *testing.T) {
	syntheticProcessGuard(t, false)
	la, _ := newDataDirs(t)
	doom := t.TempDir()
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeFakeCatalog(t, doom, nil)
	writeF(t, overridesPath(la, "bad", "package.json"), "{")
	writeF(t, overridesPath(la, "good", "package.json"), legacyMarker)
	writeF(t, overridesPath(la, "good", "decls", "entitydef", "g.decl"), "{}")
	err := runCommand("migrate-overrides", []string{"--doom", doom})
	if err == nil || !strings.Contains(err.Error(), "left unchanged") {
		t.Fatalf("command did not report the bad package: %v", err)
	}
	if readF(t, overridesPath(la, "good", "assets", "generated", "decls", "entitydef", "g.decl")) != "{}" {
		t.Fatal("healthy package not converted")
	}
	if err := runCommand("migrate-overrides", []string{"--doom", doom}); err == nil {
		t.Fatal("repeat run hid the remaining problem")
	}
	writeF(t, overridesPath(la, "bad", "package.json"), `{"id":"tests.fixed","name":"Fixed"}`)
	if err := runCommand("migrate-overrides", []string{"--doom", doom}); err != nil {
		t.Fatalf("repaired library still fails: %v", err)
	}
}

func TestInstallConvertsOverridesBeforeReplacingRuntime(t *testing.T) {
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	t.Setenv("USERPROFILE", filepath.Join(tmp, "profile"))
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeFakeCatalog(t, doom, nil)
	overrides := filepath.Join(tmp, "appdata", "snapmap-plus", "overrides")
	writeF(t, filepath.Join(overrides, "my-overrides", "package.json"), legacyMarker)
	writeF(t, filepath.Join(overrides, "my-overrides", "decls", "entitydef", "mine.decl"), "{ mine }")

	// An interrupted earlier conversion whose original path exists again must
	// stop installation before any runtime file is replaced.
	lib := &overrideLibrary{t: t, data: filepath.Join(tmp, "appdata", "snapmap-plus"), root: overrides, doom: doom}
	if !crashAt(t, lib, "moved:my-overrides") {
		t.Fatal("checkpoint not reached")
	}
	writeF(t, filepath.Join(overrides, "my-overrides", "package.json"), "recreated")
	dist := synthDist(t, tmp, "v2")
	if err := cmdInstall(flags{doom: doom, local: dist, yes: true}); err == nil {
		t.Fatal("install continued after a failed conversion recovery")
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Fatal("runtime replaced before the conversion failure was resolved")
	}
	if err := os.RemoveAll(filepath.Join(overrides, "my-overrides")); err != nil {
		t.Fatal(err)
	}
	if err := cmdInstall(flags{doom: doom, local: dist, yes: true}); err != nil {
		t.Fatalf("install after resolving the conflict: %v", err)
	}
	if readF(t, filepath.Join(overrides, "my-overrides", "assets", "generated", "decls", "entitydef", "mine.decl")) != "{ mine }" ||
		!exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Fatal("install did not convert and then deploy")
	}
}

func TestOverrideMigrationRemovesOnlyByteOrderMark(t *testing.T) {
	lib := newOverrideLibrary(t)
	descriptor := "{\n  \"id\": \"tests.current\",\n  \"name\": \"Current\",\n  \"version\": \"2.0\"\n}\n"
	lib.write(map[string]string{
		"current/package.json":                            "\xef\xbb\xbf" + descriptor,
		"current/assets/decls/snapeditorentitydef/x.decl": "current engine path, not a wrapper",
	})
	report := lib.migrate()
	if len(report.Converted) != 1 {
		t.Fatalf("report: %+v", report)
	}
	if lib.read("current/package.json") != descriptor ||
		lib.read("current/assets/decls/snapeditorentitydef/x.decl") != "current engine path, not a wrapper" {
		t.Fatal("more than the byte-order mark changed")
	}
}

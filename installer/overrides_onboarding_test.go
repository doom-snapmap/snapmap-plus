package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOriginalSnapHakOverrideOnboarding(t *testing.T) {
	_, profile := newDataDirs(t)
	doom := t.TempDir()
	writeFakeCatalog(t, doom, nil)
	files := map[string]string{
		"generated/decls/snapeditorentitydef/unknown/unknown.decl":           "{ edit = { authorValue = 1; } }\n",
		"generated/decls/snapeditorsettings/settings.decl":                   "{ edit = { authorValue = 2; } }\n",
		"generated/decls/snappropertyinspector_vec3/vec3inspector_size.decl": "{ edit = { authorValue = 3; } }\n",
	}
	for name, body := range files {
		writeF(t, filepath.Join(profile, "snaphak", "overrides", filepath.FromSlash(name)), body)
	}
	verifyOriginalOnboarding(t, doom, files)
}

func verifyOriginalOnboarding(t *testing.T, doom string, files map[string]string) {
	t.Helper()
	migrateUserData()
	report, err := migrateOverrides(appDataDir(), doom)
	if err != nil || len(report.Rejected) != 0 || len(report.Converted) != 1 {
		t.Fatalf("original SnapHak onboarding failed: %v, %+v", err, report)
	}
	root := filepath.Join(appDataDir(), "overrides")
	all := treeOf(t, root)
	prefix := ""
	for name := range all {
		if strings.HasSuffix(name, "/package.json") {
			if prefix != "" {
				t.Fatal("loose overrides split into multiple delivery units")
			}
			prefix = strings.TrimSuffix(name, "package.json")
		}
	}
	if prefix == "" {
		t.Fatal("no current package was created")
	}
	if prefix != starterPackageName+"/" {
		t.Fatalf("loose SnapHak content did not become the authoring package: %s", prefix)
	}
	if err := ensureStarterPackage(); err != nil {
		t.Fatal(err)
	}
	if got := readF(t, filepath.Join(root, starterPackageName, "package.json")); got != all[prefix+"package.json"] {
		t.Fatal("starter creation replaced the migrated package descriptor")
	}
	for name, body := range files {
		if all[prefix+"assets/"+name] != body {
			t.Errorf("authored bytes were lost at %s", name)
		}
		backup := filepath.Join(report.Converted[0].Backup, "original", filepath.FromSlash(name))
		if got := readF(t, backup); got != body {
			t.Errorf("backup differs at %s", name)
		}
	}
	if err := validateDescriptor([]byte(all[prefix+"package.json"])); err != nil {
		t.Fatal(err)
	}
	again, err := migrateOverrides(appDataDir(), doom)
	if err != nil || len(again.Converted) != 0 || len(again.Rejected) != 0 {
		t.Fatalf("migration was not idempotent: %v, %+v", err, again)
	}
	t.Logf("verified %d original files, their backups, current descriptor and repeat migration", len(files))
}

// Private acceptance against supplied archives. No user or game bytes are
// retained in the repository. Inputs are opt-in; normal CI uses synthetic data.
func TestSuppliedOverrideArchive(t *testing.T) {
	source, doom := os.Getenv("SNAPMAP_TEST_OVERRIDE_ARCHIVE"), os.Getenv("SNAPMAP_TEST_DOOM")
	if source == "" || doom == "" {
		t.Skip("set SNAPMAP_TEST_OVERRIDE_ARCHIVE and SNAPMAP_TEST_DOOM for private acceptance")
	}
	_, profile := newDataDirs(t)
	z, err := zip.OpenReader(source)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	files := map[string]string{}
	for _, file := range z.File {
		if file.FileInfo().IsDir() {
			continue
		}
		if !strings.HasPrefix(file.Name, "overrides/") {
			t.Fatalf("expected overrides/ root, got %s", file.Name)
		}
		name := strings.TrimPrefix(file.Name, "overrides/")
		if !enginePathValid(name) {
			t.Fatalf("unsafe supplied path: %s", name)
		}
		r, err := file.Open()
		if err != nil {
			t.Fatal(err)
		}
		body, err := io.ReadAll(r)
		r.Close()
		if err != nil {
			t.Fatal(err)
		}
		files[name] = string(body)
		writeF(t, filepath.Join(profile, "snaphak", "overrides", filepath.FromSlash(name)), string(body))
	}
	verifyOriginalOnboarding(t, doom, files)
}

func TestSuppliedLegacyMapConformance(t *testing.T) {
	source, converted, doom := os.Getenv("SNAPMAP_TEST_LEGACY_ARCHIVE"), os.Getenv("SNAPMAP_TEST_CONVERTED_ARCHIVE"), os.Getenv("SNAPMAP_TEST_DOOM")
	if source == "" || converted == "" || doom == "" {
		t.Skip("set legacy/converted archive paths and DOOM for private adapter conformance")
	}
	lib := newOverrideLibrary(t)
	lib.doom = doom
	read := func(path string) map[string][]byte {
		z, err := zip.OpenReader(path)
		if err != nil {
			t.Fatal(err)
		}
		defer z.Close()
		files := map[string][]byte{}
		for _, f := range z.File {
			if f.FileInfo().IsDir() {
				continue
			}
			if !enginePathValid(f.Name) {
				t.Fatalf("unsafe archive path %s", f.Name)
			}
			r, err := f.Open()
			if err != nil {
				t.Fatal(err)
			}
			body, err := io.ReadAll(r)
			r.Close()
			if err != nil {
				t.Fatal(err)
			}
			files[f.Name] = body
		}
		return files
	}
	for name, body := range read(source) {
		writeF(t, lib.path("cyberdemon/"+name), string(body))
	}
	report := lib.migrate()
	if len(report.Rejected) != 0 || len(report.Converted) != 1 {
		t.Fatalf("disk conversion: %+v", report)
	}
	expected := read(converted)
	actual := treeOf(t, lib.path("cyberdemon"))
	count := 0
	for name, body := range actual {
		if strings.HasSuffix(name, "/") {
			continue
		}
		count++
		want, found := expected[name]
		if !found {
			t.Errorf("disk-only file: %s", name)
			continue
		}
		if name == "package.json" {
			var a, b map[string]json.RawMessage
			if json.Unmarshal([]byte(body), &a) != nil || json.Unmarshal(want, &b) != nil {
				t.Fatal("invalid descriptor")
			}
			// Filesystem folder identity and embedded delivery identity differ;
			// all authored metadata, policies and resource bytes must agree.
			delete(a, "id")
			delete(b, "id")
			x, _ := json.Marshal(a)
			y, _ := json.Marshal(b)
			if !bytes.Equal(x, y) {
				t.Errorf("descriptor differs:\n%s\n%s", x, y)
			}
		} else if !bytes.Equal([]byte(body), want) {
			t.Errorf("asset differs: %s", name)
		}
	}
	if count != len(expected) {
		t.Errorf("disk/map file counts differ: %d/%d", count, len(expected))
	}
	t.Logf("compared %d files through disk and embedded-map adapters", count)
}

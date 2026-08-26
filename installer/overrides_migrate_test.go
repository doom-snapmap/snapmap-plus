package main

import (
	"os"
	"path/filepath"
	"testing"
)

func overridesPath(la string, parts ...string) string {
	return filepath.Join(append([]string{la, "snapmap-plus", "overrides"}, parts...)...)
}

func writeFileAt(t *testing.T, path, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
}

// A fresh install with no legacy tree still gets the starter package, so a user has an obvious place to drop
// their own content instead of a bare overrides root.
func TestMigrateLegacyOverrides_scaffoldsStarterPackage(t *testing.T) {
	la, _ := newDataDirs(t)

	migrateUserData()

	marker := overridesPath(la, starterPackageName, "package.json")
	if _, err := os.Stat(marker); err != nil {
		t.Fatalf("expected the starter package marker at %s: %v", marker, err)
	}
}

// The pre-package overrides\generated tree becomes a real package: its files move across with their layout
// intact, the marker is written, and the old tree is removed once everything is verified present.
func TestMigrateLegacyOverrides_movesLegacyTreeIntoPackage(t *testing.T) {
	la, _ := newDataDirs(t)
	writeFileAt(t, overridesPath(la, "generated", "decls", "snapeditorentitydef", "func", "lift.decl"), "LIFT")
	writeFileAt(t, overridesPath(la, "generated", "requirements", "mine.requirements"), "REQ")

	migrateUserData()

	got, err := os.ReadFile(overridesPath(la, starterPackageName, "decls", "snapeditorentitydef", "func", "lift.decl"))
	if err != nil || string(got) != "LIFT" {
		t.Fatalf("decl not migrated with its layout intact: %q, %v", got, err)
	}
	if got, err := os.ReadFile(overridesPath(la, starterPackageName, "requirements", "mine.requirements")); err != nil || string(got) != "REQ" {
		t.Fatalf("requirements not migrated: %q, %v", got, err)
	}
	if _, err := os.Stat(overridesPath(la, starterPackageName, "package.json")); err != nil {
		t.Errorf("the migrated folder must be marked as a package, or nothing enumerates it: %v", err)
	}
	if _, err := os.Stat(overridesPath(la, "generated")); !os.IsNotExist(err) {
		t.Errorf("the legacy tree should be gone once fully mirrored, but it still exists")
	}
}

// A legacy tree holding only empty directories is removed without claiming anything was moved.
func TestMigrateLegacyOverrides_removesEmptyLegacyTree(t *testing.T) {
	la, _ := newDataDirs(t)
	if err := os.MkdirAll(overridesPath(la, "generated", "decls"), 0o755); err != nil {
		t.Fatal(err)
	}

	migrateUserData()

	if _, err := os.Stat(overridesPath(la, "generated")); !os.IsNotExist(err) {
		t.Errorf("an empty legacy tree should be removed, but it still exists")
	}
}

// A file the user already has in the starter package wins over a same-named file in the legacy tree, and the
// legacy tree is still cleared -- the migration never overwrites, exactly like the app-data migration.
func TestMigrateLegacyOverrides_neverClobbersExisting(t *testing.T) {
	la, _ := newDataDirs(t)
	rel := filepath.Join("decls", "snapeditorentitydef", "func", "lift.decl")
	writeFileAt(t, overridesPath(la, "generated", rel), "OLD")
	writeFileAt(t, overridesPath(la, starterPackageName, rel), "MINE")

	migrateUserData()

	if got, err := os.ReadFile(overridesPath(la, starterPackageName, rel)); err != nil || string(got) != "MINE" {
		t.Fatalf("the user's own file must survive the migration: %q, %v", got, err)
	}
}

// An existing marker is never rewritten -- the user may have renamed or re-described their package, and this
// runs on every update.
func TestMigrateLegacyOverrides_keepsExistingMarker(t *testing.T) {
	la, _ := newDataDirs(t)
	marker := overridesPath(la, starterPackageName, "package.json")
	writeFileAt(t, marker, `{"name":"renamed by me"}`)

	migrateUserData()

	if got, err := os.ReadFile(marker); err != nil || string(got) != `{"name":"renamed by me"}` {
		t.Fatalf("an existing package.json must not be rewritten: %q, %v", got, err)
	}
}

// Running the installer twice is a no-op the second time: nothing is duplicated and nothing is lost.
func TestMigrateLegacyOverrides_isIdempotent(t *testing.T) {
	la, _ := newDataDirs(t)
	rel := filepath.Join("decls", "snapeditorentitydef", "func", "lift.decl")
	writeFileAt(t, overridesPath(la, "generated", rel), "LIFT")

	migrateUserData()
	migrateUserData()

	if got, err := os.ReadFile(overridesPath(la, starterPackageName, rel)); err != nil || string(got) != "LIFT" {
		t.Fatalf("content did not survive a second run: %q, %v", got, err)
	}
	if _, err := os.Stat(overridesPath(la, "generated")); !os.IsNotExist(err) {
		t.Errorf("the legacy tree reappeared on the second run")
	}
}

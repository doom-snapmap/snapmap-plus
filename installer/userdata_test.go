package main

import (
	"os"
	"path/filepath"
	"testing"
)

// newDataDirs points LOCALAPPDATA and USERPROFILE at fresh temp dirs so every path helper (appDataDir,
// oldAppDataDir, oldUserContentDir) resolves under the test's control. Returns (localAppData, userProfile).
func newDataDirs(t *testing.T) (string, string) {
	t.Helper()
	la := t.TempDir()
	up := t.TempDir()
	t.Setenv("LOCALAPPDATA", la)
	t.Setenv("USERPROFILE", up)
	return la, up
}

// TestMigrateUserData_scaffolds: a fresh profile with no old content still gets the empty content tree created.
func TestMigrateUserData_scaffolds(t *testing.T) {
	la, _ := newDataDirs(t)
	migrateUserData()
	for _, sub := range userContentSubdirs {
		if _, err := os.Stat(filepath.Join(la, "snapmap-plus", sub)); err != nil {
			t.Errorf("expected scaffolded subfolder %q, got: %v", sub, err)
		}
	}
}

// Migrate legacy content into app data and retire the verified source.
func TestMigrateUserData_foldsOldContentForward(t *testing.T) {
	la, up := newDataDirs(t)
	oldPrefab := filepath.Join(up, "snaphak", "prefabs", "arena.json")
	if err := os.MkdirAll(filepath.Dir(oldPrefab), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(oldPrefab, []byte("MY PREFAB"), 0o644); err != nil {
		t.Fatal(err)
	}

	migrateUserData()

	got, err := os.ReadFile(filepath.Join(la, "snapmap-plus", "prefabs", "arena.json"))
	if err != nil || string(got) != "MY PREFAB" {
		t.Fatalf("content not migrated to the new location: %q, %v", got, err)
	}
	// The old folder is removed after migration.
	if _, err := os.Stat(filepath.Join(up, "snaphak")); !os.IsNotExist(err) {
		t.Errorf("the old %%USERPROFILE%%\\snaphak folder should be removed after a full migration, but it still exists")
	}
}

// R-021: the retired package folders are neither copied nor created, and an old one is left
// in place for the player to deal with; everything else still moves.
func TestMigrateUserData_leavesRetiredPackageFolders(t *testing.T) {
	la, up := newDataDirs(t)
	oldPackage := filepath.Join(up, "snaphak", "overrides", "boss", "package.json") // retired package folder
	oldPrefab := filepath.Join(up, "snaphak", "prefabs", "arena.json")
	writeF(t, oldPackage, "{}")
	writeF(t, oldPrefab, "MY PREFAB")

	migrateUserData()

	if got := readF(t, filepath.Join(la, "snapmap-plus", "prefabs", "arena.json")); got != "MY PREFAB" {
		t.Fatalf("prefab not migrated: %q", got)
	}
	for _, retired := range []string{"overrides", "package-cache", "package-staging", "override-backups"} { // retired package folder names
		if exists(filepath.Join(la, "snapmap-plus", retired)) {
			t.Errorf("migration created the retired folder %s", retired)
		}
	}
	if got := readF(t, oldPackage); got != "{}" {
		t.Fatalf("the old package folder was not left in place: %q", got)
	}
	if exists(oldPrefab) {
		t.Error("the migrated prefab was not retired from the old folder")
	}
}

// TestMigrateUserData_neverClobbersNewer: if the user already has a file at the new location, an old-location
// file of the same name must NOT overwrite it (and re-running is a no-op).
func TestMigrateUserData_neverClobbersNewer(t *testing.T) {
	la, up := newDataDirs(t)
	name := filepath.Join("prefabs", "keep.json")

	oldFile := filepath.Join(up, "snaphak", name)
	os.MkdirAll(filepath.Dir(oldFile), 0o755)
	os.WriteFile(oldFile, []byte("OLD"), 0o644)

	newFile := filepath.Join(la, "snapmap-plus", name)
	os.MkdirAll(filepath.Dir(newFile), 0o755)
	os.WriteFile(newFile, []byte("NEW"), 0o644)

	migrateUserData()
	migrateUserData() // idempotent

	got, _ := os.ReadFile(newFile)
	if string(got) != "NEW" {
		t.Errorf("migration clobbered newer content: got %q, want %q", got, "NEW")
	}
	if got := readF(t, oldFile); got != "OLD" {
		t.Fatalf("conflicting source was discarded: %q", got)
	}
}

// TestLoadRecord_fallsBackToOldLocation: a record written by a pre-rename install (at %LOCALAPPDATA%\open-snaphak)
// is found by loadRecord and migrated forward to the new location.
func TestLoadRecord_fallsBackToOldLocation(t *testing.T) {
	la, _ := newDataDirs(t)
	oldDir := filepath.Join(la, "open-snaphak")
	os.MkdirAll(oldDir, 0o755)
	os.WriteFile(filepath.Join(oldDir, "install.json"), []byte(`{"version":"v0.9.9","doom_path":"D:\\DOOM"}`), 0o644)

	rec, err := loadRecord()
	if err != nil {
		t.Fatalf("loadRecord should have found the old record: %v", err)
	}
	if rec.Version != "v0.9.9" {
		t.Errorf("wrong version from fallback record: %q", rec.Version)
	}
	if _, err := os.Stat(filepath.Join(la, "snapmap-plus", "install.json")); err != nil {
		t.Errorf("the old record should have been migrated forward to the new location: %v", err)
	}
}

// TestResolveToken_fallsBackToOldLocation: a token saved by a pre-rename install is still honored.
func TestResolveToken_fallsBackToOldLocation(t *testing.T) {
	la, _ := newDataDirs(t)
	oldDir := filepath.Join(la, "open-snaphak")
	os.MkdirAll(oldDir, 0o755)
	os.WriteFile(filepath.Join(oldDir, "token"), []byte("ghp_oldtoken\n"), 0o600)

	if got := resolveToken(flags{}); got != "ghp_oldtoken" {
		t.Errorf("resolveToken should fall back to the old token location: got %q", got)
	}
}

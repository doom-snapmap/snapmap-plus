package main

import (
	"os"
	"path/filepath"
	"testing"
)

// Isolate both the current and earlier data homes.
func newDataDirs(t *testing.T) (string, string) {
	t.Helper()
	la := t.TempDir()
	up := t.TempDir()
	t.Setenv("LOCALAPPDATA", la)
	t.Setenv("USERPROFILE", up)
	return la, up
}

func TestEnsureUserDataTree(t *testing.T) {
	local, profile := newDataDirs(t)
	old := filepath.Join(profile, "snaphak", "prefabs", "arena.json")
	oldData := filepath.Join(local, "open-snaphak")
	writeF(t, old, "my prefab")
	writeF(t, filepath.Join(oldData, "token"), "old token")
	writeF(t, filepath.Join(oldData, "install.json"), "{\"doom_path\":\"old folder\"}")
	ensureUserDataTree()
	if _, err := os.Stat(filepath.Join(appDataDir(), "prefabs")); err != nil {
		t.Fatal(err)
	}
	if readF(t, old) != "my prefab" || exists(filepath.Join(appDataDir(), "prefabs", "arena.json")) {
		t.Fatal("unrelated prefab moved")
	}
	if _, err := loadRecord(); err != errNotInstalled {
		t.Fatalf("read the retired install record: %v", err)
	}
	t.Setenv("SNAPMAP_PLUS_TOKEN", "")
	if resolveToken(flags{}) != "" {
		t.Fatal("read the retired token")
	}
}

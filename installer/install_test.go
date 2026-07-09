package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

// TestInstallUninstallRoundTrip exercises install -> uninstall against a synthetic bundle + fake DOOM dir.
// Fully hermetic (no real DOOM, no network): it verifies the overlay deploys, a pre-existing file is backed
// up, and uninstall restores the original + removes everything it placed (including snaphak_logs/).
func TestInstallUninstallRoundTrip(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	dist := filepath.Join(tmp, "dist")
	mkdirAll(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))

	// a pre-existing file to exercise backup/restore
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeF(t, filepath.Join(doom, "XINPUT1_3.dll"), "ORIGINAL")

	// a synthetic dist with a MANIFEST.sha256 (matches package-qt.ps1's "hash  relpath" format)
	overlay := map[string]string{
		"XINPUT1_3.dll": "backend",
		filepath.Join("snaphak", "snaphakui.dll"):  "ui",
		filepath.Join("platforms", "qwindows.dll"): "qt",
	}
	manifest := ""
	for rel, content := range overlay {
		writeF(t, filepath.Join(dist, rel), content)
		sum := sha256.Sum256([]byte(content))
		manifest += hex.EncodeToString(sum[:]) + "  " + rel + "\n"
	}
	writeF(t, filepath.Join(dist, "MANIFEST.sha256"), manifest)

	if err := cmdInstall(flags{doom: doom, local: dist}); err != nil {
		t.Fatalf("install: %v", err)
	}
	if !exists(filepath.Join(doom, "XINPUT1_3.dll.snaphak-bak")) {
		t.Error("expected a backup of the pre-existing XINPUT1_3.dll")
	}
	if got := readF(t, filepath.Join(doom, "XINPUT1_3.dll")); got != "backend" {
		t.Errorf("deployed XINPUT1_3.dll = %q, want %q", got, "backend")
	}

	// simulate a runtime log dir that uninstall must clean
	mkdirAll(t, filepath.Join(doom, "snaphak_logs"))
	writeF(t, filepath.Join(doom, "snaphak_logs", "snaphak_backend.log"), "log")

	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if got := readF(t, filepath.Join(doom, "XINPUT1_3.dll")); got != "ORIGINAL" {
		t.Errorf("after uninstall XINPUT1_3.dll = %q, want the restored %q", got, "ORIGINAL")
	}
	for _, p := range []string{"snaphak", "platforms", "snaphak_logs", "XINPUT1_3.dll.snaphak-bak"} {
		if exists(filepath.Join(doom, p)) {
			t.Errorf("expected %q removed after uninstall", p)
		}
	}
}

// TestUpdateDoesNotBackUpOwnDLL guards the vanilla-restore against the "backed up our own DLL" bug: on a clean
// DOOM dir (no genuine XINPUT1_3.dll -- the common case), a fresh install must create no backup, an update must
// NOT back up our own previously-installed DLL, and uninstall must then leave the dir with NO XINPUT1_3.dll
// (there was nothing genuine to restore) rather than "restoring" our own DLL as if it were vanilla.
func TestUpdateDoesNotBackUpOwnDLL(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	mkdirAll(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe") // a clean DOOM dir -- NO pre-existing XINPUT1_3.dll

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v1")}); err != nil {
		t.Fatalf("install v1: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll.snaphak-bak")) {
		t.Fatal("fresh install into a clean dir must not create a backup (there was no genuine file)")
	}
	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v2")}); err != nil { // an update
		t.Fatalf("update: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll.snaphak-bak")) {
		t.Error("update must not back up our own previously-installed DLL")
	}
	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Error("after uninstall a clean dir must have no XINPUT1_3.dll (nothing genuine to restore)")
	}
}

// synthDist writes a synthetic dist/ (the 3 overlay files + a matching MANIFEST.sha256) whose content varies by
// tag, so installing two different tags exercises the update path (distinct bytes -> distinct hashes).
func synthDist(t *testing.T, dir, tag string) string {
	t.Helper()
	dist := filepath.Join(dir, "dist-"+tag)
	overlay := map[string]string{
		"XINPUT1_3.dll": "backend-" + tag,
		filepath.Join("snaphak", "snaphakui.dll"):  "ui-" + tag,
		filepath.Join("platforms", "qwindows.dll"): "qt-" + tag,
	}
	manifest := ""
	for rel, content := range overlay {
		writeF(t, filepath.Join(dist, rel), content)
		sum := sha256.Sum256([]byte(content))
		manifest += hex.EncodeToString(sum[:]) + "  " + rel + "\n"
	}
	writeF(t, filepath.Join(dist, "MANIFEST.sha256"), manifest)
	return dist
}

func mkdirAll(t *testing.T, dir string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
}

func writeF(t *testing.T, path, content string) {
	t.Helper()
	mkdirAll(t, filepath.Dir(path))
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readF(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

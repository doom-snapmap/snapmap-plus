package main

import (
	"path/filepath"
	"testing"
)

// ogFootprint lays a full original-SnapHak overlay into a fake DOOM dir (every file its bundle ships,
// at the paths it ships them), plus the vanilla files that must survive a migration untouched.
func ogFootprint(t *testing.T, doom string) {
	t.Helper()
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeF(t, filepath.Join(doom, "superscriptx64.dll"), "vanilla") // a GENUINE game file -- never touch
	writeF(t, filepath.Join(doom, "bink2w64.dll"), "vanilla")
	for _, rel := range []string{
		"dinput8.dll", "XINPUT1_3.dll", "snaphak_algo.dll", "snaphak_ext.dll",
		"changelog.txt", "doomlegacymod.txt",
		filepath.Join("snaphak", "snaphakui.dll"),
		filepath.Join("snaphak", "Qt5Core.dll"), filepath.Join("snaphak", "Qt5Gui.dll"),
		filepath.Join("snaphak", "Qt5Svg.dll"), filepath.Join("snaphak", "Qt5Widgets.dll"),
		filepath.Join("snaphak", "lua51.dll"), filepath.Join("snaphak", "ds_descriptions.json"),
		filepath.Join("plugins", "platforms", "qwindows.dll"),
		filepath.Join("platforms", "qwindows.dll"),
	} {
		writeF(t, filepath.Join(doom, rel), "OG")
	}
}

// TestLegacyMigrationOnInstall: installing into a DOOM folder that has the original SnapHak removes
// every original file, deploys ours, creates NO backup of the original's DLLs (they are not genuine
// game files), prunes the emptied Qt plugin dirs, and leaves vanilla files alone.
func TestLegacyMigrationOnInstall(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	ogFootprint(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v1")}); err != nil {
		t.Fatalf("install: %v", err)
	}
	for _, rel := range []string{
		"dinput8.dll", "snaphak_algo.dll", "snaphak_ext.dll", "changelog.txt", "doomlegacymod.txt",
		filepath.Join("snaphak", "Qt5Core.dll"), filepath.Join("snaphak", "Qt5Gui.dll"),
		filepath.Join("snaphak", "Qt5Svg.dll"), filepath.Join("snaphak", "Qt5Widgets.dll"),
		filepath.Join("snaphak", "lua51.dll"), filepath.Join("snaphak", "ds_descriptions.json"),
		"plugins",
	} {
		if exists(filepath.Join(doom, rel)) {
			t.Errorf("original-SnapHak file %q should be removed", rel)
		}
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll.snaphak-bak")) {
		t.Error("the original's XINPUT1_3.dll must NOT be backed up as a genuine file")
	}
	if got := readF(t, filepath.Join(doom, "XINPUT1_3.dll")); got != "backend-v1" {
		t.Errorf("XINPUT1_3.dll = %q, want ours (%q)", got, "backend-v1")
	}
	if got := readF(t, filepath.Join(doom, "snaphak", "snaphakui.dll")); got != "ui-v1" {
		t.Errorf("snaphakui.dll = %q, want ours (%q)", got, "ui-v1")
	}
	for _, rel := range []string{"superscriptx64.dll", "bink2w64.dll", "DOOMx64vk.exe"} {
		if got := readF(t, filepath.Join(doom, rel)); got != "vanilla" && rel != "DOOMx64vk.exe" {
			t.Errorf("vanilla file %q = %q, want untouched", rel, got)
		}
	}
	rec, err := loadRecord()
	if err != nil {
		t.Fatalf("record: %v", err)
	}
	if len(rec.LegacyRemoved) == 0 {
		t.Error("record should note the original-SnapHak files that were removed")
	}
	if len(rec.Backups) != 0 {
		t.Errorf("no genuine file was overwritten -- Backups should be empty, got %v", rec.Backups)
	}

	// and the migrated install uninstalls to truly-vanilla: no XINPUT1_3.dll, no snaphak\ left
	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	for _, rel := range []string{"XINPUT1_3.dll", "snaphak"} {
		if exists(filepath.Join(doom, rel)) {
			t.Errorf("after uninstall %q should be gone (vanilla DOOM ships no such path)", rel)
		}
	}
	if !exists(filepath.Join(doom, "superscriptx64.dll")) {
		t.Error("uninstall must leave vanilla superscriptx64.dll in place")
	}
}

// TestLoneDinput8IsNotLegacy: a dinput8.dll with no original-SnapHak marker could be an unrelated
// mod -- detection must not fire and the file must survive an install.
func TestLoneDinput8IsNotLegacy(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeF(t, filepath.Join(doom, "dinput8.dll"), "some other mod")
	writeF(t, filepath.Join(doom, "changelog.txt"), "some other changelog")
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))

	if got := detectLegacy(doom); got != nil {
		t.Fatalf("detectLegacy = %v, want nil (no marker present)", got)
	}
	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v1")}); err != nil {
		t.Fatalf("install: %v", err)
	}
	if got := readF(t, filepath.Join(doom, "dinput8.dll")); got != "some other mod" {
		t.Errorf("lone dinput8.dll = %q, want untouched", got)
	}
	if got := readF(t, filepath.Join(doom, "changelog.txt")); got != "some other changelog" {
		t.Errorf("lone changelog.txt = %q, want untouched", got)
	}
}

// TestDetectLegacySingleMarker: one marker is enough, and extras ride along only then.
func TestDetectLegacySingleMarker(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	writeF(t, filepath.Join(doom, "snaphak_ext.dll"), "OG")
	writeF(t, filepath.Join(doom, "dinput8.dll"), "OG")
	got := detectLegacy(doom)
	want := map[string]bool{"snaphak_ext.dll": true, "dinput8.dll": true}
	if len(got) != len(want) {
		t.Fatalf("detectLegacy = %v, want exactly %v", got, want)
	}
	for _, rel := range got {
		if !want[rel] {
			t.Errorf("unexpected detection %q", rel)
		}
	}
}

// TestLegacyBackupDropped: an install made BEFORE this migration existed backed up the original's
// XINPUT1_3.dll as if it were genuine. The next install (an update) with the original still present
// must delete that stale backup and drop its record entry, so uninstall restores nothing non-vanilla.
func TestLegacyBackupDropped(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	ogFootprint(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))

	// simulate the pre-migration state: our v1 is installed, the original's DLL sits in a backup
	bak := filepath.Join(doom, "XINPUT1_3.dll.snaphak-bak")
	writeF(t, bak, "OG")
	if err := saveRecord(&installRecord{
		Version:  "v0",
		DoomPath: doom,
		Files:    []string{"XINPUT1_3.dll", filepath.Join("snaphak", "snaphakui.dll")},
		Backups:  []backup{{Rel: "XINPUT1_3.dll", Backup: bak}},
	}); err != nil {
		t.Fatal(err)
	}

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v2")}); err != nil {
		t.Fatalf("update: %v", err)
	}
	if exists(bak) {
		t.Error("the stale backup of the original's XINPUT1_3.dll should be deleted")
	}
	rec, err := loadRecord()
	if err != nil {
		t.Fatalf("record: %v", err)
	}
	if len(rec.Backups) != 0 {
		t.Errorf("Backups should be empty after the migration, got %v", rec.Backups)
	}
	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Error("after uninstall no XINPUT1_3.dll may remain (nothing genuine to restore)")
	}
}

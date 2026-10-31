package main

import (
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// These tests hold the layout contract on every migration path: after an install, update or
// migration the DOOM folder holds exactly the detected build's pair and nothing else of ours, and the
// data folder holds the two executables, install.json and the player's own data.

// ourStandIn is a stand-in for a DLL of an earlier Snapmap+ release: it carries the marker every
// Snapmap+ DLL does, and bytes no current bundle has.
func ourStandIn(name string) string {
	return "MZ stand-in for " + name + " .\\snapmap-plus\\ of an earlier release"
}

// doomFiles lists every file under doom, relative to it, sorted.
func doomFiles(t *testing.T, doom string) []string {
	t.Helper()
	var out []string
	filepath.WalkDir(doom, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			t.Fatal(err)
		}
		if !d.IsDir() {
			rel, _ := filepath.Rel(doom, path)
			out = append(out, rel)
		}
		return nil
	})
	sort.Strings(out)
	return out
}

// wantDoom fails unless doom holds exactly the vanilla executables, the given extra files and pair.
func wantDoom(t *testing.T, stage, doom string, extra ...string) {
	t.Helper()
	want := append([]string{"DOOMx64.exe", "DOOMx64vk.exe"}, extra...)
	sort.Strings(want)
	if got := doomFiles(t, doom); strings.Join(got, "|") != strings.Join(want, "|") {
		t.Errorf("%s: the DOOM folder holds\n  %s\nwant exactly\n  %s", stage, strings.Join(got, "\n  "), strings.Join(want, "\n  "))
	}
}

// dataAllowed are the data folder's entries the contract allows: the installer's three, and the
// player's data.
var dataAllowed = map[string]bool{
	selfExeAsset: true, storeToolName: true, stateName: true,
	"config.json": true, "prefabs": true, "strings": true, "mods": true, "rawmap.json": true, "test-maps": true,
	"cache": true, "token": true, "pinned.json": true, "overrides": true,
}

// wantData fails on any data-folder entry outside the contract, and on anything in cache\installer
// but the recorded backups of files that were not ours.
func wantData(t *testing.T, stage, data string) {
	t.Helper()
	entries, _ := os.ReadDir(data)
	for _, e := range entries {
		if !dataAllowed[e.Name()] {
			t.Errorf("%s: the data folder holds %s", stage, e.Name())
		}
	}
	recorded := map[string]bool{}
	if rec, err := loadStateAt(data); err == nil {
		for _, bk := range rec.Backups {
			recorded[strings.ToLower(bk.Backup)] = true
		}
	}
	if exists(installerCacheDir(data)) {
		var left []string
		filepath.WalkDir(installerCacheDir(data), func(path string, d fs.DirEntry, err error) error {
			if err == nil && !d.IsDir() && !recorded[strings.ToLower(path)] {
				left = append(left, path)
			}
			return nil
		})
		if len(left) > 0 {
			t.Errorf("%s: cache\\installer holds %v", stage, left)
		}
	}
}

func allOutput(t *testing.T, fn func()) string {
	t.Helper()
	var out string
	errOut := captureStderr(t, func() { out = captureStdout(t, fn) })
	return out + errOut
}

// From the original SnapHak: its files go, the player's content under %USERPROFILE%\snaphak comes
// across, the pair is installed, and a file that is not ours stays and is named.
func TestContractFromSnapHak(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	ogFootprint(t, r.doom)
	writeF(t, filepath.Join(r.doom, "DOOMx64vk.exe"), fakeExe)
	writeF(t, filepath.Join(r.doom, "dinput8.dll.doomforge.rollback"), "another tool's file")
	profile := filepath.Join(r.tmp, "profile")
	writeF(t, filepath.Join(profile, "snaphak", "prefabs", "arena.json"), `{"prefab":1}`)
	writeF(t, filepath.Join(profile, "snaphak", "strings", "en.json"), `{}`)
	writeF(t, filepath.Join(profile, "snaphak", "rawmap.json"), `{"rawmap":1}`)
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	out := allOutput(t, func() {
		if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
			t.Fatalf("install: %v", err)
		}
	})
	for _, want := range []string{"Found:            the original SnapHak", "Removing the original SnapHak", "Not ours, left in place:", "dinput8.dll.doomforge.rollback"} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	wantDoom(t, "SnapHak install", r.doom, "XINPUT1_4.dll", currentUIDll, "bink2w64.dll", "superscriptx64.dll", "dinput8.dll.doomforge.rollback")
	wantData(t, "SnapHak install", r.data)
	for _, rel := range []string{`prefabs\arena.json`, `strings\en.json`, "rawmap.json"} {
		if !exists(filepath.Join(r.data, rel)) {
			t.Errorf("%s did not come across", rel)
		}
	}
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatal(err)
	}
	wantDoom(t, "uninstall", r.doom, "bink2w64.dll", "superscriptx64.dll", "dinput8.dll.doomforge.rollback")
	wantData(t, "uninstall", r.data)
	if exists(statePath(r.data)) || !exists(filepath.Join(r.data, "prefabs", "arena.json")) {
		t.Error("uninstall kept install.json or removed the player's prefabs")
	}
}

// From v0.2.1-beta.19 on the April 2024 build: the 2018 pair it left goes, whether recorded or not,
// and the April 2024 pair is installed from a bundle that no longer carries the 2018 files.
func TestContractFromBeta19Layout(t *testing.T) {
	for _, recorded := range []bool{true, false} {
		t.Run(map[bool]string{true: "recorded", false: "unrecorded"}[recorded], func(t *testing.T) {
			r := newUpdateRig(t, "v1.0.0-beta.2")
			writeF(t, filepath.Join(r.doom, legacyProxy), ourStandIn(legacyProxy))
			writeF(t, filepath.Join(r.doom, legacyUIDll), ourStandIn(legacyUIDll))
			writeF(t, filepath.Join(r.data, `overrides\tinypack\package.json`), `{"id":"tinypack"}`)
			files := []string{}
			if recorded {
				files = []string{legacyProxy, legacyUIDll}
			}
			if err := saveRecord(&installRecord{Version: legacyReleaseTag, DoomPath: r.doom, Files: files}); err != nil {
				t.Fatal(err)
			}
			sum := sha256Hex([]byte("beta.19 installer"))
			writeF(t, filepath.Join(r.data, selfExeAsset), "beta.19 installer")
			writeF(t, filepath.Join(r.data, retiredExecutableRecord), `{"snapmap-plus.exe":"`+sum+`"}`)
			m := newMockGitHub(t)
			m.publish(goodRelease(t, "v1.0.0-beta.2", true))
			startupCleanup()
			out := allOutput(t, func() {
				if err := cmdUpdate(flags{yes: true}); err != nil {
					t.Fatalf("update: %v", err)
				}
			})
			for _, want := range []string{"with the other DOOM build's files", "Removed files of ours:", legacyProxy, legacyUIDll} {
				if !strings.Contains(out, want) {
					t.Errorf("output lacks %q:\n%s", want, out)
				}
			}
			wantDoom(t, "beta.19 update", r.doom, "XINPUT1_4.dll", currentUIDll)
			wantData(t, "beta.19 update", r.data)
			rec, err := loadRecord()
			if err != nil || rec.Executables[selfExeAsset].SHA256 != sha256Hex(installerBytes("v1.0.0-beta.2")) || rec.DoomBuild != build2024 {
				t.Errorf("install.json = %+v, %v", rec, err)
			}
			if !exists(filepath.Join(r.data, `overrides\tinypack\package.json`)) {
				t.Error("the player's overrides package was touched")
			}
		})
	}
}

// From v0.2.1-beta.20: both pairs, backups of our own DLLs in DOOM, a backup of a file that was not
// ours, and every retired bookkeeping file. The update leaves the contract, and uninstall restores the
// file that was there before Snapmap+.
func TestContractFromBeta20Layout(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	stable := filepath.Join(r.data, selfExeAsset)
	writeF(t, stable, "beta.20 installer")
	writeF(t, filepath.Join(r.doom, "XINPUT1_4.dll"), ourStandIn("XINPUT1_4.dll"))
	writeF(t, filepath.Join(r.doom, currentUIDll), ourStandIn(currentUIDll))
	writeF(t, filepath.Join(r.doom, legacyProxy), ourStandIn(legacyProxy))
	writeF(t, filepath.Join(r.doom, legacyUIDll), ourStandIn(legacyUIDll))
	writeF(t, filepath.Join(r.doom, "XINPUT1_4.dll.snapmap-plus-bak"), ourStandIn("an older XINPUT1_4.dll"))
	writeF(t, filepath.Join(r.doom, currentUIDll+".snapmap-plus-bak"), ourStandIn("an older frontend"))
	writeF(t, filepath.Join(r.doom, legacyUIDll+".snapmap-plus-bak"), ourStandIn("an older 2018 frontend"))
	writeF(t, filepath.Join(r.doom, "xinput1_3.dll.snapmap-plus-bak"), "a controller tool's XInput proxy")
	writeF(t, filepath.Join(r.doom, "dinput8.dll.doomforge.rollback"), "another tool's file")
	if err := saveRecord(&installRecord{Version: "v0.2.1-beta.20", DoomPath: r.doom,
		Files: []string{"XINPUT1_4.dll", currentUIDll, legacyProxy, legacyUIDll},
		Backups: []backup{
			{Rel: "XINPUT1_4.dll", Backup: filepath.Join(r.doom, "XINPUT1_4.dll.snapmap-plus-bak")},
			{Rel: legacyProxy, Backup: filepath.Join(r.doom, "xinput1_3.dll.snapmap-plus-bak")},
		}}); err != nil {
		t.Fatal(err)
	}
	sum := sha256Hex([]byte("beta.20 installer"))
	writeF(t, filepath.Join(r.data, retiredExecutableRecord), `{"snapmap-plus.exe":"`+sum+`"}`)
	writeF(t, filepath.Join(r.data, retiredVersionsRecord), `{"snapmap-plus.exe":{"sha256":"`+sum+`","version":"v0.2.1-beta.20"}}`)
	writeF(t, stable+".updates.json", `{"current_sha256":"`+sum+`","backups":{"snapmap-plus.exe.old":"00"}}`)
	writeF(t, stable+".old", "the beta.19 installer it replaced")
	mkdirAll(t, filepath.Join(r.data, "override-backups"))
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))

	startupCleanup()
	out := allOutput(t, func() {
		if err := cmdUpdate(flags{yes: true}); err != nil {
			t.Fatalf("update: %v", err)
		}
	})
	for _, want := range []string{"with both DOOM builds' files, backups in the DOOM folder and retired installer records", "Tidying the data folder",
		"installer-files.json (folded into install.json)", "snapmap-plus.exe.old", "Backups of files that were not ours",
		"Not ours, left in place:", "dinput8.dll.doomforge.rollback", `override-backups\ (an empty folder of an earlier release)`} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	wantDoom(t, "beta.20 update", r.doom, "XINPUT1_4.dll", currentUIDll, "dinput8.dll.doomforge.rollback")
	wantData(t, "beta.20 update", filepath.Join(r.data))
	saved := filepath.Join(installerCacheDir(r.data), "backup", legacyProxy)
	if readF(t, saved) != "a controller tool's XInput proxy" {
		t.Fatal("the backup of a file that was not ours was not kept in cache, installer, backup")
	}
	rec, err := loadRecord()
	if err != nil || len(rec.Backups) != 1 || rec.Backups[0].Rel != legacyProxy || rec.Backups[0].Backup != saved {
		t.Fatalf("recorded backups = %+v, %v", rec, err)
	}
	if strings.Join(rec.Files, "|") != "XINPUT1_4.dll|"+currentUIDll {
		t.Errorf("recorded files = %q", rec.Files)
	}

	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatal(err)
	}
	wantDoom(t, "uninstall", r.doom, legacyProxy, "dinput8.dll.doomforge.rollback")
	if readF(t, filepath.Join(r.doom, legacyProxy)) != "a controller tool's XInput proxy" {
		t.Error("uninstall did not put back the file that was there before Snapmap+")
	}
	wantData(t, "uninstall", r.data)
	if exists(statePath(r.data)) || exists(filepath.Join(r.data, "cache")) {
		t.Error("uninstall left install.json or the installer cache")
	}
}

// From the current release: a plain update, with nothing to clean, and install.json carries the
// folded state with its version key first, as the DLLs read it.
func TestContractPlainUpdateAndRecordShape(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	r.seedUserData(t)
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatal(err)
	}
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	out := allOutput(t, func() {
		if err := cmdUpdate(flags{yes: true}); err != nil {
			t.Fatal(err)
		}
	})
	if !strings.Contains(out, "Found:            Snapmap+ v1.0.0-beta.1") || !strings.Contains(out, "A plain update") {
		t.Errorf("output:\n%s", out)
	}
	wantDoom(t, "plain update", r.doom, "XINPUT1_4.dll", currentUIDll)
	raw := readF(t, statePath(r.data))
	if !strings.HasPrefix(raw, "{\n  \"version\": \"v1.0.0-beta.2\"") {
		t.Errorf("install.json does not begin with its version:\n%s", raw)
	}
	rec, _ := loadRecord()
	if rec.DoomPath != r.doom || rec.DoomBuild != build2024 || rec.Executables[selfExeAsset].Version != "v1.0.0-beta.2" ||
		rec.SelfUpdate == nil || rec.SelfUpdate.Release != "v1.0.0-beta.2" || rec.SelfUpdate.PlacedSHA256 == "" {
		t.Errorf("install.json = %s", raw)
	}
	for _, retired := range []string{retiredExecutableRecord, retiredVersionsRecord, selfExeAsset + ".updates.json", selfExeAsset + ".old"} {
		if exists(filepath.Join(r.data, retired)) {
			t.Errorf("the update wrote %s", retired)
		}
	}
}

// A fresh machine: the pair, the record and nothing else.
func TestContractFreshInstall(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	out := allOutput(t, func() {
		if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
			t.Fatal(err)
		}
	})
	if !strings.Contains(out, "nothing: this is a fresh install") {
		t.Errorf("output:\n%s", out)
	}
	wantDoom(t, "fresh install", r.doom, "XINPUT1_4.dll", currentUIDll)
	wantData(t, "fresh install", r.data)
}

// An old bundle that still carries the 2018 pair installs only the detected build's pair, so it can
// never put the other build's files back.
func TestContractOldUniversalBundleCannotRepollute(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	m := newMockGitHub(t)
	m.publish(universalRelease(t, "v1.0.0-beta.2"))
	out := allOutput(t, func() {
		if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
			t.Fatal(err)
		}
	})
	if !strings.Contains(out, "not installed: "+legacyProxy) {
		t.Errorf("output does not name the skipped 2018 file:\n%s", out)
	}
	wantDoom(t, "old bundle", r.doom, "XINPUT1_4.dll", currentUIDll)
}

// A file at a name of ours that is someone else's (no Snapmap+ marker, not recorded, not a bundle
// file) is left in place and named, never removed.
func TestContractLeavesForeignFilesAtOurNames(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.2")
	writeF(t, filepath.Join(r.doom, legacyProxy), "a controller tool's XInput proxy")
	writeF(t, filepath.Join(r.doom, "snapmap-plus", "notes.txt"), "the player's note")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	out := allOutput(t, func() {
		if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
			t.Fatal(err)
		}
	})
	for _, want := range []string{"Not ours, left in place:", legacyProxy, `snapmap-plus\notes.txt`} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	wantDoom(t, "foreign files", r.doom, "XINPUT1_4.dll", currentUIDll, legacyProxy, `snapmap-plus\notes.txt`)
}

// The ownership rule: recorded, a bundle file's bytes, or the Snapmap+ marker in ASCII or UTF-16.
func TestOwnershipRule(t *testing.T) {
	doom := t.TempDir()
	writeF(t, filepath.Join(doom, "a.dll"), "plain")
	writeF(t, filepath.Join(doom, "b.dll"), "has snapmap-plus inside")
	writeF(t, filepath.Join(doom, "c.dll"), "s\x00n\x00a\x00p\x00m\x00a\x00p\x00-\x00p\x00l\x00u\x00s\x00")
	writeF(t, filepath.Join(doom, "d.dll"), "bundle bytes")
	writeF(t, filepath.Join(doom, "e.dll"), "recorded")
	own := newOwnership(doom, &installRecord{DoomPath: doom, Files: []string{"E.DLL"}},
		[]manifestEntry{{rel: "x", sha256: sha256Hex([]byte("bundle bytes"))}})
	for name, want := range map[string]bool{"a.dll": false, "b.dll": true, "c.dll": true, "d.dll": true, "e.dll": true} {
		if got := own.ours(doom, name); got != want {
			t.Errorf("ours(%s) = %v, want %v", name, got, want)
		}
	}
	if newOwnership(doom, &installRecord{DoomPath: filepath.Join(doom, "other"), Files: []string{"e.dll"}}, nil).ours(doom, "e.dll") {
		t.Error("a record of another DOOM folder made a file ours")
	}
}

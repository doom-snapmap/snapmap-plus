package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// legacyRun records one run of the legacy installer.
type legacyRun struct {
	name  string
	bytes string
	args  []string
}

// fakeLegacyInstaller stands in for running v0.2.1-beta.19's snapmap-plus.exe: it records the call
// and does what that installer's install does to app data and the DOOM folder: it copies itself into
// app data under its own name and records that copy in installer-files.json, rewrites install.json in
// its own format (dropping fields it does not know), places the 2018 pair, backs up a file that is in
// the way as <file>.snapmap-plus-bak in DOOM, and keeps the my-overrides starter package. fail makes
// it exit with status 3 instead.
func fakeLegacyInstaller(t *testing.T, fail bool) *[]legacyRun {
	t.Helper()
	var runs []legacyRun
	saved := runLegacyExe
	runLegacyExe = func(path string, args []string) error {
		b, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("legacy installer not on disk: %v", err)
		}
		runs = append(runs, legacyRun{filepath.Base(path), string(b), args})
		if fail {
			return exec.Command("cmd", "/c", "exit 3").Run()
		}
		self := filepath.Join(appDataDir(), filepath.Base(path))
		if err := copyFile(path, self); err != nil {
			t.Fatalf("legacy self-copy: %v", err)
		}
		writeF(t, filepath.Join(appDataDir(), retiredExecutableRecord), `{"`+filepath.Base(path)+`":"`+sha256Hex(b)+`"}`)
		doom := args[2]
		var backups []backup
		if target := filepath.Join(doom, legacyProxy); isRegular(target) {
			os.Rename(target, target+".snapmap-plus-bak")
			backups = append(backups, backup{Rel: legacyProxy, Backup: target + ".snapmap-plus-bak"})
		}
		writeF(t, filepath.Join(doom, legacyProxy), "backend-legacy snapmap-plus")
		writeF(t, filepath.Join(doom, legacyUIDll), "ui-legacy snapmap-plus")
		writeF(t, filepath.Join(appDataDir(), "overrides", "my-overrides", "package.json"), `{"id":"my-overrides"}`)
		return saveRecord(&installRecord{Version: legacyReleaseTag, DoomPath: doom, Files: []string{legacyProxy, legacyUIDll}, Backups: backups})
	}
	t.Cleanup(func() { runLegacyExe = saved })
	return &runs
}

func legacyMockRelease(t *testing.T) *mockRelease {
	t.Helper()
	r := goodRelease(t, legacyReleaseTag, true)
	exe := []byte("snapmap-plus.exe " + legacyReleaseTag + " (the 2018 installer)")
	r.assets[selfExeAsset] = &mockAsset{body: exe, digest: "sha256:" + sha256Hex(exe)}
	return r
}

// The 2018 build is installed, updated and reinstalled by v0.2.1-beta.19's own installer, downloaded
// by tag, verified, and run under a name that never replaces the current installer's stable copy.
func TestInstall2018BuildRunsTheLegacyInstaller(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	writeF(t, filepath.Join(r.doom, "DOOMx64vk.exe"), fake2018)
	writeF(t, filepath.Join(r.doom, "DOOMx64.exe"), fake2018)
	m := newMockGitHub(t)
	m.publish(legacyMockRelease(t))
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	runs := fakeLegacyInstaller(t, false)

	if err := cmdInstall(flags{yes: true, doom: r.doom, beta: true}); err != nil {
		t.Fatalf("install on 2018: %v", err)
	}
	if len(*runs) != 1 {
		t.Fatalf("legacy installer ran %d times, want 1", len(*runs))
	}
	run := (*runs)[0]
	if run.name != legacyExeName || !strings.Contains(run.bytes, "the 2018 installer") {
		t.Errorf("ran %s (%q), want the downloaded %s", run.name, run.bytes, legacyExeName)
	}
	if want := []string{"install", "--doom", r.doom, "--release", legacyReleaseTag, "--yes"}; strings.Join(run.args, "|") != strings.Join(want, "|") {
		t.Errorf("legacy args = %q, want %q", run.args, want)
	}
	wantDoom(t, "2018 install", r.doom, legacyProxy, legacyUIDll)
	wantData(t, "2018 install", r.data)
	if rec, err := loadRecord(); err != nil || rec.DoomBuild != build2018 || rec.Executables[selfExeAsset].SHA256 == "" {
		t.Errorf("install.json after the legacy run = %+v, %v", rec, err)
	}
	if !exists(filepath.Join(r.data, "overrides", "my-overrides", "package.json")) {
		t.Error("the legacy installer's own steps did not run")
	}
	if rec, err := loadRecord(); err != nil || rec.Version != legacyReleaseTag {
		t.Errorf("record = %+v, %v", rec, err)
	}
	stable := filepath.Join(r.data, selfExeAsset)
	if got := readF(t, stable); got != string(installerBytes("v1.0.0-beta.1")) {
		t.Errorf("the stable copy = %q, want the current installer", got)
	}
	if exists(filepath.Join(r.data, legacyExeName)) {
		t.Error("the legacy installer's copy of itself was left in app data")
	}
	if rec, _ := loadExecutableRecord(r.data); len(rec) != 1 || rec[selfExeAsset] == "" {
		t.Errorf("installer-files.json = %v, want only the current installer", rec)
	}
	r.noTempLeftovers(t, "2018 install")

	// Update and reinstall take the same route, then the current installer updates itself.
	m.publish(goodRelease(t, "v1.0.0-beta.2", true))
	if err := cmdUpdate(flags{yes: true}); err != nil {
		t.Fatalf("update on 2018: %v", err)
	}
	if len(*runs) != 2 {
		t.Fatalf("update did not run the legacy installer alone: %d runs", len(*runs))
	}
	wantDoom(t, "2018 update", r.doom, legacyProxy, legacyUIDll)
	wantData(t, "2018 update", r.data)
	if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.2")) {
		t.Errorf("the current installer did not update itself: %q", got)
	}
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(r.doom, "XINPUT1_3.dll")) {
		t.Error("uninstall left the legacy backend")
	}
	wantDoom(t, "2018 uninstall", r.doom)
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil || len(*runs) != 3 {
		t.Fatalf("reinstall: %v, %d runs", err, len(*runs))
	}
	r.noTempLeftovers(t, "2018 lifecycle")
}

// On the 2018 build after an earlier installer's blind update, the April 2024 pair goes, the legacy
// installer's backup of a file that was not ours leaves DOOM for cache\installer, and that file comes
// back at uninstall.
func TestInstall2018BuildCleansAnEarlierBlindUpdate(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0-beta.1")
	writeF(t, filepath.Join(r.doom, "DOOMx64vk.exe"), fake2018)
	writeF(t, filepath.Join(r.doom, "DOOMx64.exe"), fake2018)
	writeF(t, filepath.Join(r.doom, currentProxy), ourStandIn(currentProxy))
	writeF(t, filepath.Join(r.doom, currentUIDll), ourStandIn(currentUIDll))
	writeF(t, filepath.Join(r.doom, currentProxy+".snapmap-plus-bak"), ourStandIn("an older backend"))
	writeF(t, filepath.Join(r.doom, legacyProxy), "a controller tool's XInput proxy")
	if err := saveRecord(&installRecord{Version: "v0.2.1-beta.20", DoomPath: r.doom, Files: []string{currentProxy, currentUIDll}}); err != nil {
		t.Fatal(err)
	}
	m := newMockGitHub(t)
	m.publish(legacyMockRelease(t))
	m.publish(goodRelease(t, "v1.0.0-beta.1", true))
	fakeLegacyInstaller(t, false)
	out := allOutput(t, func() {
		if err := cmdUpdate(flags{yes: true}); err != nil {
			t.Fatalf("update on 2018: %v", err)
		}
	})
	for _, want := range []string{"Cleaning the DOOM folder", currentProxy, "Cleaning up after the " + legacyReleaseTag + " installer",
		"Backups of files that were not ours"} {
		if !strings.Contains(out, want) {
			t.Errorf("output lacks %q:\n%s", want, out)
		}
	}
	wantDoom(t, "2018 update", r.doom, legacyProxy, legacyUIDll)
	wantData(t, "2018 update", r.data)
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatal(err)
	}
	wantDoom(t, "2018 uninstall", r.doom, legacyProxy)
	if readF(t, filepath.Join(r.doom, legacyProxy)) != "a controller tool's XInput proxy" {
		t.Error("uninstall did not put back the file that was there before Snapmap+")
	}
	wantData(t, "2018 uninstall", r.data)
}

// A corrupt legacy installer is never run; a failing one fails the install with its exit status.
func TestInstall2018BuildRefusesAndReportsLegacyFailures(t *testing.T) {
	t.Run("corrupt", func(t *testing.T) {
		r := newUpdateRig(t, "v1.0.0-beta.1")
		writeF(t, filepath.Join(r.doom, "DOOMx64vk.exe"), fake2018)
		writeF(t, filepath.Join(r.doom, "DOOMx64.exe"), fake2018)
		m := newMockGitHub(t)
		bad := legacyMockRelease(t)
		bad.assets[selfExeAsset].body = []byte("tampered")
		m.publish(bad)
		runs := fakeLegacyInstaller(t, false)
		err := cmdInstall(flags{yes: true, doom: r.doom})
		if err == nil || !strings.Contains(err.Error(), "integrity check failed") || len(*runs) != 0 {
			t.Fatalf("err = %v, runs = %d; want a refusal before running", err, len(*runs))
		}
		r.noTempLeftovers(t, "refused legacy installer")
	})
	t.Run("exit status", func(t *testing.T) {
		r := newUpdateRig(t, "v1.0.0-beta.1")
		writeF(t, filepath.Join(r.doom, "DOOMx64vk.exe"), fake2018)
		writeF(t, filepath.Join(r.doom, "DOOMx64.exe"), fake2018)
		m := newMockGitHub(t)
		m.publish(legacyMockRelease(t))
		fakeLegacyInstaller(t, true)
		err := cmdInstall(flags{yes: true, doom: r.doom})
		if err == nil || !strings.Contains(err.Error(), "failed (exit 3)") {
			t.Fatalf("err = %v, want the legacy installer's exit status", err)
		}
		r.noTempLeftovers(t, "failed legacy installer")
	})
}

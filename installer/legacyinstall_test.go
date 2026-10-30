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
// and does what that installer's install does to app data and the DOOM folder (copies itself into
// app data under its own name, writes the install record, places XINPUT1_3.dll and keeps the
// my-overrides starter package). fail makes it exit with status 3 instead.
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
		if err := installSelfCopy(path, filepath.Join(appDataDir(), filepath.Base(path))); err != nil {
			t.Fatalf("legacy self-copy: %v", err)
		}
		doom := args[2]
		writeF(t, filepath.Join(doom, "XINPUT1_3.dll"), "backend-legacy")
		writeF(t, filepath.Join(appDataDir(), "overrides", "my-overrides", "package.json"), `{"id":"my-overrides"}`)
		return saveRecord(&installRecord{Version: legacyReleaseTag, DoomPath: doom, Files: []string{"XINPUT1_3.dll"}})
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
	if exists(filepath.Join(r.doom, "XINPUT1_4.dll")) || readF(t, filepath.Join(r.doom, "XINPUT1_3.dll")) != "backend-legacy" {
		t.Error("the 2018 build must get only the legacy release's backend")
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
	if len(*runs) != 2 || exists(filepath.Join(r.doom, "XINPUT1_4.dll")) {
		t.Fatalf("update did not run the legacy installer alone: %d runs", len(*runs))
	}
	if got := readF(t, r.exe); got != string(installerBytes("v1.0.0-beta.2")) {
		t.Errorf("the current installer did not update itself: %q", got)
	}
	if err := cmdUninstall(flags{yes: true}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(r.doom, "XINPUT1_3.dll")) {
		t.Error("uninstall left the legacy backend")
	}
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil || len(*runs) != 3 {
		t.Fatalf("reinstall: %v, %d runs", err, len(*runs))
	}
	r.noTempLeftovers(t, "2018 lifecycle")
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

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// From the last beta to the first stable release: the beta installer replaces itself with the stable
// one and relaunches it, and the update installs the stable pair, both on the default channel and
// with --beta.
func TestBetaToStableUpdate(t *testing.T) {
	for _, f := range []flags{{yes: true}, {yes: true, beta: true}} {
		r := newUpdateRig(t, "v1.0.0-beta.26")
		m := newMockGitHub(t)
		m.publish(goodRelease(t, "v1.0.0-beta.26", true))
		if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
			t.Fatalf("install: %v", err)
		}
		m.publish(goodRelease(t, "v1.0.0", false))

		rec := stubRelaunch(t)
		captureStdout(t, func() {
			if relaunched, _ := selfUpdateFirst([]string{"update", "--yes"}); !relaunched {
				t.Error("the beta installer did not replace itself with the stable one")
			}
		})
		if rec.calls != 1 || readF(t, r.exe) != string(installerBytes("v1.0.0")) {
			t.Fatalf("relaunches %d, exe %q", rec.calls, readF(t, r.exe))
		}
		// The relaunched image does the update.
		version = "v1.0.0"
		if err := cmdUpdate(f); err != nil {
			t.Fatalf("update %+v: %v", f, err)
		}
		r.installed(t, "v1.0.0")
	}
}

// An update never moves to an older version on its own: a beta player on the default channel keeps
// the newer beta, the installer keeps its own newer image, and only --release installs the older one.
func TestUpdateNeverDowngradesWithoutRelease(t *testing.T) {
	r := newUpdateRig(t, "v1.1.0-beta.1")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0", false))
	m.publish(goodRelease(t, "v1.1.0-beta.1", true))
	if err := cmdInstall(flags{yes: true, doom: r.doom, beta: true}); err != nil {
		t.Fatalf("install --beta: %v", err)
	}
	r.installed(t, "v1.1.0-beta.1")

	out := captureStdout(t, func() {
		if err := cmdUpdate(flags{yes: true}); err != nil {
			t.Errorf("update: %v", err)
		}
	})
	r.installed(t, "v1.1.0-beta.1")
	if !strings.Contains(out, "is installed, which is newer than v1.0.0") || !strings.Contains(out, "snapmap-plus update --release v1.0.0") {
		t.Errorf("the refusal does not say what to do:\n%s", out)
	}
	if got := readF(t, r.exe); got != string(installerBytes("v1.1.0-beta.1")) {
		t.Errorf("the installer replaced itself with an older one: %q", got)
	}
	if relaunched, _ := selfUpdateFirst([]string{"update", "--yes"}); relaunched {
		t.Error("the installer relaunched an older image")
	}

	if err := cmdUpdate(flags{yes: true, release: "v1.0.0"}); err != nil {
		t.Fatalf("update --release: %v", err)
	}
	r.installed(t, "v1.0.0")
	if got := readF(t, r.exe); got != string(installerBytes("v1.1.0-beta.1")) {
		t.Errorf("--release replaced the installer with an older one: %q", got)
	}
}

// An install.json that is not JSON is renamed aside with a warning, and the update then runs as a
// fresh install instead of failing every time.
func TestUnreadableRecordIsSetAside(t *testing.T) {
	r := newUpdateRig(t, "v1.0.0")
	m := newMockGitHub(t)
	m.publish(goodRelease(t, "v1.0.0", false))
	if err := cmdInstall(flags{yes: true, doom: r.doom}); err != nil {
		t.Fatal(err)
	}
	writeF(t, statePath(r.data), "{not json")
	errOut := captureStderr(t, func() {
		if err := cmdUpdate(flags{yes: true, doom: r.doom}); err != nil {
			t.Errorf("update with an unreadable record: %v", err)
		}
	})
	if !strings.Contains(errOut, "could not be read") || !strings.Contains(errOut, "install.json.unreadable") {
		t.Errorf("no warning names the renamed record:\n%s", errOut)
	}
	if readF(t, statePath(r.data)+unreadableSuffix) != "{not json" {
		t.Error("the unreadable record was not kept")
	}
	r.installed(t, "v1.0.0")
}

// Without an install record, uninstall --doom removes the files whose contents show they are
// Snapmap+'s and leaves every other file; without --doom it says how to point it at DOOM.
func TestUninstallWithoutRecord(t *testing.T) {
	syntheticProcessGuard(t, false)
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeF(t, filepath.Join(doom, currentProxy), "a backend that loads snapmap-plus files")
	writeF(t, filepath.Join(doom, currentUIDll), "a snapmap-plus frontend")
	writeF(t, filepath.Join(doom, "dinput8.dll"), "another program")

	if err := cmdUninstall(flags{yes: true}); err == nil || !strings.Contains(err.Error(), "uninstall --doom") {
		t.Fatalf("uninstall without a record or --doom: %v", err)
	}
	if err := cmdUninstall(flags{yes: true, doom: doom}); err != nil {
		t.Fatalf("uninstall --doom: %v", err)
	}
	for _, rel := range []string{currentProxy, currentUIDll} {
		if exists(filepath.Join(doom, rel)) {
			t.Errorf("%s was left in DOOM", rel)
		}
	}
	if !exists(filepath.Join(doom, "dinput8.dll")) || !exists(filepath.Join(doom, "DOOMx64vk.exe")) {
		t.Error("uninstall removed a file that is not Snapmap+'s")
	}
	if err := cmdUninstall(flags{yes: true, doom: doom}); err == nil || !strings.Contains(err.Error(), "no Snapmap+ files were found") {
		t.Errorf("uninstall of a clean folder: %v", err)
	}
	if _, err := os.Stat(filepath.Join(tmp, "appdata", "snapmap-plus", stateName)); err == nil {
		t.Error("uninstall left an install record")
	}
}

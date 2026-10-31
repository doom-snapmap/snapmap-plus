package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestShouldSelfUpdate covers the pure update decision.
func TestShouldSelfUpdate(t *testing.T) {
	cases := []struct {
		name         string
		running, tag string
		want         bool
	}{
		{"dev build never self-updates", "dev", "v0.1.0-beta.4", false},
		{"unknown running version", "", "v0.1.0-beta.4", false},
		{"already current", "v0.1.0-beta.4", "v0.1.0-beta.4", false},
		{"no release tag", "v0.1.0-beta.4", "", false},
		{"newer release", "v0.1.0-beta.3", "v0.1.0-beta.4", true},
	}
	for _, c := range cases {
		if got := shouldSelfUpdate(c.running, c.tag); got != c.want {
			t.Errorf("%s: shouldSelfUpdate(%q,%q)=%v want %v", c.name, c.running, c.tag, got, c.want)
		}
	}
}

// replaceRig is a data folder and a separate folder holding the executable being replaced.
func replaceRig(t *testing.T) (data, path, newExe string) {
	t.Helper()
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	data = filepath.Join(tmp, "appdata", "snapmap-plus")
	path = filepath.Join(tmp, "bin", "snapmap-plus.exe")
	newExe = filepath.Join(tmp, "new", "snapmap-plus.exe")
	writeF(t, path, "OLD")
	writeF(t, newExe, "NEW")
	return data, path, newExe
}

// noOldImages fails when a replaced image is left anywhere: beside the executable or in the cache.
func noOldImages(t *testing.T, data, path string) {
	t.Helper()
	for _, dir := range []string{filepath.Dir(path), installerCacheDir(data), data} {
		entries, _ := os.ReadDir(dir)
		for _, e := range entries {
			if strings.Contains(strings.ToLower(e.Name()), ".old") || strings.HasPrefix(e.Name(), ".snapmap-plus-update-") {
				t.Errorf("left %s in %s", e.Name(), dir)
			}
		}
	}
	if exists(filepath.Join(data, "cache")) {
		t.Error("left an empty cache folder")
	}
}

// The executable is swapped for the new bytes and the replaced image, which is not running, is removed
// at once: nothing is left beside it or in the data folder.
func TestReplaceExe(t *testing.T) {
	data, path, newExe := replaceRig(t)
	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	noOldImages(t, data, path)
	if exists(statePath(data)) {
		t.Error("a replacement with nothing left to remove wrote install.json")
	}
}

// If the new exe can't be copied in, the original is untouched and no image is moved aside.
func TestReplaceExeRollbackOnMissingNew(t *testing.T) {
	data, path, _ := replaceRig(t)
	if err := replaceExe(path, filepath.Join(filepath.Dir(path), "does-not-exist.exe")); err == nil {
		t.Fatal("expected an error when the new exe is missing")
	}
	if got := readF(t, path); got != "OLD" {
		t.Errorf("after a failed replace, exe should be OLD, got %q", got)
	}
	noOldImages(t, data, path)
}

// A replaced image that cannot be removed (it is still running) waits in cache\installer, recorded in
// install.json, and the next start removes it.
func TestReplaceExeSetsARunningImageAsideForTheNextStart(t *testing.T) {
	data, path, newExe := replaceRig(t)
	ops := replaceExeOps{copyFile: copyFile, rename: os.Rename, remove: func(string) error { return errors.New("image is running") }}
	if err := replaceExeWithOps(path, newExe, ops); err != nil {
		t.Fatalf("replace: %v", err)
	}
	aside := filepath.Join(installerCacheDir(data), "snapmap-plus.exe.old")
	if readF(t, aside) != "OLD" || exists(path+".old") {
		t.Fatal("the running image was not moved into cache\\installer")
	}
	rec, err := loadStateAt(data)
	if err != nil || rec.SelfUpdate == nil || len(rec.SelfUpdate.SetAside) != 1 || rec.SelfUpdate.SetAside[0].Path != aside ||
		rec.SelfUpdate.SetAside[0].For != path {
		t.Fatalf("set-aside record = %+v, %v", rec.SelfUpdate, err)
	}
	if !strings.HasPrefix(readF(t, statePath(data)), "{\n  \"version\"") {
		t.Error("install.json must begin with its version key, which the DLLs read")
	}
	sweepSetAside()
	noOldImages(t, data, path)
	if rec, _ := loadStateAt(data); rec.SelfUpdate != nil && len(rec.SelfUpdate.SetAside) != 0 {
		t.Errorf("the swept image is still recorded: %+v", rec.SelfUpdate)
	}
}

// The sweep removes only unchanged images whose replacement exists; a recovery copy and a changed file
// stay.
func TestSweepSetAsideKeepsRecoveryAndChangedFiles(t *testing.T) {
	data, path, _ := replaceRig(t)
	cache := installerCacheDir(data)
	writeF(t, filepath.Join(cache, "a.exe.old"), "recovery")
	writeF(t, filepath.Join(cache, "b.exe.old"), "changed since")
	sumA, _ := fileSHA256(filepath.Join(cache, "a.exe.old"))
	if err := recordSetAside(filepath.Join(cache, "a.exe.old"), sumA, filepath.Join(filepath.Dir(path), "missing.exe")); err != nil {
		t.Fatal(err)
	}
	if err := recordSetAside(filepath.Join(cache, "b.exe.old"), digestOf("the bytes we moved"), path); err != nil {
		t.Fatal(err)
	}
	sweepSetAside()
	if readF(t, filepath.Join(cache, "a.exe.old")) != "recovery" || readF(t, filepath.Join(cache, "b.exe.old")) != "changed since" {
		t.Fatal("the sweep removed a recovery copy or a changed file")
	}
	rec, _ := loadStateAt(data)
	if len(rec.SelfUpdate.SetAside) != 1 || filepath.Base(rec.SelfUpdate.SetAside[0].Path) != "a.exe.old" {
		t.Errorf("set-aside record = %+v, want only the recovery copy", rec.SelfUpdate.SetAside)
	}
}

// Older recovery copies remain intact while a free aside name is selected.
func TestReplaceExePreservesEarlierRecoveryCopies(t *testing.T) {
	data, path, newExe := replaceRig(t)
	stale := filepath.Join(installerCacheDir(data), "snapmap-plus.exe.old")
	writeF(t, stale, "STALE")
	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe: %v", err)
	}
	if readF(t, path) != "NEW" || readF(t, stale) != "STALE" {
		t.Fatal("the replacement changed an existing recovery copy")
	}
	if exists(stale + "2") {
		t.Error("the replaced image was not removed")
	}
}

// Earlier releases' bookkeeping is folded into install.json and removed, and their .old images go while
// the executable they belong to exists.
func TestRetireBookkeepingFoldsAndRemoves(t *testing.T) {
	tmp := t.TempDir()
	dir := filepath.Join(tmp, "snapmap-plus")
	exe := filepath.Join(dir, selfExeAsset)
	writeF(t, exe, "current installer")
	writeF(t, filepath.Join(dir, storeToolName), "store tool")
	sum, _ := fileSHA256(exe)
	toolSum, _ := fileSHA256(filepath.Join(dir, storeToolName))
	writeF(t, filepath.Join(dir, retiredExecutableRecord), `{"snapmap-plus.exe":"`+sum+`","snapmap-plus-store.exe":"`+toolSum+`","gone.exe":"00"}`)
	writeF(t, filepath.Join(dir, retiredVersionsRecord), `{"snapmap-plus.exe":{"sha256":"`+sum+`","version":"v0.2.1-beta.20"}}`)
	writeF(t, exe+".updates.json", `{"current_sha256":"`+sum+`","backups":{"snapmap-plus.exe.old":"aa"}}`)
	writeF(t, exe+".old", "replaced installer")
	writeF(t, exe+".old2", "replaced installer 2")
	writeF(t, exe+".old-notes", "keep")
	writeF(t, exe+".old200", "keep")
	removed := retireBookkeeping(dir, filepath.Join(tmp, "elsewhere", selfExeAsset))
	for _, gone := range []string{retiredExecutableRecord, retiredVersionsRecord, selfExeAsset + ".updates.json", selfExeAsset + ".old", selfExeAsset + ".old2"} {
		if exists(filepath.Join(dir, gone)) {
			t.Errorf("%s was not removed", gone)
		}
	}
	for _, kept := range []string{".old-notes", ".old200"} {
		if readF(t, exe+kept) != "keep" {
			t.Errorf("an unrelated %s was touched", kept)
		}
	}
	if len(removed) != 5 {
		t.Errorf("reported %d removals: %q", len(removed), removed)
	}
	rec, err := loadStateAt(dir)
	if err != nil || rec.Executables[selfExeAsset].SHA256 != sum || rec.Executables[selfExeAsset].Version != "v0.2.1-beta.20" ||
		rec.Executables[storeToolName].SHA256 != toolSum || len(rec.Executables) != 2 {
		t.Fatalf("folded executables = %+v, %v", rec.Executables, err)
	}
	if rec.DoomPath != "" {
		t.Error("folding created an installation record")
	}

	// Without the executable it belongs to, a .old is the only copy left and stays.
	os.Remove(exe)
	writeF(t, exe+".old", "the only copy")
	retireBookkeeping(dir, "")
	if readF(t, exe+".old") != "the only copy" {
		t.Error("the only remaining installer image was removed")
	}
}

func TestReplaceExePartialStageFailureKeepsOriginal(t *testing.T) {
	data, path, fresh := replaceRig(t)
	ops := replaceExeOps{rename: os.Rename, remove: os.Remove, copyFile: func(_, dst string) error {
		writeF(t, dst, "partial")
		return errors.New("injected disk write failure")
	}}
	if err := replaceExeWithOps(path, fresh, ops); err == nil {
		t.Fatal("partial staging write was accepted")
	}
	if got := readF(t, path); got != "OLD" {
		t.Fatal("staging failure changed the old executable")
	}
	noOldImages(t, data, path)
}

func TestReplaceExeInstallAndRollbackFailures(t *testing.T) {
	for _, rollbackFails := range []bool{false, true} {
		t.Run(fmtBool(rollbackFails), func(t *testing.T) {
			data, path, fresh := replaceRig(t)
			aside := filepath.Join(installerCacheDir(data), "snapmap-plus.exe.old")
			ops := replaceExeOps{copyFile: copyFile, remove: os.Remove, rename: func(src, dst string) error {
				if strings.HasPrefix(filepath.Base(src), ".snapmap-plus-update-") {
					return errors.New("injected installation rename failure")
				}
				if src == aside && rollbackFails {
					return errors.New("injected rollback failure")
				}
				return os.Rename(src, dst)
			}}
			err := replaceExeWithOps(path, fresh, ops)
			if err == nil {
				t.Fatal("failed publication returned success")
			}
			if rollbackFails {
				if !strings.Contains(err.Error(), "rollback failed") || !strings.Contains(err.Error(), aside) {
					t.Fatalf("missing recovery details: %v", err)
				}
				if got := readF(t, aside); got != "OLD" {
					t.Fatal("rollback failure lost the recoverable image")
				}
				sweepSetAside()
				writeF(t, path, "OLD") // A later manual recovery must not revive a deletion claim.
				sweepSetAside()
				startupCleanup()
				if readF(t, aside) != "OLD" {
					t.Fatal("startup cleanup discarded the rollback recovery image")
				}
			} else if got := readF(t, path); got != "OLD" {
				t.Fatal("successful rollback did not restore the original")
			} else {
				noOldImages(t, data, path)
			}
		})
	}
}

func fmtBool(value bool) string {
	if value {
		return "rollback-fails"
	}
	return "rollback-restores"
}

// sha256("abc"), the standard test vector.
const abcSHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

// TestAssetMatchesFile: the on-disk-already-current check that stops a session from re-downloading and
// re-replacing an exe that a previous run already updated.
func TestAssetMatchesFile(t *testing.T) {
	tmp := t.TempDir()
	p := filepath.Join(tmp, "f")
	writeF(t, p, "abc")
	if !assetMatchesFile(&ghAsset{Digest: "sha256:" + abcSHA256}, p) {
		t.Error("matching digest not recognized")
	}
	if assetMatchesFile(&ghAsset{Digest: "sha256:" + abcSHA256}, filepath.Join(tmp, "missing")) {
		t.Error("missing file reported as matching")
	}
	if assetMatchesFile(&ghAsset{Digest: ""}, p) {
		t.Error("empty digest must not match (must fall through to the download path)")
	}
	if assetMatchesFile(&ghAsset{Digest: "sha256:deadbeef"}, p) {
		t.Error("wrong digest reported as matching")
	}
}

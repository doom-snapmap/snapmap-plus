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

// TestReplaceExe: the running exe is swapped for the new bytes, and the old bytes are preserved at <exe>.old
// (to be cleaned on the next launch).
func TestReplaceExe(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snapmap-plus.exe")
	writeF(t, path, "OLD")
	newExe := filepath.Join(tmp, "new", "snapmap-plus.exe")
	writeF(t, newExe, "NEW")

	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, path+".old"); got != "OLD" {
		t.Errorf("expected the old exe preserved at .old, got %q", got)
	}
}

// TestReplaceExeRollbackOnMissingNew: if the new exe can't be copied in, the original is restored and no .old
// is left behind -- the caller is never left without a working exe.
func TestReplaceExeRollbackOnMissingNew(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snapmap-plus.exe")
	writeF(t, path, "OLD")
	missing := filepath.Join(tmp, "does-not-exist.exe")

	if err := replaceExe(path, missing); err == nil {
		t.Fatal("expected an error when the new exe is missing")
	}
	if got := readF(t, path); got != "OLD" {
		t.Errorf("after a failed replace, exe should be rolled back to OLD, got %q", got)
	}
	if exists(path + ".old") {
		t.Error("a failed replace should leave no .old behind")
	}
}

// Older recovery copies remain intact while a free backup name is selected.
func TestReplaceExePreservesEarlierBackup(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snapmap-plus.exe")
	writeF(t, path, "OLD")
	writeF(t, path+".old", "STALE")
	newExe := filepath.Join(tmp, "new", "snapmap-plus.exe")
	writeF(t, newExe, "NEW")

	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, path+".old"); got != "STALE" {
		t.Errorf(".old recovery copy changed: %q", got)
	}
	if got := readF(t, path+".old2"); got != "OLD" {
		t.Errorf(".old2 should hold the just-replaced exe, got %q", got)
	}
}

// Startup cleanup requires recorded hashes for both the current image and backups.
func TestRemoveOldLeftoversSweepsRecordedSuffixes(t *testing.T) {
	tmp := t.TempDir()
	exe := filepath.Join(tmp, "snapmap-plus.exe")
	writeF(t, exe, "CURRENT")
	writeF(t, exe+".old", "a")
	writeF(t, exe+".old2", "b")
	writeF(t, exe+".old3", "c")
	writeF(t, filepath.Join(tmp, "other.dat"), "keep")
	writeF(t, exe+".old-notes", "keep")
	writeF(t, exe+".old200", "keep")
	writeF(t, exe+".old4", "unrecorded")
	for _, suffix := range []string{".old", ".old2", ".old3"} {
		if err := recordUpdateBackup(exe, exe+suffix); err != nil {
			t.Fatal(err)
		}
	}

	removeOldLeftovers(exe)

	for _, gone := range []string{exe + ".old", exe + ".old2", exe + ".old3"} {
		if exists(gone) {
			t.Errorf("%s still present, want removed", filepath.Base(gone))
		}
	}
	if got := readF(t, exe); got != "CURRENT" {
		t.Errorf("exe touched by the sweep: %q", got)
	}
	if got := readF(t, filepath.Join(tmp, "other.dat")); got != "keep" {
		t.Errorf("unrelated file touched by the sweep: %q", got)
	}
	for _, suffix := range []string{".old-notes", ".old200"} {
		if got := readF(t, exe+suffix); got != "keep" {
			t.Fatalf("unrelated suffix was removed: %s", suffix)
		}
	}
	if readF(t, exe+".old4") != "unrecorded" {
		t.Fatal("unrecorded backup filename was removed")
	}
}

func TestReplaceExePartialStageFailureKeepsOriginal(t *testing.T) {
	dir := t.TempDir()
	path, fresh := filepath.Join(dir, "snapmap-plus.exe"), filepath.Join(dir, "new.exe")
	writeF(t, path, "OLD")
	writeF(t, fresh, "NEW")
	ops := replaceExeOps{rename: os.Rename, copyFile: func(_, dst string) error {
		writeF(t, dst, "partial")
		return errors.New("injected disk write failure")
	}}
	if err := replaceExeWithOps(path, fresh, ops); err == nil {
		t.Fatal("partial staging write was accepted")
	}
	if got := readF(t, path); got != "OLD" || exists(path+".old") {
		t.Fatal("staging failure changed the old executable")
	}
	matches, _ := filepath.Glob(filepath.Join(dir, ".snapmap-plus-update-*"))
	if len(matches) != 0 {
		t.Fatalf("partial stage was not removed: %v", matches)
	}
}

func TestReplaceExeInstallAndRollbackFailures(t *testing.T) {
	for _, rollbackFails := range []bool{false, true} {
		t.Run(fmtBool(rollbackFails), func(t *testing.T) {
			dir := t.TempDir()
			path, fresh := filepath.Join(dir, "snapmap-plus.exe"), filepath.Join(dir, "new.exe")
			writeF(t, path, "OLD")
			writeF(t, fresh, "NEW")
			if rollbackFails {
				digest, _ := fileSHA256(path)
				if err := saveJSONFile(path+".updates.json", &updateRecord{Current: digest,
					Backups: map[string]string{filepath.Base(path) + ".old": digest}}); err != nil {
					t.Fatal(err)
				}
			}
			ops := replaceExeOps{copyFile: copyFile, rename: func(src, dst string) error {
				if strings.HasPrefix(filepath.Base(src), ".snapmap-plus-update-") {
					return errors.New("injected installation rename failure")
				}
				if src == path+".old" && rollbackFails {
					return errors.New("injected rollback failure")
				}
				return os.Rename(src, dst)
			}}
			err := replaceExeWithOps(path, fresh, ops)
			if err == nil {
				t.Fatal("failed publication returned success")
			}
			if rollbackFails {
				if !strings.Contains(err.Error(), "rollback failed") || !strings.Contains(err.Error(), path+".old") {
					t.Fatalf("missing recovery details: %v", err)
				}
				if got := readF(t, path+".old"); got != "OLD" {
					t.Fatal("rollback failure lost the recoverable image")
				}
				removeOldLeftovers(path)
				writeF(t, path, "OLD") // A later manual recovery must not revive a stale deletion claim.
				removeOldLeftovers(path)
				if readF(t, path+".old") != "OLD" {
					t.Fatal("startup cleanup discarded the rollback recovery image")
				}
			} else if got := readF(t, path); got != "OLD" || exists(path+".old") {
				t.Fatal("successful rollback did not restore the original")
			}
		})
	}
}

func TestUpdateCleanupPreservesModifiedImagesAndBackupBytes(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "installer.exe")
	writeF(t, exe, "current")
	writeF(t, exe+".old", "backup")
	if err := recordUpdateBackup(exe, exe+".old"); err != nil {
		t.Fatal(err)
	}
	writeF(t, exe, "user changed current")
	removeOldLeftovers(exe)
	if readF(t, exe+".old") != "backup" {
		t.Fatal("changed current image lost its recovery backup")
	}
	writeF(t, exe, "current")
	writeF(t, exe+".old", "user changed backup")
	removeOldLeftovers(exe)
	if readF(t, exe+".old") != "user changed backup" {
		t.Fatal("changed backup was deleted")
	}
}

func TestReplaceExeRefusesAnUnrecognizedUpdateRecord(t *testing.T) {
	dir := t.TempDir()
	exe, fresh := filepath.Join(dir, "installer.exe"), filepath.Join(dir, "new.exe")
	writeF(t, exe, "current")
	writeF(t, fresh, "new")
	writeF(t, exe+".updates.json", `{"personal":"keep"}`)
	if err := replaceExe(exe, fresh); err == nil {
		t.Fatal("replacement overwrote an unrecognized adjacent record")
	}
	if readF(t, exe) != "current" || readF(t, exe+".updates.json") != `{"personal":"keep"}` {
		t.Fatal("unrecognized record refusal changed original files")
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

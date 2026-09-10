package main

import (
	"path/filepath"
	"testing"
)

func TestCleanupDeletesOnlyUnchangedOwnedExecutables(t *testing.T) {
	dir, running := t.TempDir(), filepath.Join(t.TempDir(), "running.exe")
	writeF(t, running, "installer")
	owned, modified := filepath.Join(dir, "owned.exe"), filepath.Join(dir, "modified.exe")
	for _, dst := range []string{owned, modified} {
		if err := installSelfCopy(running, dst); err != nil {
			t.Fatal(err)
		}
	}
	writeF(t, modified, "user replacement")
	writeF(t, filepath.Join(dir, "personal.exe"), "personal")
	cleanupOwnedExecutables(dir, running)
	if exists(owned) {
		t.Fatal("unchanged installer copy was not removed")
	}
	if readF(t, modified) != "user replacement" || readF(t, filepath.Join(dir, "personal.exe")) != "personal" {
		t.Fatal("cleanup touched an unowned executable")
	}
}

func TestSelfCopyRefusesUnownedCollision(t *testing.T) {
	dir := t.TempDir()
	src, dst := filepath.Join(t.TempDir(), "installer.exe"), filepath.Join(dir, "personal.exe")
	writeF(t, src, "installer")
	writeF(t, dst, "personal")
	if err := installSelfCopy(src, dst); err == nil {
		t.Fatal("unowned destination was replaced")
	}
	if readF(t, dst) != "personal" {
		t.Fatal("unowned destination bytes changed")
	}
}

func TestOwnedSelfCopyCanUpdateAndBeRemoved(t *testing.T) {
	src, dst := filepath.Join(t.TempDir(), "installer.exe"), filepath.Join(t.TempDir(), "installed.exe")
	writeF(t, src, "version one")
	if err := installSelfCopy(src, dst); err != nil {
		t.Fatal(err)
	}
	writeF(t, src, "version two")
	if err := installSelfCopy(src, dst); err != nil {
		t.Fatal(err)
	}
	if readF(t, dst) != "version two" {
		t.Fatal("owned copy did not update")
	}
	cleanupOwnedExecutables(filepath.Dir(dst), src)
	if exists(dst) {
		t.Fatal("updated ownership digest was not retained")
	}
}

func TestCleanupPreservesRunningImageAndRejectsRecordTraversal(t *testing.T) {
	parent := t.TempDir()
	dir := filepath.Join(parent, "appdata")
	running, outside := filepath.Join(dir, "running.exe"), filepath.Join(parent, "outside.exe")
	writeF(t, running, "running")
	writeF(t, outside, "outside")
	digest, _ := fileSHA256(running)
	otherDigest, _ := fileSHA256(outside)
	if err := saveExecutableRecord(dir, executableRecord{"running.exe": digest, "../outside.exe": otherDigest}); err != nil {
		t.Fatal(err)
	}
	cleanupOwnedExecutables(dir, running)
	if readF(t, running) != "running" || readF(t, outside) != "outside" {
		t.Fatal("cleanup removed the running image or escaped its directory")
	}
	cleanupOwnedExecutables(dir, "")
	if !exists(running) {
		t.Fatal("unknown running image allowed executable removal")
	}
}

func TestExecutableRecordReplacementKeepsOnlyTheNewManifest(t *testing.T) {
	dir := t.TempDir()
	for _, rec := range []executableRecord{{"old.exe": "old hash"}, {"new.exe": "new hash"}} {
		if err := saveExecutableRecord(dir, rec); err != nil {
			t.Fatal(err)
		}
	}
	got, err := loadExecutableRecord(dir)
	if err != nil || len(got) != 1 || got["new.exe"] != "new hash" {
		t.Fatalf("manifest replacement failed: %#v, %v", got, err)
	}
}

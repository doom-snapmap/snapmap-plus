package main

import (
	"path/filepath"
	"testing"
)

func TestMigrationKeepsConflictingMetadataAndUnknownFiles(t *testing.T) {
	newDataDirs(t)
	old, dst := oldAppDataDir(), appDataDir()
	writeF(t, filepath.Join(old, "install.json"), "old record")
	writeF(t, filepath.Join(dst, "install.json"), "new record")
	writeF(t, filepath.Join(old, "token"), "saved token")
	writeF(t, filepath.Join(old, "personal.txt"), "keep")
	migrateUserData()
	if got := readF(t, filepath.Join(old, "install.json")); got != "old record" {
		t.Fatal("lost the old install record")
	}
	if got := readF(t, filepath.Join(dst, "install.json")); got != "new record" {
		t.Fatal("replaced the current install record")
	}
	if got := readF(t, filepath.Join(dst, "token")); got != "saved token" {
		t.Fatal("token was not copied")
	}
	if got := readF(t, filepath.Join(old, "personal.txt")); got != "keep" {
		t.Fatal("unknown legacy file was removed")
	}
}

func TestMigrationKeepsMetadataWhenDestinationCannotBeWritten(t *testing.T) {
	newDataDirs(t)
	writeF(t, filepath.Join(oldAppDataDir(), "token"), "saved token")
	mkdirAll(t, filepath.Join(appDataDir(), "token"))
	migrateUserData()
	if got := readF(t, filepath.Join(oldAppDataDir(), "token")); got != "saved token" {
		t.Fatal("failed metadata copy discarded its source")
	}
}

func TestMigrationRemovesOnlyVerifiedMetadata(t *testing.T) {
	newDataDirs(t)
	writeF(t, filepath.Join(oldAppDataDir(), "token"), "saved token")
	migrateUserData()
	if exists(oldAppDataDir()) {
		t.Fatal("verified, empty legacy metadata directory remains")
	}
	if got := readF(t, filepath.Join(appDataDir(), "token")); got != "saved token" {
		t.Fatal("token was not retained")
	}
}

func TestMirrorRejectsReadErrorsAndDifferentEntryTypes(t *testing.T) {
	src, dst := filepath.Join(t.TempDir(), "missing"), t.TempDir()
	if fullyMirrored(src, dst) {
		t.Fatal("missing source was accepted as a complete walk")
	}
	if _, err := copyTreeMissing(src, dst); err == nil {
		t.Fatal("source enumeration failure was ignored")
	}
	writeF(t, filepath.Join(src, "file"), "data")
	mkdirAll(t, filepath.Join(dst, "file"))
	if fullyMirrored(src, dst) {
		t.Fatal("directory at a file path was accepted")
	}
}

func TestCopyFailureRetainsTheEntireLegacyTree(t *testing.T) {
	newDataDirs(t)
	src, dst := oldUserContentDir(), appDataDir()
	writeF(t, filepath.Join(src, "blocked", "file"), "uncopied")
	writeF(t, filepath.Join(src, "other"), "copied")
	writeF(t, filepath.Join(dst, "blocked"), "existing file")
	migrateUserData()
	if got := readF(t, filepath.Join(src, "blocked", "file")); got != "uncopied" {
		t.Fatal("copy failure lost its source")
	}
	if got := readF(t, filepath.Join(src, "other")); got != "copied" {
		t.Fatal("incomplete migration retired the source tree")
	}
}

func TestRetirementRechecksFilesBeforeRemoval(t *testing.T) {
	src, dst := t.TempDir(), t.TempDir()
	writeF(t, filepath.Join(src, "file"), "before")
	writeF(t, filepath.Join(dst, "file"), "before")
	if !fullyMirrored(src, dst) {
		t.Fatal("identical files did not verify")
	}
	writeF(t, filepath.Join(src, "file"), "after")
	if err := removeMirroredTree(src, dst); err == nil {
		t.Fatal("retirement accepted changed source bytes")
	}
	if got := readF(t, filepath.Join(src, "file")); got != "after" {
		t.Fatal("changed source was removed")
	}
}

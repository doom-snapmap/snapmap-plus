//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMigrationRefusesLinkedDestinations(t *testing.T) {
	src, dst, outside := t.TempDir(), t.TempDir(), t.TempDir()
	writeF(t, filepath.Join(src, "linked", "file"), "source")
	if err := os.Symlink(outside, filepath.Join(dst, "linked")); err != nil {
		t.Fatal(err)
	}
	if _, err := copyTreeMissing(src, dst); err == nil {
		t.Fatal("migration followed a directory link")
	}
	if exists(filepath.Join(outside, "file")) || fullyMirrored(src, dst) {
		t.Fatal("linked destination was written or accepted")
	}
}

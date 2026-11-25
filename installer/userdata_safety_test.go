package main

import (
	"path/filepath"
	"testing"
)

func TestCopyMissingFilePreservesExistingFile(t *testing.T) {
	dir := t.TempDir()
	src, dst := filepath.Join(dir, "src"), filepath.Join(dir, "dst")
	writeF(t, src, "source")
	writeF(t, dst, "keep")
	if err := copyMissingFile(src, dst); err != nil {
		t.Fatal(err)
	}
	if identicalFiles(src, dst) || readF(t, dst) != "keep" {
		t.Fatal("replaced a different file")
	}
}

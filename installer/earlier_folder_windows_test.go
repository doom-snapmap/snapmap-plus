package main

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
)

func TestRenameEarlierFolderAfterChildIsReleased(t *testing.T) {
	root := t.TempDir()
	src, dst := filepath.Join(root, ".preparing-1"), filepath.Join(root, "retained")
	child := filepath.Join(src, "converted", "author.lua")
	writeF(t, child, "original bytes")
	held, err := os.Open(child)
	if err != nil {
		t.Fatal(err)
	}
	err = os.Rename(src, dst)
	if !errors.Is(err, fs.ErrPermission) && !errors.Is(err, syscall.Errno(32)) {
		held.Close()
		t.Fatalf("expected a Windows sharing or access error while the child was open, got %v", err)
	}
	released := make(chan error, 1)
	time.AfterFunc(100*time.Millisecond, func() { released <- held.Close() })
	err = renameEarlierFolder(src, dst)
	if closeErr := <-released; closeErr != nil {
		t.Fatal(closeErr)
	}
	if err != nil || readF(t, filepath.Join(dst, "converted", "author.lua")) != "original bytes" {
		t.Fatalf("rename after release: %v", err)
	}
	writeF(t, filepath.Join(src, "other.lua"), "different bytes")
	if err := renameEarlierFolder(src, dst); err == nil || readF(t, filepath.Join(src, "other.lua")) != "different bytes" {
		t.Fatal("an existing conversion record was replaced")
	}
}

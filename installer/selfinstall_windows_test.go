//go:build windows

package main

import (
	"path/filepath"
	"syscall"
	"testing"
)

func TestLockedExecutableRecordRetainsThePreviousManifest(t *testing.T) {
	dir := t.TempDir()
	if err := saveExecutableRecord(dir, executableRecord{"old.exe": "old hash"}); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, executableRecordName)
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(path), syscall.GENERIC_READ,
		syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.CloseHandle(h)
	if err := saveExecutableRecord(dir, executableRecord{"new.exe": "new hash"}); err == nil {
		t.Fatal("locked manifest replacement unexpectedly succeeded")
	}
	got, err := loadExecutableRecord(dir)
	if err != nil || len(got) != 1 || got["old.exe"] != "old hash" {
		t.Fatalf("failed replacement changed the original manifest: %#v, %v", got, err)
	}
}

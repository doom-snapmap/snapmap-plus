//go:build windows

package main

import (
	"path/filepath"
	"syscall"
	"testing"
)

// Simulate a running .old image with a handle that denies delete sharing.
// Replacement must use a free numbered aside name.
func TestReplaceExeLockedOldFallsBack(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snapmap-plus.exe")
	writeF(t, path, "CURRENT")
	writeF(t, path+".old", "RUNNING-OLD-IMAGE")
	newExe := filepath.Join(tmp, "new", "snapmap-plus.exe")
	writeF(t, newExe, "NEW")

	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(path+".old"),
		syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatalf("locking .old: %v", err)
	}
	defer syscall.CloseHandle(h)

	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe with a locked .old: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, path+".old"); got != "RUNNING-OLD-IMAGE" {
		t.Errorf("the locked .old must be left alone, got %q", got)
	}
	if got := readF(t, path+".old2"); got != "CURRENT" {
		t.Errorf("the replaced exe should be aside at .old2, got %q", got)
	}
}

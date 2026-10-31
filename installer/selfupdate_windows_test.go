//go:build windows

package main

import (
	"path/filepath"
	"syscall"
	"testing"
)

// lockFile opens path with a handle that denies delete sharing, as a running image does.
func lockFile(t *testing.T, path string) {
	t.Helper()
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(path),
		syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatalf("locking %s: %v", path, err)
	}
	t.Cleanup(func() { syscall.CloseHandle(h) })
}

// A locked recovery copy keeps its name; the replacement takes the next free one.
func TestReplaceExeLockedOldFallsBack(t *testing.T) {
	data, path, newExe := replaceRig(t)
	held := filepath.Join(installerCacheDir(data), "snapmap-plus.exe.old")
	writeF(t, held, "RUNNING-OLD-IMAGE")
	lockFile(t, held)
	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe with a locked .old: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, held); got != "RUNNING-OLD-IMAGE" {
		t.Errorf("the locked .old must be left alone, got %q", got)
	}
}

// A set-aside image that is still locked at the next start stays recorded and is retried.
func TestSweepRetriesALockedImage(t *testing.T) {
	data, path, _ := replaceRig(t)
	aside := filepath.Join(installerCacheDir(data), "snapmap-plus.exe.old")
	writeF(t, aside, "PARENT-STILL-RUNNING")
	sum, _ := fileSHA256(aside)
	if err := recordSetAside(aside, sum, path); err != nil {
		t.Fatal(err)
	}
	lockFile(t, aside)
	sweepSetAside()
	if readF(t, aside) != "PARENT-STILL-RUNNING" {
		t.Fatal("a locked image changed")
	}
	if rec, _ := loadStateAt(data); rec.SelfUpdate == nil || len(rec.SelfUpdate.SetAside) != 1 {
		t.Fatalf("the locked image is no longer recorded: %+v", rec.SelfUpdate)
	}
}

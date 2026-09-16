//go:build windows

package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// makeJunction creates a directory junction, which needs no symlink privilege.
func makeJunction(t *testing.T, link, target string) {
	t.Helper()
	mkdirAll(t, link)
	substitute, err := syscall.UTF16FromString(`\??\` + target)
	if err != nil {
		t.Fatal(err)
	}
	printName, err := syscall.UTF16FromString(target)
	if err != nil {
		t.Fatal(err)
	}
	data := make([]byte, 16+2*(len(substitute)+len(printName)))
	binary.LittleEndian.PutUint32(data, 0xa0000003) // IO_REPARSE_TAG_MOUNT_POINT
	binary.LittleEndian.PutUint16(data[4:], uint16(len(data)-8))
	binary.LittleEndian.PutUint16(data[10:], uint16(2*(len(substitute)-1)))
	binary.LittleEndian.PutUint16(data[12:], uint16(2*len(substitute)))
	binary.LittleEndian.PutUint16(data[14:], uint16(2*(len(printName)-1)))
	for i, ch := range append(substitute, printName...) {
		binary.LittleEndian.PutUint16(data[16+2*i:], ch)
	}
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(link), syscall.GENERIC_WRITE,
		0, nil, syscall.OPEN_EXISTING, syscall.FILE_FLAG_OPEN_REPARSE_POINT|syscall.FILE_FLAG_BACKUP_SEMANTICS, 0)
	if err != nil {
		t.Fatal(err)
	}
	var returned uint32
	err = syscall.DeviceIoControl(h, 0x000900a4, &data[0], uint32(len(data)), nil, 0, &returned, nil)
	syscall.CloseHandle(h)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Remove(link) })
}

func TestOverrideMigrationRefusesLinks(t *testing.T) {
	lib := newOverrideLibrary(t)
	outside := t.TempDir()
	writeF(t, filepath.Join(outside, "decls", "entitydef", "outside.decl"), "outside")
	lib.write(map[string]string{
		"linked/package.json":            legacyMarker,
		"linked/decls/entitydef/a.decl":  "{}",
		"healthy/package.json":           legacyMarker,
		"healthy/decls/entitydef/h.decl": "{}",
	})
	makeJunction(t, lib.path("linked/decls/external"), outside)
	makeJunction(t, lib.path("root-link"), outside)
	report := lib.migrate()
	if len(report.Converted) != 1 || report.Converted[0].Package != "healthy" || len(report.Rejected) != 1 ||
		!strings.Contains(report.Rejected[0], "link") {
		t.Fatalf("report: %+v", report)
	}
	if lib.read("linked/decls/entitydef/a.decl") != "{}" || readF(t, filepath.Join(outside, "decls", "entitydef", "outside.decl")) != "outside" {
		t.Fatal("linked package or link target changed")
	}
	if !strings.Contains(strings.Join(report.Notes, "\n"), "root-link is a link") {
		t.Fatalf("root link not reported: %v", report.Notes)
	}
	info, err := os.Lstat(lib.path("root-link"))
	if err != nil || info.Mode()&os.ModeIrregular == 0 && info.Mode()&os.ModeSymlink == 0 {
		t.Fatalf("root link was changed: %v", err)
	}
}

func TestRenameNoReplaceNeverOverwrites(t *testing.T) {
	dir := t.TempDir()
	writeF(t, filepath.Join(dir, "a"), "source")
	writeF(t, filepath.Join(dir, "b"), "existing")
	if err := renameNoReplace(filepath.Join(dir, "a"), filepath.Join(dir, "b")); err == nil {
		t.Fatal("existing file replaced")
	}
	if readF(t, filepath.Join(dir, "a")) != "source" || readF(t, filepath.Join(dir, "b")) != "existing" {
		t.Fatal("a copy was lost")
	}
	long := filepath.Join(dir, strings.Repeat("long-folder-name\\", 20))
	mkdirAll(t, long)
	if err := renameNoReplace(filepath.Join(dir, "a"), filepath.Join(long, "moved")); err != nil {
		t.Fatalf("long destination: %v", err)
	}
}

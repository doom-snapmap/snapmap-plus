//go:build windows

package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

// Directory junctions do not require the symlink privilege on Windows.
func TestMigrationRefusesDirectoryJunction(t *testing.T) {
	src, dst, outside := t.TempDir(), t.TempDir(), t.TempDir()
	writeF(t, filepath.Join(src, "linked", "file"), "source")
	link := filepath.Join(dst, "linked")
	mkdirAll(t, link)
	substitute, err := syscall.UTF16FromString(`\??\` + outside)
	if err != nil {
		t.Fatal(err)
	}
	printName, err := syscall.UTF16FromString(outside)
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
	defer os.Remove(link)
	if _, err := copyTreeMissing(src, dst); err == nil {
		t.Fatal("migration followed a destination junction")
	}
	if exists(filepath.Join(outside, "file")) || fullyMirrored(src, dst) {
		t.Fatal("junction destination was written or accepted")
	}
}

func TestMigrationKeepsUnreadableSource(t *testing.T) {
	newDataDirs(t)
	path := filepath.Join(oldUserContentDir(), "prefabs", "locked.json")
	writeF(t, path, "saved prefab")
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(path),
		syscall.GENERIC_READ, 0, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	migrateUserData()
	if fullyMirrored(oldUserContentDir(), appDataDir()) {
		t.Error("unreadable source was accepted as mirrored")
	}
	syscall.CloseHandle(h)
	if got := readF(t, path); got != "saved prefab" {
		t.Fatal("read failure lost the source")
	}
}

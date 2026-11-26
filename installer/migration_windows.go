//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

// A cache entry may share a read-only file with another composition. Unlink only this name.
func removeMigrationFile(path string) error {
	stat, err := os.Stat(path)
	if err != nil {
		return err
	}
	if stat.Mode().Perm()&0200 != 0 {
		return os.Remove(path)
	}
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	h, err := syscall.CreateFile(name, 0x10000, syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE|syscall.FILE_SHARE_DELETE, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		return err
	}
	defer syscall.CloseHandle(h)
	flags := uint32(1 | 2 | 16)
	ok, _, e := syscall.NewLazyDLL("kernel32.dll").NewProc("SetFileInformationByHandle").Call(uintptr(h), 21, uintptr(unsafe.Pointer(&flags)), unsafe.Sizeof(flags))
	if ok == 0 {
		return e
	}
	return nil
}

func moveMigrationFile(src, dst string) error { return publishNativeCopy(src, dst) }

func lockMigration(data string) (func(), error) {
	dir := installerCacheDir(data)
	if !plainPath(dir) {
		return nil, fmt.Errorf("linked installer cache %s", dir)
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	name, err := syscall.UTF16PtrFromString(filepath.Join(dir, "content-migration.lock"))
	if err != nil {
		return nil, err
	}
	// A nonshared, delete-on-close handle also releases the lock if the process exits early.
	h, err := syscall.CreateFile(name, syscall.GENERIC_WRITE|0x10000, 0, nil, syscall.CREATE_NEW, 0x04000000 /* FILE_FLAG_DELETE_ON_CLOSE */, 0)
	if err != nil {
		return nil, fmt.Errorf("could not reserve the content migration: %w", err)
	}
	return func() { syscall.CloseHandle(h) }, nil
}

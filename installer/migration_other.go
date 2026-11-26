//go:build !windows

package main

import (
	"fmt"
	"os"
	"syscall"
)

func removeMigrationFile(path string) error { return os.Remove(path) }

func moveMigrationFile(src, dst string) error {
	if err := os.Link(src, dst); err != nil {
		return err
	}
	return os.Remove(src)
}

func lockMigration(data string) (func(), error) {
	dir := installerCacheDir(data)
	if !plainPath(dir) {
		return nil, fmt.Errorf("linked installer cache %s", dir)
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	dirHandle, err := os.Open(dir)
	if err != nil {
		return nil, err
	}
	if err = syscall.Flock(int(dirHandle.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		dirHandle.Close()
		return nil, err
	}
	return func() { dirHandle.Close() }, nil
}

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Player content lives in writable local app data, outside the game installation.

// userContentSubdirs is the content tree the installer scaffolds and the backend reads; it mirrors
// sh_user_dirs_subfolders in src/backend/user_dirs.c. A strings folder a previous install left stays as it is.
var userContentSubdirs = []string{"prefabs"}

// ensureUserDataTree creates missing player-content directories.
func ensureUserDataTree() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	for _, sub := range userContentSubdirs {
		os.MkdirAll(filepath.Join(dir, sub), 0o755)
	}
}

// plainPath refuses links in any existing component before copying or deleting.
func plainPath(path string) bool {
	for path = filepath.Clean(path); ; path = filepath.Dir(path) {
		info, err := os.Lstat(path)
		if err != nil && !os.IsNotExist(err) {
			return false
		}
		if err == nil && info.Mode()&(os.ModeSymlink|os.ModeIrregular) != 0 {
			return false
		}
		if filepath.Dir(path) == path {
			return true
		}
	}
}

func identicalFiles(src, dst string) bool {
	if !plainPath(src) || !plainPath(dst) {
		return false
	}
	a, ea := os.Lstat(src)
	b, eb := os.Lstat(dst)
	if ea != nil || eb != nil || !a.Mode().IsRegular() || !b.Mode().IsRegular() || a.Size() != b.Size() {
		return false
	}
	x, ea := fileSHA256(src)
	y, eb := fileSHA256(dst)
	return ea == nil && eb == nil && x == y
}

func copyMissingFile(src, dst string) error {
	if !plainPath(src) || !plainPath(dst) {
		return fmt.Errorf("cannot copy through linked paths: %s", src)
	}
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	info, err := in.Stat()
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("cannot copy a non-regular file: %s", src)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, info.Mode().Perm())
	if os.IsExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(out, in)
	err = errors.Join(copyErr, out.Sync(), out.Close())
	if err != nil {
		os.Remove(dst)
	}
	return err
}

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Player content lives in writable local app data, outside the game installation.
// Keep userContentSubdirs aligned with the backend startup folders in dllmain.c.

// userContentSubdirs is the content tree the installer scaffolds and the backend reads. Mirrors dllmain.c's subs[].
var userContentSubdirs = []string{"strings", "overrides", "prefabs"}

// oldUserContentDir is the legacy home-root data folder; empty if USERPROFILE is unset.
func oldUserContentDir() string {
	base := os.Getenv("USERPROFILE")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "snaphak")
}

// oldAppDataDir is the pre-rename app-data folder, %LOCALAPPDATA%\open-snaphak (install record + token).
func oldAppDataDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "open-snaphak")
}

func oldRecordPath() string {
	if d := oldAppDataDir(); d != "" {
		return filepath.Join(d, "install.json")
	}
	return ""
}

func oldTokenPath() string {
	if d := oldAppDataDir(); d != "" {
		return filepath.Join(d, "token")
	}
	return ""
}

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

// Migration preserves conflicting or unreadable sources for manual recovery.
// Failures are reported without failing installation.
func migrateUserData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	ensureUserDataTree()

	// 1) User content: %USERPROFILE%\snaphak -> the app-data dir, then remove the old folder if fully mirrored.
	if old := oldUserContentDir(); old != "" && !sameFile(old, dir) {
		if _, err := os.Stat(old); err == nil {
			_, copyErr := copyTreeMissing(old, dir)
			if copyErr == nil && fullyMirrored(old, dir) {
				if removeMirroredTree(old, dir) == nil {
					fmt.Printf("  ~ moved your saved content (overrides / prefabs / rawmaps) to %s\n", dir)
				}
			} else {
				fmt.Printf("  ~ copied your content to %s -- kept %s (some files could not be verified)\n", dir, old)
			}
		}
	}

	// Retire only verified metadata; unknown files keep the old directory alive.
	if oldAD := oldAppDataDir(); oldAD != "" && !sameFile(oldAD, dir) {
		if _, err := os.Stat(oldAD); err == nil {
			for _, name := range []string{"install.json", "token"} {
				s, t := filepath.Join(oldAD, name), filepath.Join(dir, name)
				if _, err := os.Lstat(s); os.IsNotExist(err) {
					continue
				}
				if err := copyMissingFile(s, t); err == nil && identicalFiles(s, t) {
					os.Remove(s)
				} else {
					fmt.Printf("  ! kept legacy metadata %s -- the destination could not be verified\n", s)
				}
			}
			os.Remove(oldAD) // Only an empty directory can be retired.
		}
	}

	// Migrate overrides last so newly copied legacy content follows the same layout.
	migrateLegacyOverrides()
}

// fullyMirrored rejects missing entries, byte differences, links and read errors.
func fullyMirrored(src, dst string) bool {
	err := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			return rerr
		}
		if !identicalFiles(path, filepath.Join(dst, rel)) {
			return fmt.Errorf("unverified migration: %s", path)
		}
		return nil
	})
	return err == nil
}

// copyTreeMissing never overwrites a destination and reports incomplete copies.
func copyTreeMissing(src, dst string) (int, error) {
	copied := 0
	var failures error
	walkErr := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			failures = errors.Join(failures, err)
			return nil
		}
		if d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			failures = errors.Join(failures, rerr)
			return nil
		}
		target := filepath.Join(dst, rel)
		if _, err := os.Lstat(target); err == nil {
			return nil
		}
		if err := copyMissingFile(path, target); err == nil {
			copied++
		} else {
			failures = errors.Join(failures, err)
		}
		return nil
	})
	return copied, errors.Join(failures, walkErr)
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
		return fmt.Errorf("migration refuses linked paths: %s", src)
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
		return fmt.Errorf("migration requires a regular file: %s", src)
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

// Recheck each source and remove empty directories only. Late additions survive.
func removeMirroredTree(src, dst string) error {
	var dirs []string
	err := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			dirs = append(dirs, path)
			return nil
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if !identicalFiles(path, filepath.Join(dst, rel)) {
			return fmt.Errorf("migration changed before removal: %s", path)
		}
		return os.Remove(path)
	})
	if err != nil {
		return err
	}
	for i := len(dirs) - 1; i >= 0; i-- {
		if err := os.Remove(dirs[i]); err != nil {
			return err
		}
	}
	return nil
}

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// Player content lives in writable local app data, outside the game installation.

// userContentSubdirs is the content tree the installer scaffolds and the backend reads; it mirrors
// sh_user_dirs_subfolders in src/backend/user_dirs.c. A strings folder a previous install left stays as it is.
var userContentSubdirs = []string{"prefabs"}

// retiredContent reports whether a path relative to a content root belongs to a package folder this
// release does not use. The installer never copies, scaffolds or removes it.
func retiredContent(rel string) bool {
	first := strings.ToLower(strings.SplitN(filepath.ToSlash(rel), "/", 2)[0])
	switch first {
	case "overrides", "package-cache", "package-staging", "override-backups": // retired package folder names
		return true
	}
	return false
}

// regenerableContent reports whether a path relative to a content root is regenerable cache: the
// cache folder (the companion window's WebView2 profile lives in cache\webview2) and a top-level
// webview2 folder. The game rebuilds it, so it is never backed up, copied or required at a destination.
func regenerableContent(rel string) bool {
	first := strings.ToLower(strings.SplitN(filepath.ToSlash(rel), "/", 2)[0])
	return first == "cache" || first == "webview2"
}

// outsideMigration reports content the user-data migration leaves where it is: retired package
// folders and regenerable cache.
func outsideMigration(rel string) bool {
	return rel != "." && (retiredContent(rel) || regenerableContent(rel))
}

// oldUserContentDir is the home-root data folder, %USERPROFILE%\snaphak; empty if USERPROFILE is unset.
func oldUserContentDir() string {
	base := os.Getenv("USERPROFILE")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "snaphak")
}

// oldAppDataDir is the app-data folder %LOCALAPPDATA%\open-snaphak, which may hold an install record and token.
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

// migrateUserData moves content and metadata from the old folders into the app-data dir, keeping conflicting
// or unreadable sources. Failures are reported without failing installation.
func migrateUserData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	ensureUserDataTree()

	// User content: %USERPROFILE%\snaphak is copied into the app-data dir, then removed if fully mirrored;
	// retired package folders and regenerable cache stay.
	if old := oldUserContentDir(); old != "" && !sameFile(old, dir) {
		if _, err := os.Stat(old); err == nil {
			_, copyErr := copyTreeMissing(old, dir)
			if copyErr == nil && fullyMirrored(old, dir) {
				if removeMirroredTree(old, dir) == nil {
					ui.OK("moved your saved content (prefabs / strings / rawmaps) to %s", dir)
				}
			} else {
				ui.Warn("copied your content to %s -- kept %s (some files could not be verified)", dir, old)
			}
		}
	}

	// Only verified copies of the record and token are removed; other files keep the folder.
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
					ui.Warn("left %s in the old open-snaphak folder because its copy could not be checked; you can delete it", s)
				}
			}
			os.Remove(oldAD) // Fails unless the directory is empty.
		}
	}
}

// fullyMirrored rejects missing entries, byte differences, links and read errors; retired package and
// cache content is skipped.
func fullyMirrored(src, dst string) bool {
	err := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			return rerr
		}
		if outsideMigration(rel) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			return nil
		}
		if !identicalFiles(path, filepath.Join(dst, rel)) {
			return fmt.Errorf("unverified migration: %s", path)
		}
		return nil
	})
	return err == nil
}

// copyTreeMissing never overwrites a destination and reports incomplete copies; retired package and
// cache content is skipped.
func copyTreeMissing(src, dst string) (int, error) {
	copied := 0
	var failures error
	walkErr := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			failures = errors.Join(failures, err)
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			failures = errors.Join(failures, rerr)
			return nil
		}
		if outsideMigration(rel) {
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
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

// removeMirroredTree rechecks each source file, removes it and then the empty directories. Files added
// late, retired package content and regenerable cache stay.
func removeMirroredTree(src, dst string) error {
	var dirs []string
	kept := false
	err := filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		if outsideMigration(rel) {
			kept = true
			if d.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if d.IsDir() {
			dirs = append(dirs, path)
			return nil
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
		if err := os.Remove(dirs[i]); err != nil && !(kept && dirs[i] == src) {
			return err
		}
	}
	return nil
}

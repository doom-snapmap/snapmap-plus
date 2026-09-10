package main

import (
	"fmt"
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

// migrateUserData copies missing legacy files, then removes the source when
// fullyMirrored finds every destination path. Existing destination files win;
// contents are not compared. It also retires old metadata and the shared override
// layout. Migration is best-effort and does not fail installation.
func migrateUserData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	ensureUserDataTree()

	// 1) User content: %USERPROFILE%\snaphak -> the app-data dir, then remove the old folder if fully mirrored.
	if old := oldUserContentDir(); old != "" && !sameFile(old, dir) {
		if _, err := os.Stat(old); err == nil {
			copyTreeMissing(old, dir)
			if fullyMirrored(old, dir) {
				if os.RemoveAll(old) == nil {
					fmt.Printf("  ~ moved your saved content (overrides / prefabs / rawmaps) to %s\n", dir)
				}
			} else {
				fmt.Printf("  ~ copied your content to %s -- kept %s (some files could not be verified)\n", dir, old)
			}
		}
	}

	// Copy missing old metadata, then remove its directory. Copy errors are ignored
	// here; unlike content migration, this path has no post-copy verification.
	if oldAD := oldAppDataDir(); oldAD != "" && !sameFile(oldAD, dir) {
		if _, err := os.Stat(oldAD); err == nil {
			for _, name := range []string{"install.json", "token"} {
				s, t := filepath.Join(oldAD, name), filepath.Join(dir, name)
				if _, e := os.Stat(t); e != nil {
					if _, e := os.Stat(s); e == nil {
						copyFile(s, t)
					}
				}
			}
			os.RemoveAll(oldAD)
		}
	}

	// Migrate overrides last so newly copied legacy content follows the same layout.
	migrateLegacyOverrides()
}

// fullyMirrored checks destination path presence for enumerated files.
// It does not compare bytes or treat source enumeration errors as failure.
func fullyMirrored(src, dst string) bool {
	ok := true
	filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			ok = false
			return nil
		}
		if _, e := os.Stat(filepath.Join(dst, rel)); e != nil {
			ok = false
		}
		return nil
	})
	return ok
}

// copyTreeMissing recursively copies every file under src into dst, preserving the relative layout, but only
// when the destination file does NOT already exist (it never overwrites). Returns the number of files copied.
// Best-effort: an unreadable/unwritable file (or a missing src) is skipped, not fatal.
func copyTreeMissing(src, dst string) int {
	copied := 0
	filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil { // unreadable entry, or src doesn't exist -> nothing to migrate
			return nil
		}
		if d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			return nil
		}
		target := filepath.Join(dst, rel)
		if _, err := os.Stat(target); err == nil {
			return nil // already present -> never overwrite
		}
		if os.MkdirAll(filepath.Dir(target), 0o755) != nil {
			return nil
		}
		if copyFile(path, target) == nil {
			copied++
		}
		return nil
	})
	return copied
}

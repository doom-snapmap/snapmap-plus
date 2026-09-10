package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// appDataDir holds installer metadata and player content under LOCALAPPDATA.
// The backend owns config.json. An unset LOCALAPPDATA returns an empty path.
func appDataDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "snapmap-plus")
}

// sameFile reports whether two paths are the same on-disk file (so we never try to overwrite/delete the exe
// we're currently running from).
func sameFile(a, b string) bool {
	sa, ea := os.Stat(a)
	sb, eb := os.Stat(b)
	return ea == nil && eb == nil && os.SameFile(sa, sb)
}

// selfInstall keeps a stable executable copy in app data. It skips the running
// location and ignores copy failures so the original launch can continue.
func selfInstall() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	exe, err := os.Executable()
	if err != nil {
		return
	}
	// Preserve the running executable's name.
	target := filepath.Join(dir, filepath.Base(exe))
	if sameFile(exe, target) {
		return // already running from the installed location
	}
	_, existed := os.Stat(target)
	if os.MkdirAll(dir, 0o755) != nil {
		return
	}
	if copyFile(exe, target) != nil {
		return
	}
	if existed != nil { // first time -> let the user know where it went
		fmt.Printf("(snapmap-plus is now installed at %s -- run it from there in future, or add that folder to your PATH)\n", target)
	}
}

// cleanupAppData removes installer metadata and executable copies other than
// the running file. It preserves config.json and nonempty content folders.
func cleanupAppData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	os.Remove(filepath.Join(dir, "install.json"))
	os.Remove(filepath.Join(dir, "token"))
	// This cleanup treats every top-level .exe as an installer copy.
	// It does not consult an ownership record for these files.
	exe, _ := os.Executable()
	if matches, err := filepath.Glob(filepath.Join(dir, "*.exe")); err == nil {
		for _, m := range matches {
			if exe == "" || !sameFile(exe, m) {
				os.Remove(m)
			}
		}
	}
	// Remove only empty content folders, then the parent if empty.
	for _, sub := range userContentSubdirs {
		removeIfEmpty(filepath.Join(dir, sub))
	}
	removeIfEmpty(dir)
}

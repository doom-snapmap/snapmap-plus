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

// selfInstall keeps a stable executable copy in app data. Unless force is set it refuses when that
// copy is a newer release than this one; copy failures are ignored so the launch can continue.
func selfInstall(force bool) error {
	dir := appDataDir()
	if dir == "" {
		return nil
	}
	exe, err := executablePath()
	if err != nil {
		return nil
	}
	// Preserve the running executable's name.
	target := filepath.Join(dir, filepath.Base(exe))
	if sameFile(exe, target) {
		if installSelfCopy(exe, target) == nil {
			noteCopyVersion(target, version)
		}
		return nil // already running from the installed location
	}
	if installed := copyVersionOf(target); installed != "" && !force {
		if c, ok := compareVersions(version, installed); ok && c < 0 {
			return fmt.Errorf("this snapmap-plus.exe is %s, older than the %s installed at %s -- run that one instead, "+
				"or pass --force to replace it with this one. Nothing was changed", version, installed, target)
		}
	}
	_, existed := os.Stat(target)
	if os.MkdirAll(dir, 0o755) != nil {
		return nil
	}
	if installSelfCopy(exe, target) != nil {
		return nil
	}
	noteCopyVersion(target, version)
	if existed != nil { // first time -> let the user know where it went
		fmt.Printf("(snapmap-plus is now installed at %s -- run it from there in future, or add that folder to your PATH)\n", target)
	}
	return nil
}

// cleanupAppData removes metadata and unchanged, recorded executable copies.
// Unrecorded files, the running image and player content remain untouched.
func cleanupAppData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	os.Remove(filepath.Join(dir, "install.json"))
	os.Remove(filepath.Join(dir, "token"))
	exe, _ := executablePath()
	cleanupOwnedExecutables(dir, exe)
	pruneCopyVersions(dir)
	// Remove only empty content folders, then the parent if empty.
	for _, sub := range userContentSubdirs {
		removeIfEmpty(filepath.Join(dir, sub))
	}
	removeIfEmpty(dir)
}

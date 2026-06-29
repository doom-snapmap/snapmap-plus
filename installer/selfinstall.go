package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// appDataDir is %LOCALAPPDATA%\open-snaphak -- where the install record, the token, and (after selfInstall) a
// stable copy of snaphak.exe live. Returns "" if LOCALAPPDATA is not set.
func appDataDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "open-snaphak")
}

// sameFile reports whether two paths are the same on-disk file (so we never try to overwrite/delete the exe
// we're currently running from).
func sameFile(a, b string) bool {
	sa, ea := os.Stat(a)
	sb, eb := os.Stat(b)
	return ea == nil && eb == nil && os.SameFile(sa, sb)
}

// selfInstall copies the running snaphak.exe into appDataDir, so a hand-delivered exe lives in a stable place
// (survives a Downloads cleanup; can be added to PATH). No-op if it's already running from there. Best-effort:
// any failure is silent -- the tool still works from wherever it was launched.
func selfInstall() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	exe, err := os.Executable()
	if err != nil {
		return
	}
	target := filepath.Join(dir, "snaphak.exe")
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
		fmt.Printf("(snaphak is now installed at %s -- run it from there in future, or add that folder to your PATH)\n", target)
	}
}

// cleanupAppData removes the install record, the saved token, and the stable snaphak.exe copy, then the folder
// itself if empty. Called on uninstall. Can't delete the exe if you're running THAT copy -- left in place then.
func cleanupAppData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	os.Remove(filepath.Join(dir, "install.json"))
	os.Remove(filepath.Join(dir, "token"))
	self := filepath.Join(dir, "snaphak.exe")
	if exe, err := os.Executable(); err != nil || !sameFile(exe, self) {
		os.Remove(self)
	}
	removeIfEmpty(dir)
}

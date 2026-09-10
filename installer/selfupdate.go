package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// selfExeAsset is the standalone CLI published alongside each release's overlay bundle.
const selfExeAsset = "snapmap-plus.exe"

// shouldSelfUpdate decides whether the running snapmap-plus.exe should replace itself with the resolved release's
// exe. It stays put for a dev/local build (version "dev" -- never clobber a hand-built binary) or when the
// running version already matches the release tag.
func shouldSelfUpdate(runningVersion, releaseTag string) bool {
	if runningVersion == "" || runningVersion == "dev" {
		return false
	}
	if releaseTag == "" || releaseTag == runningVersion {
		return false
	}
	return true
}

// assetMatchesFile reports whether the file on disk already has the release asset's exact bytes, via the
// asset's "sha256:<hex>" digest. False when the digest is absent -- the caller then downloads and compares.
func assetMatchesFile(a *ghAsset, path string) bool {
	hexDigest, ok := strings.CutPrefix(a.Digest, "sha256:")
	if !ok || hexDigest == "" {
		return false
	}
	sum, err := fileSHA256(path)
	return err == nil && sum == hexDigest
}

// selfUpdate runs after the overlay update and reports failures without failing
// that installation. Check disk contents as well as the running version: this
// process can keep running after an earlier update replaced its executable.
// The new binary takes effect on the next launch.
func selfUpdate(f flags, token string) {
	rel, err := fetchRelease(f, token)
	if err != nil {
		fmt.Printf("(couldn't check for a snapmap-plus.exe update: %v)\n", err)
		return
	}
	if !shouldSelfUpdate(version, rel.TagName) {
		return // already current, or a dev build
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == selfExeAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return // this release doesn't ship a standalone exe
	}

	exe, err := os.Executable()
	if err != nil {
		fmt.Printf("(couldn't locate the running snapmap-plus.exe to update it: %v)\n", err)
		return
	}
	if assetMatchesFile(asset, exe) {
		return // the on-disk exe is already this release (an earlier run updated it) -- nothing to do
	}
	tmp, err := os.MkdirTemp("", "snapmap-plus-exe-")
	if err != nil {
		return
	}
	defer os.RemoveAll(tmp)
	newExe := filepath.Join(tmp, selfExeAsset)
	if err := downloadAsset(asset, token, newExe); err != nil {
		fmt.Printf("(couldn't download the new snapmap-plus.exe: %v)\n", err)
		return
	}
	if onDisk, err := fileSHA256(exe); err == nil {
		if fresh, err := fileSHA256(newExe); err == nil && fresh == onDisk {
			return // same bytes already in place (release had no digest to catch this earlier)
		}
	}
	if err := replaceExe(exe, newExe); err != nil {
		fmt.Printf("(couldn't replace snapmap-plus.exe (%v) -- the overlay updated fine; you can grab the new snapmap-plus.exe from the release if needed)\n", err)
		return
	}
	// Keep the stable %LOCALAPPDATA% copy current too, if we're running from somewhere else.
	if dir := appDataDir(); dir != "" {
		if filepath.Clean(filepath.Dir(exe)) == filepath.Clean(dir) {
			_ = installSelfCopy(exe, exe)
		}
		if stable := filepath.Join(dir, selfExeAsset); !sameFile(stable, exe) {
			if os.MkdirAll(dir, 0o755) == nil {
				_ = installSelfCopy(newExe, stable) // best-effort
			}
		}
	}
	fmt.Printf("Updated snapmap-plus.exe to %s (takes effect next time you run snapmap-plus).\n", rel.TagName)
}

type replaceExeOps struct {
	copyFile func(string, string) error
	rename   func(string, string) error
}

// Stage complete bytes before renaming the current image. A failed rollback
// reports the old image's recovery path and never removes that image.
func replaceExe(path, newExe string) error {
	return replaceExeWithOps(path, newExe, replaceExeOps{copyFile, os.Rename})
}

func replaceExeWithOps(path, newExe string, ops replaceExeOps) error {
	staged, err := os.CreateTemp(filepath.Dir(path), ".snapmap-plus-update-*")
	if err != nil {
		return err
	}
	stagedPath := staged.Name()
	defer os.Remove(stagedPath)
	if err := staged.Close(); err != nil {
		return err
	}
	if err := ops.copyFile(newExe, stagedPath); err != nil {
		return fmt.Errorf("stage new executable: %w", err)
	}
	if !identicalFiles(newExe, stagedPath) {
		return fmt.Errorf("staged executable does not match the download")
	}
	removeOldLeftovers(path)
	for i := 0; i < 10; i++ {
		old := path + ".old"
		if i > 0 {
			old = fmt.Sprintf("%s.old%d", path, i+1)
		}
		if _, statErr := os.Lstat(old); !os.IsNotExist(statErr) {
			continue // Existing recovery copies are never overwritten.
		}
		if err := forgetUpdateBackup(path, old); err != nil {
			return fmt.Errorf("prepare executable recovery record: %w", err)
		}
		if err = ops.rename(path, old); err != nil {
			continue // this aside name is held by a still-running image -- try the next one
		}
		if installErr := ops.rename(stagedPath, path); installErr != nil {
			if rollbackErr := ops.rename(old, path); rollbackErr != nil {
				return fmt.Errorf("install new executable: %v; rollback failed: %v; recover the original from %s", installErr, rollbackErr, old)
			}
			return fmt.Errorf("install new executable (original restored): %w", installErr)
		}
		if err := recordUpdateBackup(path, old); err != nil {
			fmt.Printf("(kept the prior installer at %s; could not record cleanup ownership: %v)\n", old, err)
		}
		return nil
	}
	return fmt.Errorf("could not reserve an executable backup path: %v", err)
}

// Clean recorded backups after a successful update. Locked images remain.
func cleanupSelfUpdateLeftovers() {
	if exe, err := os.Executable(); err == nil {
		removeOldLeftovers(exe)
	}
}

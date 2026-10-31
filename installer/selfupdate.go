package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// selfExeAsset is the standalone CLI published alongside each release's overlay bundle.
const selfExeAsset = "snapmap-plus.exe"

// executablePath locates the running image; tests point it at a stand-in so the test binary is never replaced.
var executablePath = os.Executable

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
		ui.Warn("couldn't check for a snapmap-plus.exe update: %v", err)
		return
	}
	noteReleaseChecked(rel.TagName)
	if !shouldSelfUpdate(version, rel.TagName) {
		return // already current, or a dev build
	}
	if _, updated := selfUpdateTo(rel, token); updated {
		ui.OK("Updated snapmap-plus.exe to %s (takes effect next time you run snapmap-plus).", rel.TagName)
	}
}

// selfUpdateTo replaces the running image with the release's snapmap-plus.exe when the release ships one
// and the bytes on disk differ. It returns the executable's path and whether it was replaced; every
// failure is a warning. Shared by the start-of-run update (which then relaunches) and the update
// command's final step (which only reports).
func selfUpdateTo(rel *ghRelease, token string) (string, bool) {
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == selfExeAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return "", false // this release doesn't ship a standalone exe
	}

	exe, err := executablePath()
	if err != nil {
		ui.Warn("couldn't locate the running snapmap-plus.exe to update it: %v", err)
		return "", false
	}
	if assetMatchesFile(asset, exe) {
		return exe, false // the on-disk exe is already this release (an earlier run updated it) -- nothing to do
	}
	tmp, err := os.MkdirTemp("", "snapmap-plus-exe-")
	if err != nil {
		return exe, false
	}
	defer os.RemoveAll(tmp)
	newExe := filepath.Join(tmp, selfExeAsset)
	if err := downloadAsset(asset, token, newExe); err != nil {
		ui.Warn("couldn't download the new snapmap-plus.exe: %v", err)
		return exe, false
	}
	if onDisk, err := fileSHA256(exe); err == nil {
		if fresh, err := fileSHA256(newExe); err == nil && fresh == onDisk {
			return exe, false // same bytes already in place (release had no digest to catch this earlier)
		}
	}
	if err := replaceExe(exe, newExe); err != nil {
		ui.Warn("couldn't replace snapmap-plus.exe (%v) -- you can grab the new snapmap-plus.exe from the release if needed", err)
		return exe, false
	}
	if sum, err := fileSHA256(exe); err == nil {
		notePlacedInstaller(rel.TagName, sum)
	}
	// Keep the stable %LOCALAPPDATA% copy current too, if we're running from somewhere else.
	if dir := appDataDir(); dir != "" {
		if filepath.Clean(filepath.Dir(exe)) == filepath.Clean(dir) {
			if installSelfCopy(exe, exe) == nil {
				noteCopyVersion(exe, rel.TagName)
			}
		}
		if stable := filepath.Join(dir, selfExeAsset); !sameFile(stable, exe) {
			if os.MkdirAll(dir, 0o755) == nil && installSelfCopy(newExe, stable) == nil { // best-effort
				noteCopyVersion(stable, rel.TagName)
			}
		}
	}
	return exe, true
}

type replaceExeOps struct {
	copyFile func(string, string) error
	rename   func(string, string) error
	remove   func(string) error
}

// Stage complete bytes before moving the current image aside. A failed rollback reports the old
// image's recovery path and never removes that image.
func replaceExe(path, newExe string) error {
	return replaceExeWithOps(path, newExe, replaceExeOps{copyFile, os.Rename, os.Remove})
}

// replaceExeWithOps moves the current image into the data folder's cache\installer (beside it when
// that is not possible), puts the new bytes in its place, and removes the replaced image at once. A
// replaced image that is still running cannot be removed: it is recorded in install.json and removed
// at the next start.
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
	sum, _ := fileSHA256(path)
	defer tidyInstallerCache()
	err = fmt.Errorf("no free name")
	for _, old := range asideCandidates(path) {
		if _, statErr := os.Lstat(old); !os.IsNotExist(statErr) {
			continue // Existing recovery copies are never overwritten.
		}
		if err = ops.rename(path, old); err != nil {
			continue // another volume, or a name held by a still-running image -- try the next one
		}
		if installErr := ops.rename(stagedPath, path); installErr != nil {
			if rollbackErr := ops.rename(old, path); rollbackErr != nil {
				return fmt.Errorf("install new executable: %v; rollback failed: %v; recover the original from %s", installErr, rollbackErr, old)
			}
			return fmt.Errorf("install new executable (original restored): %w", installErr)
		}
		if ops.remove(old) != nil {
			if recErr := recordSetAside(old, sum, path); recErr != nil {
				ui.Warn("kept the prior installer at %s; could not record it for removal: %v", old, recErr)
			}
		}
		return nil
	}
	return fmt.Errorf("could not reserve an executable backup path: %v", err)
}

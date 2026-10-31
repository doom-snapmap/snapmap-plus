package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// The 2018 build is installed by the final 2018 release's own snapmap-plus.exe, exactly as that
// release would install itself: its install also converts the packages in overrides\ and keeps the
// my-overrides starter package, steps the current installer does not have. The downloaded copy is
// renamed so its own stable copy never replaces the current installer's in %LOCALAPPDATA%.

// legacyExeName is the name the legacy installer runs under (and copies itself to in app data).
const legacyExeName = "snapmap-plus-" + legacyReleaseTag + ".exe"

// runLegacyExe runs the downloaded legacy installer attached to this console; tests replace it.
var runLegacyExe = func(path string, args []string) error {
	cmd := exec.Command(path, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	return cmd.Run()
}

// installLegacyRelease downloads legacyReleaseTag's snapmap-plus.exe, verified like every release
// asset, and runs its install for doom. Install, update and reinstall on the 2018 build all come here.
func installLegacyRelease(f flags, doom string) error {
	token := resolveToken(f)
	rel, err := fetchRelease(flags{release: legacyReleaseTag}, token)
	if err != nil {
		return err
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == selfExeAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return fmt.Errorf("release %s has no %s to install the 2018 build with. Nothing was changed", legacyReleaseTag, selfExeAsset)
	}
	tmp, err := os.MkdirTemp("", "snapmap-plus-legacy-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	exe := filepath.Join(tmp, legacyExeName)
	if err := downloadAsset(asset, token, exe); err != nil {
		return fmt.Errorf("%v. Nothing was changed", err)
	}
	digest, err := fileSHA256(exe)
	if err != nil {
		return err
	}
	args := []string{"install", "--doom", doom, "--release", legacyReleaseTag}
	if f.yes {
		args = append(args, "--yes")
	}
	if f.token != "" {
		args = append(args, "--token", f.token)
	}
	ui.Section("Installing files")
	ui.Note("Running the %s installer for the 2018 build...", legacyReleaseTag)
	runErr := runLegacyExe(exe, args)
	ui.External()
	dropLegacyCopy(digest)
	if runErr != nil {
		var exit *exec.ExitError
		if errors.As(runErr, &exit) {
			return fmt.Errorf("the %s installer failed (exit %d)", legacyReleaseTag, exit.ExitCode())
		}
		return fmt.Errorf("couldn't run the %s installer (%v)", legacyReleaseTag, runErr)
	}
	return nil
}

// dropLegacyCopy removes the copy the legacy installer keeps of itself in app data, while its bytes
// are still the downloaded ones, so the current installer stays the only one recorded there.
func dropLegacyCopy(digest string) {
	dir := appDataDir()
	if dir == "" {
		return
	}
	path := filepath.Join(dir, legacyExeName)
	rec, err := loadExecutableRecord(dir)
	if err != nil {
		return
	}
	if ownsExecutable(path, digest) && os.Remove(path) == nil {
		delete(rec, legacyExeName)
	} else if _, statErr := os.Lstat(path); os.IsNotExist(statErr) {
		delete(rec, legacyExeName)
	}
	if len(rec) == 0 {
		os.Remove(filepath.Join(dir, executableRecordName))
		return
	}
	saveExecutableRecord(dir, rec)
}

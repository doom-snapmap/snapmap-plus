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
	// That installer rewrites install.json in its own format and keeps backups in DOOM, so the
	// installer's own state is carried across its run and the DOOM folder is brought to the 2018
	// pair's contract before and after it.
	dir := appDataDir()
	saved, _ := loadStateAt(dir)
	tidyRetiredFolders()
	reportStartupTidy()
	tidy2018(doom, "Cleaning the DOOM folder")
	ui.Section("Installing files")
	ui.Note("Running the %s installer for the 2018 build...", legacyReleaseTag)
	runErr := runLegacyExe(exe, args)
	ui.External()
	dropLegacyCopy(digest)
	if saved != nil {
		updateStateAt(dir, func(r *installRecord) {
			for name, e := range saved.Executables {
				if _, have := r.Executables[name]; !have && ownsExecutable(filepath.Join(dir, name), e.SHA256) {
					if r.Executables == nil {
						r.Executables = map[string]exeEntry{}
					}
					r.Executables[name] = e
				}
			}
			if r.SelfUpdate == nil {
				r.SelfUpdate = saved.SelfUpdate
			}
			if r.DoomPath != "" {
				r.DoomBuild = build2018
			}
		})
	}
	ui.Blank()
	startupTidy = append(startupTidy, retireBookkeeping(dir, "")...)
	tidyRetiredFolders()
	reportStartupTidy()
	tidy2018(doom, "Cleaning up after the "+legacyReleaseTag+" installer")
	if runErr != nil {
		var exit *exec.ExitError
		if errors.As(runErr, &exit) {
			return fmt.Errorf("the %s installer failed (exit %d)", legacyReleaseTag, exit.ExitCode())
		}
		return fmt.Errorf("couldn't run the %s installer (%v)", legacyReleaseTag, runErr)
	}
	return nil
}

// tidy2018 brings a 2018 DOOM folder to its pair's contract: the April 2024 pair and every backup of
// ours go, backups of files that were not ours move into cache\installer, and the record follows.
func tidy2018(doom, title string) {
	ui.LazySection(title)
	dir := appDataDir()
	rec, err := loadStateAt(dir)
	if err != nil {
		return
	}
	t := cleanDoomFolder(doom, buildPair(build2018), newOwnership(doom, rec, nil), rec)
	printDoomTidy(t, dir)
	if rec.DoomPath != "" || stateExists(dir) {
		saveStateAt(dir, rec)
	}
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
	saveExecutableRecord(dir, rec)
}

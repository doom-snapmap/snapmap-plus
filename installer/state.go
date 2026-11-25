package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// install.json, in %LOCALAPPDATA%\snapmap-plus, is the installer's only bookkeeping file. The backend and
// the frontend read its first "version" key, so Version stays the first field.
const stateName = "install.json"

// Bookkeeping files a previous install placed that this release does not ship; they are folded into
// install.json and removed.
const (
	retiredExecutableRecord = "installer-files.json"    // name -> SHA-256 of the executables placed
	retiredVersionsRecord   = "installer-versions.json" // name -> {sha256, version}
	retiredUpdatesSuffix    = ".updates.json"           // <exe>.updates.json: self-update backups
)

// installRecord is install.json.
type installRecord struct {
	Version     string   `json:"version"` // first: the backend reads the first "version" key
	DoomPath    string   `json:"doom_path"`
	DoomBuild   string   `json:"doom_build,omitempty"` // build2024 or build2018
	InstalledAt string   `json:"installed_at"`
	Files       []string `json:"files"`   // DOOM-relative paths of the pair placed under DoomPath
	Backups     []backup `json:"backups"` // files that were not ours and were replaced, kept in cache\installer
	// Executables: the executables this installer placed in the data folder, by file name.
	Executables map[string]exeEntry `json:"executables,omitempty"`
	SelfUpdate  *selfUpdateState    `json:"self_update,omitempty"`
}

type backup struct {
	Rel    string `json:"rel"`    // DOOM-relative path that held a file that was not ours
	Backup string `json:"backup"` // absolute path of the saved-aside copy
}

// exeEntry ties an executable in the data folder to the bytes this installer placed, so a file the
// player replaced is never deleted or overwritten, and names the release it is when known.
type exeEntry struct {
	SHA256  string `json:"sha256"`
	Version string `json:"version,omitempty"`
}

// selfUpdateState is what the installer's own updates leave behind.
type selfUpdateState struct {
	Release      string     `json:"release,omitempty"`       // the last release checked
	CheckedAt    string     `json:"checked_at,omitempty"`    // when
	PlacedSHA256 string     `json:"placed_sha256,omitempty"` // the snapmap-plus.exe it placed
	SetAside     []setAside `json:"set_aside,omitempty"`     // replaced images still to remove
}

// setAside is a replaced executable moved aside because it was running. It is removed at the next start
// while its bytes are unchanged and the executable that replaced it exists.
type setAside struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	For    string `json:"for"`
}

func statePath(dir string) string { return filepath.Join(dir, stateName) }

// installerCacheDir is where the installer keeps what must outlive a run but not clutter the data
// folder: replaced images that were still running, and backups of files that were not ours.
func installerCacheDir(dir string) string { return filepath.Join(dir, "cache", "installer") }

// loadStateAt reads dir's install.json. A missing file is an empty record.
func loadStateAt(dir string) (*installRecord, error) {
	data, err := os.ReadFile(statePath(dir))
	if os.IsNotExist(err) {
		return &installRecord{}, nil
	}
	if err != nil {
		return nil, err
	}
	var rec installRecord
	if err := json.Unmarshal(data, &rec); err != nil {
		return setAsideUnreadable(dir, err)
	}
	return &rec, nil
}

// unreadableSuffix names the copy of an install.json that could not be read.
const unreadableSuffix = ".unreadable"

// setAsideUnreadable renames an install.json that is not valid JSON to install.json.unreadable and
// starts an empty record, so the next install or update can run; files in DOOM are then recognised
// by their contents.
func setAsideUnreadable(dir string, cause error) (*installRecord, error) {
	p := statePath(dir)
	aside := p + unreadableSuffix
	os.Remove(aside)
	if err := os.Rename(p, aside); err != nil {
		return nil, fmt.Errorf("%s could not be read (%v) and could not be renamed (%v) -- delete it, then run: snapmap-plus update", p, cause, err)
	}
	ui.Warn("%s could not be read (%v), so it was renamed to %s and a new record is started. "+
		"If Snapmap+ is installed, run: snapmap-plus update", p, cause, filepath.Base(aside))
	return &installRecord{}, nil
}

// saveStateAt replaces dir's install.json atomically.
func saveStateAt(dir string, rec *installRecord) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	return saveJSONFile(statePath(dir), rec)
}

// updateStateAt applies fn to dir's install.json and saves it.
func updateStateAt(dir string, fn func(*installRecord)) error {
	rec, err := loadStateAt(dir)
	if err != nil {
		return err
	}
	fn(rec)
	return saveStateAt(dir, rec)
}

// stateExists reports whether dir holds an install.json.
func stateExists(dir string) bool {
	_, err := os.Lstat(statePath(dir))
	return err == nil
}

func saveJSONFile(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".install-json-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	_, err = tmp.Write(data)
	if err == nil {
		err = tmp.Sync()
	}
	closeErr := tmp.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(tmp.Name(), path)
}

func recordPath() (string, error) {
	dir := appDataDir()
	if dir == "" {
		return "", fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	return statePath(dir), nil
}

// errNotInstalled is loadRecord's answer for a data folder without an installation in its record.
var errNotInstalled = errors.New("no installation recorded")

// loadRecord returns the installation recorded in install.json, or an error when there is none: no
// file, or a record that holds only the installer's own state.
func loadRecord() (*installRecord, error) {
	p, err := recordPath()
	if err != nil {
		return nil, err
	}
	dir := filepath.Dir(p)
	rec, err := loadStateAt(dir)
	if err != nil {
		return nil, err
	}
	if rec.DoomPath == "" {
		return nil, errNotInstalled
	}
	return rec, nil
}

// noteReleaseChecked records the release the installer last resolved for its own update. It writes
// only an existing install.json, so a lookup never creates the data folder's record.
func noteReleaseChecked(tag string) {
	dir := appDataDir()
	if dir == "" || tag == "" || !stateExists(dir) {
		return
	}
	updateStateAt(dir, func(r *installRecord) {
		if r.SelfUpdate == nil {
			r.SelfUpdate = &selfUpdateState{}
		}
		r.SelfUpdate.Release = tag
		r.SelfUpdate.CheckedAt = time.Now().UTC().Format(time.RFC3339)
	})
}

// notePlacedInstaller records the hash of the snapmap-plus.exe a self-update placed.
func notePlacedInstaller(tag, sum string) {
	dir := appDataDir()
	if dir == "" || sum == "" {
		return
	}
	updateStateAt(dir, func(r *installRecord) {
		if r.SelfUpdate == nil {
			r.SelfUpdate = &selfUpdateState{}
		}
		r.SelfUpdate.Release = tag
		r.SelfUpdate.CheckedAt = time.Now().UTC().Format(time.RFC3339)
		r.SelfUpdate.PlacedSHA256 = sum
	})
}

// recordSetAside remembers a replaced image that could not be removed yet.
func recordSetAside(path, sum, forExe string) error {
	dir := appDataDir()
	if dir == "" {
		return fmt.Errorf("no data folder to record %s in", path)
	}
	return updateStateAt(dir, func(r *installRecord) {
		if r.SelfUpdate == nil {
			r.SelfUpdate = &selfUpdateState{}
		}
		kept := r.SelfUpdate.SetAside[:0]
		for _, e := range r.SelfUpdate.SetAside {
			if !strings.EqualFold(filepath.Clean(e.Path), filepath.Clean(path)) {
				kept = append(kept, e)
			}
		}
		r.SelfUpdate.SetAside = append(kept, setAside{Path: path, SHA256: sum, For: forExe})
	})
}

// sweepSetAside removes each recorded replaced image whose bytes are unchanged and whose replacement exists.
// A locked image stays recorded, a changed one is forgotten, and one without a replacement is kept.
func sweepSetAside() {
	dir := appDataDir()
	if dir == "" || !stateExists(dir) {
		return
	}
	rec, err := loadStateAt(dir)
	if err != nil || rec.SelfUpdate == nil || len(rec.SelfUpdate.SetAside) == 0 {
		return
	}
	var kept []setAside
	for _, e := range rec.SelfUpdate.SetAside {
		if st, err := os.Lstat(e.For); err != nil || !st.Mode().IsRegular() {
			kept = append(kept, e) // the only copy of the installer: keep it
			continue
		}
		if !ownsExecutable(e.Path, e.SHA256) {
			continue // gone, or no longer the bytes we moved: forget it
		}
		if os.Remove(e.Path) != nil {
			kept = append(kept, e) // still running: retried next start
		}
	}
	rec.SelfUpdate.SetAside = kept
	saveStateAt(dir, rec)
	tidyInstallerCache()
}

// tidyInstallerCache removes cache\installer, and cache, when they are empty.
func tidyInstallerCache() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	pruneEmptyDirs(filepath.Join(installerCacheDir(dir), "backup"))
	removeIfEmpty(installerCacheDir(dir))
	removeIfEmpty(filepath.Dir(installerCacheDir(dir)))
}

// pruneEmptyDirs removes root and every folder under it that holds no file.
func pruneEmptyDirs(root string) {
	var dirs []string
	filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err == nil && d.IsDir() {
			dirs = append(dirs, path)
		}
		return nil
	})
	for i := len(dirs) - 1; i >= 0; i-- {
		removeIfEmpty(dirs[i])
	}
}

// startupTidy and startupKept hold what the data-folder cleanup removed and kept, printed by the
// command that reports it.
var startupTidy, startupKept []string

// startupCleanup runs at every start: it removes recorded replaced installer images and folds
// bookkeeping files a previous install placed into install.json.
func startupCleanup() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	sweepSetAside()
	exe, _ := executablePath()
	removeLeftovers("", dir, exe)
}

// reportStartupTidy prints, once, what the start-of-run cleanup removed.
func reportStartupTidy() {
	if len(startupTidy) == 0 && len(startupKept) == 0 {
		return
	}
	ui.LazySection("Tidying the data folder")
	list := ui.List("Removed")
	for _, line := range startupTidy {
		list.Add(line)
	}
	list.Close()
	kept := ui.List("Kept")
	for _, line := range startupKept {
		kept.Add(line)
	}
	kept.Close()
	startupTidy, startupKept = nil, nil
}

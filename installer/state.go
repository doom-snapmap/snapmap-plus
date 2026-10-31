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

// install.json is the installer's only bookkeeping file. It lives in %LOCALAPPDATA%\snapmap-plus and
// records the installed version, the DOOM folder, the files placed there, the executables placed in
// the data folder with their hashes, and the self-update state. The backend and the frontend read the
// first "version" key of this file for their reports, so Version stays the first field.
const stateName = "install.json"

// The bookkeeping files of earlier releases, folded into install.json and removed when found.
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
	// LegacyRemoved: original-SnapHak files a migration deleted (informational; carried forward).
	LegacyRemoved []string `json:"legacy_removed,omitempty"`
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

// setAside is a replaced executable moved out of the way because it was still running. It is removed
// at the next start while its bytes are unchanged and the executable it was replaced by exists.
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
		return nil, fmt.Errorf("%s is not readable (%v)", statePath(dir), err)
	}
	return &rec, nil
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
	if !stateExists(dir) {
		// Pre-rename installs kept the record at %LOCALAPPDATA%\open-snaphak\; fall back to it and migrate it
		// forward, so update / status / uninstall still recognise an existing install after the rename.
		op := oldRecordPath()
		od, oerr := os.ReadFile(op)
		if op == "" || oerr != nil {
			return nil, errNotInstalled
		}
		os.WriteFile(p, od, 0o644)
	}
	rec, err := loadStateAt(dir)
	if err != nil {
		return nil, err
	}
	if rec.DoomPath == "" {
		return nil, errNotInstalled
	}
	return rec, nil
}

// saveRecord replaces install.json with rec.
func saveRecord(rec *installRecord) error {
	p, err := recordPath()
	if err != nil {
		return err
	}
	return saveStateAt(filepath.Dir(p), rec)
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

// sweepSetAside removes the replaced images recorded by earlier runs: each one whose bytes are
// unchanged and whose replacement exists. A locked image stays recorded for the next start; one that
// changed is forgotten and left alone; one whose replacement is missing is a recovery copy and stays.
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

// startupCleanup runs at every start: it removes replaced installer images from earlier runs and
// retires the bookkeeping files of earlier releases into install.json.
func startupCleanup() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	sweepSetAside()
	exe, _ := executablePath()
	startupTidy = append(startupTidy, retireBookkeeping(dir, exe)...)
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

// retireBookkeeping folds installer-files.json and installer-versions.json into install.json and
// removes them, removes <exe>.updates.json files, and removes the <exe>.old images earlier releases
// kept beside an executable in the data folder or beside the running image while that executable
// exists (a locked one is moved into cache\installer and removed at the next start). It returns one
// line per file removed.
func retireBookkeeping(dir, running string) []string {
	var removed []string
	files := readRetiredExecutables(dir)
	versions := readRetiredVersions(dir)
	if len(files) > 0 || len(versions) > 0 {
		if err := updateStateAt(dir, func(r *installRecord) {
			if r.Executables == nil {
				r.Executables = map[string]exeEntry{}
			}
			for name, sum := range files {
				if _, have := r.Executables[name]; have || !executableName(name) || !ownsExecutable(filepath.Join(dir, name), sum) {
					continue
				}
				e := exeEntry{SHA256: sum}
				if v, ok := versions[name]; ok && strings.EqualFold(v.SHA256, sum) {
					e.Version = v.Version
				}
				r.Executables[name] = e
			}
			for name, v := range versions {
				if e, have := r.Executables[name]; have && e.Version == "" && strings.EqualFold(e.SHA256, v.SHA256) {
					e.Version = v.Version
					r.Executables[name] = e
				}
			}
			if len(r.Executables) == 0 {
				r.Executables = nil
			}
		}); err != nil {
			return removed // keep the old files until their contents can be carried over
		}
	}
	for _, name := range []string{retiredExecutableRecord, retiredVersionsRecord} {
		if os.Remove(filepath.Join(dir, name)) == nil {
			removed = append(removed, name+" (folded into install.json)")
		}
	}
	names := map[string]bool{selfExeAsset: true, storeToolName: true, legacyExeName: true}
	if rec, err := loadStateAt(dir); err == nil {
		for name := range rec.Executables {
			if executableName(name) {
				names[name] = true
			}
		}
	}
	exes := []string{}
	for name := range names {
		exes = append(exes, filepath.Join(dir, name))
	}
	if running != "" && !strings.EqualFold(filepath.Clean(filepath.Dir(running)), filepath.Clean(dir)) {
		exes = append(exes, running)
	}
	for _, exe := range exes {
		removed = append(removed, retireUpdateLeftovers(dir, exe)...)
	}
	// Temporary files of an interrupted save.
	if entries, err := os.ReadDir(dir); err == nil {
		for _, e := range entries {
			n := e.Name()
			if !e.IsDir() && (strings.HasPrefix(n, ".installer-files-") || strings.HasPrefix(n, ".install-json-")) {
				os.Remove(filepath.Join(dir, n))
			}
		}
	}
	return removed
}

// retireUpdateLeftovers removes exe's .updates.json and, while exe exists, its .old images.
func retireUpdateLeftovers(dir, exe string) []string {
	var removed []string
	label := func(p string) string {
		if strings.EqualFold(filepath.Clean(filepath.Dir(p)), filepath.Clean(dir)) {
			return filepath.Base(p)
		}
		return p
	}
	if os.Remove(exe+retiredUpdatesSuffix) == nil {
		removed = append(removed, label(exe+retiredUpdatesSuffix)+" (folded into install.json)")
	}
	st, err := os.Lstat(exe)
	if err != nil || !st.Mode().IsRegular() {
		return removed // the .old may be the only copy left: keep it
	}
	entries, err := os.ReadDir(filepath.Dir(exe))
	if err != nil {
		return removed
	}
	for _, e := range entries {
		if e.IsDir() || !updateBackupName(exe, e.Name()) {
			continue
		}
		old := filepath.Join(filepath.Dir(exe), e.Name())
		if !plainPath(old) {
			continue
		}
		if os.Remove(old) == nil {
			removed = append(removed, label(old)+" (an installer an earlier update replaced)")
			continue
		}
		// Still running: move it out of the data folder's top level and remove it next start.
		if moved, sum := moveAside(old, exe); moved != "" {
			if recordSetAside(moved, sum, exe) == nil {
				removed = append(removed, label(old)+" (moved into cache\\installer; it is removed at the next start)")
			}
		}
	}
	return removed
}

func readRetiredExecutables(dir string) map[string]string {
	m := map[string]string{}
	if data, err := os.ReadFile(filepath.Join(dir, retiredExecutableRecord)); err == nil {
		json.Unmarshal(data, &m)
	}
	return m
}

func readRetiredVersions(dir string) map[string]exeEntry {
	m := map[string]exeEntry{}
	if data, err := os.ReadFile(filepath.Join(dir, retiredVersionsRecord)); err == nil {
		json.Unmarshal(data, &m)
	}
	return m
}

// moveAside renames a replaced image into the installer cache under a free <name>.old[N] name and
// returns its new path and SHA-256, or "" when it could not be moved.
func moveAside(path, forExe string) (string, string) {
	dir := appDataDir()
	if dir == "" {
		return "", ""
	}
	sum, err := fileSHA256(path)
	if err != nil {
		return "", ""
	}
	cache := installerCacheDir(dir)
	if os.MkdirAll(cache, 0o755) != nil {
		return "", ""
	}
	for i := 1; i <= 10; i++ {
		name := filepath.Base(forExe) + ".old"
		if i > 1 {
			name = fmt.Sprintf("%s%d", name, i)
		}
		dst := filepath.Join(cache, name)
		if _, err := os.Lstat(dst); !os.IsNotExist(err) {
			continue
		}
		if os.Rename(path, dst) == nil {
			return dst, sum
		}
	}
	return "", ""
}

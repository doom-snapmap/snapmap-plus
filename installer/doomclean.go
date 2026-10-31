package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// The DOOM folder contract: after any install, update or migration the folder holds exactly the pair
// for the detected build and nothing else of ours. Neither XINPUT1_4.dll nor XINPUT1_3.dll exists in a
// vanilla DOOM folder, so no backup is made of a file of ours; the only file ever backed up is one that
// is not ours and is replaced by the pair, and that backup is kept in the data folder's
// cache\installer\backup, never in DOOM.

// legacyProxy and legacyUIDll are the 2018 build's pair (v0.2.1-beta.19).
const (
	legacyProxy = "XINPUT1_3.dll"
	legacyUIDll = `snapmap-plus\snapmap-plus-ui.dll`
)

// buildPair returns the two files the build's install holds.
func buildPair(build string) []string {
	if build == build2018 {
		return []string{legacyProxy, legacyUIDll}
	}
	return []string{currentProxy, currentUIDll}
}

// ourDoomNames are the DOOM-relative paths any Snapmap+ release placed: both builds' pairs.
var ourDoomNames = []string{currentProxy, currentUIDll, legacyProxy, legacyUIDll}

// ourBackupSuffixes are the suffixes earlier installers gave the backups they kept in DOOM.
var ourBackupSuffixes = []string{".snapmap-plus-bak", ".snaphak-bak"}

// runtimeDirs are the folders under DOOM\snapmap-plus the DLLs write while the game runs.
var runtimeDirs = []string{"logs", "crash"}

// ourMarker is a string every Snapmap+ DLL carries (the snapmap-plus\ paths it loads and logs to);
// neither the original SnapHak nor an unrelated XInput proxy does.
var ourMarker = []byte("snapmap-plus")

// carriesOurMarker reports whether the file's bytes hold ourMarker, as ASCII or UTF-16.
func carriesOurMarker(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 256<<20))
	if err != nil {
		return false
	}
	if bytes.Contains(data, ourMarker) {
		return true
	}
	wide := make([]byte, 0, 2*len(ourMarker))
	for _, c := range ourMarker {
		wide = append(wide, c, 0)
	}
	return bytes.Contains(data, wide)
}

// doomOwnership decides which files in a DOOM folder are ours: one the install record for this
// folder lists, one whose bytes are a file of the bundle at hand, or one that carries ourMarker.
type doomOwnership struct {
	recorded map[string]bool // lower-case DOOM-relative paths
	digests  map[string]bool // lower-case SHA-256
}

func newOwnership(doom string, rec *installRecord, files []manifestEntry) doomOwnership {
	o := doomOwnership{recorded: map[string]bool{}, digests: map[string]bool{}}
	if rec != nil && samePath(rec.DoomPath, doom) {
		for _, rel := range rec.Files {
			o.recorded[relKey(rel)] = true
		}
	}
	for _, e := range files {
		o.digests[strings.ToLower(e.sha256)] = true
	}
	return o
}

func relKey(rel string) string { return strings.ToLower(filepath.Clean(filepath.FromSlash(rel))) }

func samePath(a, b string) bool {
	return a != "" && b != "" && strings.EqualFold(filepath.Clean(a), filepath.Clean(b))
}

// byContent reports whether the file at path is ours by its bytes.
func (o doomOwnership) byContent(path string) bool {
	if sum, err := fileSHA256(path); err == nil && o.digests[strings.ToLower(sum)] {
		return true
	}
	return carriesOurMarker(path)
}

// ours reports whether the file at DOOM-relative rel is ours.
func (o doomOwnership) ours(doom, rel string) bool {
	return o.recorded[relKey(rel)] || o.byContent(filepath.Join(doom, rel))
}

// doomTidy is what cleanDoomFolder did.
type doomTidy struct {
	removed []string // files of ours removed
	moved   []string // backups of files that were not ours, moved into cache\installer
	foreign []string // files that are not ours, left in place
}

func (t *doomTidy) addForeign(rel string) {
	for _, f := range t.foreign {
		if strings.EqualFold(f, rel) {
			return
		}
	}
	t.foreign = append(t.foreign, rel)
}

func isRegular(path string) bool {
	st, err := os.Lstat(path)
	return err == nil && st.Mode().IsRegular()
}

func containsRel(list []string, rel string) bool {
	for _, r := range list {
		if relKey(r) == relKey(rel) {
			return true
		}
	}
	return false
}

// cleanDoomFolder brings doom to the contract for the pair keep (nil for uninstall): every file of
// ours outside keep is removed, wherever an earlier release put it; backups of our own files are
// deleted; backups of files that were not ours move into cache\installer and stay recorded in rec so
// uninstall restores them; files that are not ours stay and are listed. rec.Files and rec.Backups
// are updated to match.
func cleanDoomFolder(doom string, keep []string, own doomOwnership, rec *installRecord) doomTidy {
	var t doomTidy
	gone := func(rel string) {
		t.removed = append(t.removed, rel)
		kept := rec.Files[:0]
		for _, f := range rec.Files {
			if relKey(f) != relKey(rel) {
				kept = append(kept, f)
			}
		}
		rec.Files = kept
	}
	remove := func(rel string) {
		if err := os.Remove(filepath.Join(doom, rel)); err == nil {
			gone(rel)
			if dir := filepath.Dir(rel); dir != "." && !strings.EqualFold(dir, "snapmap-plus") {
				removeIfEmpty(filepath.Join(doom, dir)) // a folder an earlier release created, now empty
			}
		} else if !os.IsNotExist(err) {
			ui.Warn("couldn't remove %s (%v) -- is DOOM still running?", rel, err)
		}
	}

	// 1) Our files at the known names, and the files the record lists, outside the pair.
	candidates := append([]string{}, ourDoomNames...)
	for _, rel := range rec.Files {
		if !containsRel(candidates, rel) {
			candidates = append(candidates, rel)
		}
	}
	for _, rel := range candidates {
		if containsRel(keep, rel) || !isRegular(filepath.Join(doom, rel)) {
			continue
		}
		if own.ours(doom, rel) {
			remove(rel)
		} else {
			t.addForeign(rel)
		}
	}
	// A recorded file that is already gone leaves the record too.
	kept := rec.Files[:0]
	for _, f := range rec.Files {
		if containsRel(keep, f) || isRegular(filepath.Join(doom, f)) {
			kept = append(kept, f)
		}
	}
	rec.Files = kept

	// 2) Backups earlier installers kept in DOOM, at the top level and under snapmap-plus\.
	for _, dir := range []string{"", "snapmap-plus"} {
		entries, _ := os.ReadDir(filepath.Join(doom, dir))
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			base := ""
			for _, suf := range ourBackupSuffixes {
				if strings.HasSuffix(strings.ToLower(e.Name()), suf) {
					base = e.Name()[:len(e.Name())-len(suf)]
				}
			}
			if base == "" {
				continue
			}
			bakRel := filepath.Join(dir, e.Name())
			baseRel := filepath.Join(dir, base)
			bakPath := filepath.Join(doom, bakRel)
			if own.byContent(bakPath) {
				if err := os.Remove(bakPath); err == nil {
					t.removed = append(t.removed, bakRel)
					dropBackup(rec, bakPath)
				} else {
					ui.Warn("couldn't remove %s (%v)", bakRel, err)
				}
				continue
			}
			for _, bk := range rec.Backups {
				if samePath(bk.Backup, bakPath) {
					baseRel = bk.Rel // the record names the file as it was
				}
			}
			dst, err := keepBackup(bakPath, baseRel)
			if err != nil {
				ui.Warn("couldn't move %s out of the DOOM folder (%v); it stays there", bakRel, err)
				t.addForeign(bakRel)
				continue
			}
			dropBackup(rec, bakPath)
			if !hasBackupFor(rec, baseRel) {
				rec.Backups = append(rec.Backups, backup{Rel: baseRel, Backup: dst})
			}
			t.moved = append(t.moved, bakRel)
		}
	}
	// Recorded backups whose copy is gone are forgotten.
	keptB := rec.Backups[:0]
	for _, bk := range rec.Backups {
		if isRegular(bk.Backup) {
			keptB = append(keptB, bk)
		}
	}
	rec.Backups = keptB

	// 3) Anything else under snapmap-plus\ besides the pair and the runtime folders.
	sub := filepath.Join(doom, "snapmap-plus")
	entries, _ := os.ReadDir(sub)
	for _, e := range entries {
		rel := filepath.Join("snapmap-plus", e.Name())
		if containsRel(keep, rel) {
			continue
		}
		if e.IsDir() {
			isRuntime := false
			for _, d := range runtimeDirs {
				isRuntime = isRuntime || strings.EqualFold(e.Name(), d)
			}
			if !isRuntime {
				t.addForeign(rel + `\`)
			}
			continue
		}
		if !isRegular(filepath.Join(doom, rel)) {
			continue
		}
		if own.ours(doom, rel) {
			remove(rel)
		} else {
			t.addForeign(rel)
		}
	}
	removeIfEmpty(sub)

	// 4) Other proxy-shaped files at the top level, which are someone else's: list them.
	top, _ := os.ReadDir(doom)
	for _, e := range top {
		n := strings.ToLower(e.Name())
		if e.IsDir() || containsRel(keep, e.Name()) {
			continue
		}
		for _, p := range []string{"dinput8.dll", "xinput", "winmm.dll", "snaphak", "snapmap-plus"} {
			if strings.HasPrefix(n, p) {
				t.addForeign(e.Name())
				break
			}
		}
	}
	return t
}

func dropBackup(rec *installRecord, path string) {
	kept := rec.Backups[:0]
	for _, bk := range rec.Backups {
		if !samePath(bk.Backup, path) {
			kept = append(kept, bk)
		}
	}
	rec.Backups = kept
}

func hasBackupFor(rec *installRecord, rel string) bool {
	for _, bk := range rec.Backups {
		if relKey(bk.Rel) == relKey(rel) {
			return true
		}
	}
	return false
}

// keepBackup moves a file that is not ours into cache\installer\backup\<rel> (a free name when one is
// already there) and returns where it went.
func keepBackup(src, rel string) (string, error) {
	dir := appDataDir()
	if dir == "" {
		return "", fmt.Errorf("the LOCALAPPDATA environment variable is not set")
	}
	base := filepath.Join(installerCacheDir(dir), "backup", rel)
	dst := base
	for i := 2; ; i++ {
		if _, err := os.Lstat(dst); os.IsNotExist(err) {
			break
		}
		if i > 50 {
			return "", fmt.Errorf("no free backup name for %s", rel)
		}
		dst = fmt.Sprintf("%s.%d", base, i)
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return "", err
	}
	if err := moveFile(src, dst); err != nil {
		return "", err
	}
	return dst, nil
}

// moveFile renames src to dst, or copies, verifies and removes src when they are on different volumes.
func moveFile(src, dst string) error {
	if os.Rename(src, dst) == nil {
		return nil
	}
	if err := copyFile(src, dst); err != nil {
		os.Remove(dst)
		return err
	}
	if !identicalFiles(src, dst) {
		os.Remove(dst)
		return fmt.Errorf("the copy of %s could not be verified", src)
	}
	if err := os.Remove(src); err != nil {
		os.Remove(dst)
		return err
	}
	return nil
}

// printDoomTidy reports cleanDoomFolder's work under the current section.
func printDoomTidy(t doomTidy, dataDir string) {
	list := ui.List("Removed files of ours")
	for _, rel := range t.removed {
		list.Add(rel)
	}
	list.Close()
	if len(t.moved) > 0 {
		moved := ui.List("Backups of files that were not ours, moved to " + filepath.Join(installerCacheDir(dataDir), "backup"))
		for _, rel := range t.moved {
			moved.Add(rel)
		}
		moved.Close()
	}
	printForeign(t.foreign)
}

func printForeign(foreign []string) {
	list := ui.List("Not ours, left in place")
	for _, rel := range foreign {
		list.Add(rel)
	}
	list.Close()
}

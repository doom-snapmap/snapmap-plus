package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// The executables this installer places in the data folder (snapmap-plus.exe, snapmap-plus-store.exe)
// are recorded in install.json by name with their SHA-256, so uninstall never deletes, and an update
// never overwrites, a file the player put at the same name.

// executableRecord maps an executable's file name to the SHA-256 of the bytes this installer placed.
type executableRecord map[string]string

func executableName(name string) bool {
	return name != "" && filepath.Base(name) == name && !strings.ContainsAny(name, `:/\`) && strings.EqualFold(filepath.Ext(name), ".exe")
}

// loadExecutableRecord reads dir's recorded executables, including any still listed in the retired
// installer-files.json.
func loadExecutableRecord(dir string) (executableRecord, error) {
	rec, err := loadStateAt(dir)
	if err != nil {
		return nil, err
	}
	out := executableRecord{}
	for name, sum := range readRetiredExecutables(dir) {
		out[name] = sum
	}
	for name, e := range rec.Executables {
		out[name] = e.SHA256
	}
	return out, nil
}

// saveExecutableRecord replaces dir's recorded executables with m, keeping each recorded version
// whose bytes are unchanged.
func saveExecutableRecord(dir string, m executableRecord) error {
	err := updateStateAt(dir, func(r *installRecord) {
		next := map[string]exeEntry{}
		for name, sum := range m {
			e := exeEntry{SHA256: sum}
			if old, ok := r.Executables[name]; ok && strings.EqualFold(old.SHA256, sum) {
				e.Version = old.Version
			}
			next[name] = e
		}
		if len(next) == 0 {
			next = nil
		}
		r.Executables = next
	})
	if err == nil {
		os.Remove(filepath.Join(dir, retiredExecutableRecord)) // its entries are in install.json now
	}
	return err
}

func ownsExecutable(path, digest string) bool {
	if digest == "" || !plainPath(path) {
		return false
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	actual, err := fileSHA256(path)
	return err == nil && strings.EqualFold(actual, digest)
}

// Only overwrite a recorded, unchanged copy or adopt identical existing bytes.
func installSelfCopy(src, dst string) error {
	dir, name := filepath.Dir(dst), filepath.Base(dst)
	if !executableName(name) || !plainPath(dst) {
		return fmt.Errorf("unsafe installer copy path: %s", dst)
	}
	rec, err := loadExecutableRecord(dir)
	if err != nil {
		return err
	}
	if identicalFiles(src, dst) {
		// An earlier installer may have created this copy before records existed.
	} else if _, err := os.Lstat(dst); os.IsNotExist(err) {
		if err := copyMissingFile(src, dst); err != nil {
			return err
		}
	} else if ownsExecutable(dst, rec[name]) {
		if err := replaceExe(dst, src); err != nil {
			return err
		}
	} else {
		return fmt.Errorf("kept unowned or modified executable: %s", dst)
	}
	if !identicalFiles(src, dst) {
		return fmt.Errorf("installer copy could not be verified: %s", dst)
	}
	rec[name], err = fileSHA256(dst)
	if err != nil {
		return err
	}
	return saveExecutableRecord(dir, rec)
}

// cleanupOwnedExecutables removes every recorded executable in dir whose bytes are unchanged, except the
// running image, and forgets the ones that changed or are gone. The running image stays recorded.
func cleanupOwnedExecutables(dir, running string) {
	rec, err := loadExecutableRecord(dir)
	if err != nil {
		return
	}
	for name, digest := range rec {
		if !executableName(name) {
			delete(rec, name)
			continue
		}
		path := filepath.Join(dir, name)
		if running == "" || sameFile(running, path) {
			continue
		}
		if !ownsExecutable(path, digest) {
			delete(rec, name)
			continue
		}
		retireUpdateLeftovers(dir, path)
		if os.Remove(path) == nil {
			delete(rec, name)
		}
	}
	saveExecutableRecord(dir, rec)
}

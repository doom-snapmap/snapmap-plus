package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const executableRecordName = "installer-files.json"

// The digest prevents uninstall from deleting a user's replacement at the same name.
type executableRecord map[string]string

func executableName(name string) bool {
	return name != "" && filepath.Base(name) == name && !strings.ContainsAny(name, `:/\`) && strings.EqualFold(filepath.Ext(name), ".exe")
}

func loadExecutableRecord(dir string) (executableRecord, error) {
	data, err := os.ReadFile(filepath.Join(dir, executableRecordName))
	if os.IsNotExist(err) {
		return executableRecord{}, nil
	}
	if err != nil {
		return nil, err
	}
	var rec executableRecord
	if err := json.Unmarshal(data, &rec); err != nil {
		return nil, err
	}
	if rec == nil {
		rec = executableRecord{}
	}
	return rec, nil
}

func saveExecutableRecord(dir string, rec executableRecord) error {
	return saveJSONFile(filepath.Join(dir, executableRecordName), rec)
}

func saveJSONFile(path string, value any) error {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".installer-files-*")
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

func ownsExecutable(path, digest string) bool {
	if digest == "" || !plainPath(path) {
		return false
	}
	info, err := os.Lstat(path)
	if err != nil || !info.Mode().IsRegular() {
		return false
	}
	actual, err := fileSHA256(path)
	return err == nil && actual == digest
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
		removeOldLeftovers(path)
		if os.Remove(path) == nil {
			delete(rec, name)
		}
	}
	if len(rec) == 0 {
		os.Remove(filepath.Join(dir, executableRecordName))
	} else {
		saveExecutableRecord(dir, rec)
	}
}

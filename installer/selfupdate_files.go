package main

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

type updateRecord struct {
	Current string            `json:"current_sha256"`
	Backups map[string]string `json:"backups"`
}

func loadUpdateRecord(exe string) (*updateRecord, error) {
	rec := &updateRecord{Backups: map[string]string{}}
	data, err := os.ReadFile(exe + ".updates.json")
	if os.IsNotExist(err) {
		return rec, nil
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, rec); err != nil {
		return nil, err
	}
	if decoded, err := hex.DecodeString(rec.Current); err != nil || len(decoded) != 32 {
		return nil, fmt.Errorf("unrecognized executable update record")
	}
	if rec.Backups == nil {
		rec.Backups = map[string]string{}
	}
	return rec, nil
}

// Clear a stale claim before reusing its name, including when publication fails.
func forgetUpdateBackup(exe, backup string) error {
	rec, err := loadUpdateRecord(exe)
	if err != nil {
		return err
	}
	name := filepath.Base(backup)
	if _, exists := rec.Backups[name]; !exists {
		return nil
	}
	delete(rec.Backups, name)
	return saveJSONFile(exe+".updates.json", rec)
}

// Only a completed replacement makes a backup eligible for later cleanup.
func recordUpdateBackup(exe, backup string) error {
	rec, err := loadUpdateRecord(exe)
	if err != nil {
		return err
	}
	rec.Current, err = fileSHA256(exe)
	if err != nil {
		return err
	}
	digest, err := fileSHA256(backup)
	if err != nil {
		return err
	}
	rec.Backups[filepath.Base(backup)] = digest
	return saveJSONFile(exe+".updates.json", rec)
}

func updateBackupName(exe, name string) bool {
	if filepath.Base(name) != name {
		return false
	}
	suffix, matches := strings.CutPrefix(name, filepath.Base(exe)+".old")
	if !matches {
		return false
	}
	n, err := strconv.Atoi(suffix)
	return suffix == "" || (err == nil && n >= 2 && n <= 10 && strconv.Itoa(n) == suffix)
}

func removeOldLeftovers(exe string) {
	rec, err := loadUpdateRecord(exe)
	if err != nil || !ownsExecutable(exe, rec.Current) {
		return // A missing or changed current image may still need its recovery copies.
	}
	for name, digest := range rec.Backups {
		path := filepath.Join(filepath.Dir(exe), name)
		if !updateBackupName(exe, name) || !ownsExecutable(path, digest) {
			continue
		}
		if os.Remove(path) == nil {
			delete(rec.Backups, name)
		}
	}
	if len(rec.Backups) == 0 {
		os.Remove(exe + ".updates.json")
	} else {
		saveJSONFile(exe+".updates.json", rec)
	}
}

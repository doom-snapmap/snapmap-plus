package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

func earlierPathExists(path string) bool {
	_, err := os.Lstat(path)
	return !os.IsNotExist(err)
}

// Windows can refuse a directory rename while another reader still has a child open.
func renameEarlierFolder(src, dst string) error {
	for attempt := 0; ; attempt++ {
		err := os.Rename(src, dst)
		if err == nil || runtime.GOOS != "windows" || attempt == 6 ||
			!(errors.Is(err, fs.ErrPermission) || errors.Is(err, syscall.Errno(32))) {
			return err
		}
		if _, statErr := os.Lstat(dst); !errors.Is(statErr, fs.ErrNotExist) {
			return err
		}
		time.Sleep(25 * time.Millisecond << attempt)
	}
}

type earlierModPlan struct {
	Source      string `json:"source"`
	Destination string `json:"destination"`
	Original    string `json:"original"`
}

func earlierTreeHash(dir string) (string, error) {
	h := sha256.New()
	err := filepath.WalkDir(dir, func(p string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(p) {
			return fmt.Errorf("cannot preserve linked mod file %s", p)
		}
		rel, err := filepath.Rel(dir, p)
		if err != nil {
			return err
		}
		fmt.Fprintf(h, "%s\x00", filepath.ToSlash(rel))
		if e.IsDir() {
			return nil
		}
		if !e.Type().IsRegular() {
			return fmt.Errorf("unreadable mod file %s", p)
		}
		sha, err := fileSHA256(p)
		if err != nil {
			return err
		}
		fmt.Fprint(h, sha, "\x00")
		return nil
	})
	return hex.EncodeToString(h.Sum(nil)), err
}

func earlierExtraFiles(src, dst string, consumed map[string]bool) error {
	return filepath.WalkDir(src, func(p string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, p)
		if err != nil || rel == "." {
			return err
		}
		if consumed[p] {
			return nil
		}
		if !plainPath(p) {
			return fmt.Errorf("cannot copy linked file %s", p)
		}
		to := filepath.Join(dst, rel)
		if e.IsDir() {
			return nil
		}
		if !e.Type().IsRegular() {
			return fmt.Errorf("unreadable file %s", p)
		}
		if earlierPathExists(to) {
			if identicalFiles(to, p) {
				return nil
			}
			return fmt.Errorf("existing author file %s differs from the translated asset", rel)
		}
		if err := os.MkdirAll(filepath.Dir(to), 0755); err != nil {
			return err
		}
		return copyFile(p, to)
	})
}

// Read interrupted records left by the earlier installer without changing its folders.
func (p *migrationPlan) previousTransactions() error {
	root := filepath.Join(p.data, "backups", "earlier-data", "mod-folders")
	entries, err := os.ReadDir(root)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	for _, entry := range entries {
		folder := filepath.Join(root, entry.Name())
		planFile := filepath.Join(folder, "plan.json")
		if !entry.IsDir() || !plainPath(planFile) || !isRegular(planFile) {
			continue
		}
		body, err := os.ReadFile(planFile)
		if err != nil {
			return err
		}
		var prior earlierModPlan
		if json.Unmarshal(body, &prior) != nil || !filepath.IsLocal(prior.Source) || !filepath.IsLocal(prior.Destination) ||
			!strings.HasPrefix(strings.ToLower(filepath.ToSlash(prior.Destination)), "mods/") || !pinHash(prior.Original) {
			return fmt.Errorf("unrecognized earlier transaction at %s", folder)
		}
		source := filepath.Join(p.data, prior.Source)
		dest := filepath.Join(p.data, prior.Destination)
		if !under(filepath.Join(p.data, "mods"), source) && !under(filepath.Join(p.data, "overrides"), source) {
			return fmt.Errorf("invalid original mod path %s", source)
		}
		original := filepath.Join(folder, "original")
		if !earlierPathExists(original) {
			continue
		}
		h, err := earlierTreeHash(original)
		if err != nil || h != prior.Original {
			return fmt.Errorf("the original mod in %s differs", folder)
		}
		if earlierPathExists(source) && !samePath(source, dest) {
			return fmt.Errorf("another mod uses %s", source)
		}
		if isRegular(filepath.Join(dest, "mod.json")) {
			continue
		}
		stage := filepath.Join(folder, "converted")
		if !earlierPathExists(stage) {
			if !isRegular(filepath.Join(dest, "mod.json")) {
				return fmt.Errorf("incomplete earlier transaction at %s", folder)
			}
			continue
		}
		start := len(p.Changes)
		if err = p.transferTree(stage, dest, "Mods", false); err != nil {
			return err
		}
		p.noteModChanges(dest, start)
	}
	return nil
}

func convertEarlierFolders(data string) error { return convertEarlierData("", data) }

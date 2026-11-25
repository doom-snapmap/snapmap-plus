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

func earlierExtraFiles(src, dst string) error {
	packageFolder := isRegular(filepath.Join(src, "package.json"))
	return filepath.WalkDir(src, func(p string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, p)
		if err != nil || rel == "." {
			return err
		}
		first := strings.ToLower(strings.SplitN(filepath.ToSlash(rel), "/", 2)[0])
		switch first {
		case "decls", "images", "shaders", "requirements", "strings", "hud", "resources":
			if packageFolder && e.IsDir() {
				return filepath.SkipDir
			}
		case "assets", ".build", "bundle.json", "bundle.canonical.json", "mod.json", "package.json", ".state.json":
			if e.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if !plainPath(p) {
			return fmt.Errorf("cannot copy linked file %s", p)
		}
		to := filepath.Join(dst, rel)
		if e.IsDir() {
			return os.MkdirAll(to, 0755)
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
		return copyFile(p, to)
	})
}

func finishEarlierPlan(data, folder string, plan earlierModPlan) error {
	source := strings.ToLower(filepath.ToSlash(filepath.Clean(plan.Source)))
	target := strings.ToLower(filepath.ToSlash(filepath.Clean(plan.Destination)))
	if !filepath.IsLocal(plan.Source) || !filepath.IsLocal(plan.Destination) || !(strings.HasPrefix(source, "mods/") || strings.HasPrefix(source, "overrides/")) || !strings.HasPrefix(target, "mods/") || len(plan.Original) != 64 {
		return fmt.Errorf("unreadable earlier mod conversion at %s", folder)
	}
	src, dst := filepath.Join(data, plan.Source), filepath.Join(data, plan.Destination)
	backup, stage := filepath.Join(folder, "original"), filepath.Join(folder, "converted")
	for _, p := range []string{src, dst, backup, stage} {
		if !plainPath(p) {
			return fmt.Errorf("linked conversion path %s", p)
		}
	}
	inPlace := strings.EqualFold(filepath.Clean(src), filepath.Clean(dst))
	if !isRegular(filepath.Join(folder, "plan.json")) {
		return fmt.Errorf("unreadable mod conversion record")
	}
	if !earlierPathExists(stage) {
		if !earlierPathExists(dst) {
			return fmt.Errorf("neither the converted folder nor %s is present", dst)
		}
		return os.Rename(filepath.Join(folder, "plan.json"), filepath.Join(folder, "finished.json"))
	}
	if !earlierPathExists(backup) {
		got, err := earlierTreeHash(src)
		if err != nil {
			return err
		}
		if got != plan.Original {
			return fmt.Errorf("%s was edited during conversion", src)
		}
		if !inPlace && earlierPathExists(dst) {
			return fmt.Errorf("a different mod now occupies %s", dst)
		}
		if err = renameEarlierFolder(src, backup); err != nil {
			return err
		}
	}
	got, err := earlierTreeHash(backup)
	if err != nil {
		return err
	}
	if got != plan.Original {
		if !earlierPathExists(src) {
			if err := renameEarlierFolder(backup, src); err != nil {
				return err
			}
		}
		return fmt.Errorf("%s changed during conversion", src)
	}
	if earlierPathExists(dst) {
		return fmt.Errorf("a different mod now occupies %s; preserved the original in %s", dst, backup)
	}
	if err = os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	if err = renameEarlierFolder(stage, dst); err != nil {
		return err
	}
	return os.Rename(filepath.Join(folder, "plan.json"), filepath.Join(folder, "finished.json"))
}

func convertEarlierFolders(data string) error {
	records := filepath.Join(data, "backups", "earlier-data", "mod-folders")
	if !plainPath(records) {
		return fmt.Errorf("linked mod backup folder %s", records)
	}
	entries, err := os.ReadDir(records)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	var failures []error
	for _, e := range entries {
		folder := filepath.Join(records, e.Name())
		p := filepath.Join(folder, "plan.json")
		if !plainPath(p) || !isRegular(p) {
			continue
		}
		b, err := os.ReadFile(p)
		var plan earlierModPlan
		if err == nil {
			err = json.Unmarshal(b, &plan)
		}
		if err == nil {
			err = finishEarlierPlan(data, folder, plan)
		}
		if err != nil {
			failures = append(failures, err)
		}
	}
	if len(failures) > 0 {
		return errors.Join(failures...)
	}
	type candidate struct{ src, dst string }
	var mods []candidate
	held := map[string]string{}
	for _, where := range []string{"mods", "overrides"} {
		root := filepath.Join(data, where)
		if !plainPath(root) {
			return fmt.Errorf("linked mod library %s", root)
		}
		err = filepath.WalkDir(root, func(dir string, e fs.DirEntry, walkErr error) error {
			if os.IsNotExist(walkErr) && dir == root {
				return nil
			}
			if walkErr != nil {
				return walkErr
			}
			if !e.IsDir() || dir == root {
				return nil
			}
			name := e.Name()
			if !plainPath(dir) || strings.HasPrefix(name, ".") || modWorkFolder.MatchString(name) || (filepath.Dir(dir) == root && strings.EqualFold(name, "maps")) {
				return filepath.SkipDir
			}
			stat, err := os.Stat(filepath.Join(dir, "assets"))
			old := err == nil && stat.IsDir()
			if err != nil && !os.IsNotExist(err) {
				return err
			}
			doc, descriptor, err := earlierDescriptor(dir)
			if err != nil {
				if old || where == "overrides" {
					return err
				}
				return filepath.SkipDir
			}
			if doc == nil {
				return nil
			}
			if descriptor == "package.json" {
				old = old || where == "overrides" || isRegular(filepath.Join(dir, "package.json")) && (earlierPathExists(filepath.Join(dir, "decls")) || earlierPathExists(filepath.Join(dir, "images")) || earlierPathExists(filepath.Join(dir, "resources")))
				if doc["id"] == nil {
					doc["id"] = filepath.Base(dir)
				}
			}
			id, salt, err := earlierModIdentity(doc)
			if err != nil {
				if old {
					failures = append(failures, fmt.Errorf("%s: %w", dir, err))
				}
				return filepath.SkipDir
			}
			if prior := held[salt]; prior != "" && prior != id {
				return fmt.Errorf("two mod ids would share namespace %s", salt)
			}
			held[salt] = id
			if old {
				dst := dir
				if where == "overrides" {
					rel, _ := filepath.Rel(root, dir)
					dst = filepath.Join(data, "mods", rel)
				}
				mods = append(mods, candidate{dir, dst})
			}
			return filepath.SkipDir
		})
		if err != nil {
			failures = append(failures, err)
		}
	}
	if len(failures) > 0 {
		return errors.Join(failures...)
	}
	for _, m := range mods {
		if err := prepareEarlierFolder(data, records, m.src, m.dst); err != nil {
			failures = append(failures, fmt.Errorf("%s: %w", m.src, err))
		}
	}
	return errors.Join(failures...)
}

func prepareEarlierFolder(data, records, src, dst string) error {
	if !strings.EqualFold(src, dst) && earlierPathExists(dst) {
		return fmt.Errorf("an existing mod already uses %s", dst)
	}
	digest, err := earlierTreeHash(src)
	if err != nil {
		return err
	}
	rel, _ := filepath.Rel(data, src)
	target, _ := filepath.Rel(data, dst)
	if err = os.MkdirAll(records, 0755); err != nil {
		return err
	}
	work, err := os.MkdirTemp(records, ".preparing-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(work)
	stage := filepath.Join(work, "converted")
	if _, err = translateEarlierMod(src, stage); err != nil {
		return err
	}
	if err = earlierExtraFiles(src, stage); err != nil {
		return err
	}
	plan := earlierModPlan{rel, target, digest}
	text, _ := json.Marshal(plan)
	if err = writeAtomicFile(filepath.Join(work, "plan.json"), text); err != nil {
		return err
	}
	key := sha256.Sum256([]byte(rel + "\x00" + digest))
	record := filepath.Join(records, hex.EncodeToString(key[:]))
	if err = renameEarlierFolder(work, record); err != nil {
		return err
	}
	return finishEarlierPlan(data, record, plan)
}

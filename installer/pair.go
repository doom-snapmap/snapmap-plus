package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// The pair goes into DOOM as one change, so the folder never holds one release's backend beside
// another release's frontend: both files are written beside their targets under temporary names and
// checked against the manifest, then each target is swapped for its staged file, and a failed swap
// puts back every target already replaced.

// Temporary names in the DOOM folder: a staged new file, and a replaced one until it is removed.
const (
	stagedPrefix  = ".snapmap-plus-new-"
	replacePrefix = ".snapmap-plus-old-"
)

// stagedFile is one file of the pair, written beside its target.
type stagedFile struct {
	rel, target, staged string
	aside               string // the replaced target while the swap runs
}

// pairRename is the rename the swap uses; tests replace it to fail one step.
var pairRename = os.Rename

// stagePair writes each of files from root beside its target in doom and checks its SHA-256. On
// failure nothing staged is left behind.
func stagePair(doom, root string, files []manifestEntry) ([]stagedFile, error) {
	var out []stagedFile
	for _, e := range files {
		target := filepath.Join(doom, e.rel)
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			discardStaged(out)
			return nil, fmt.Errorf("couldn't create a folder in your DOOM folder (%v) -- check you have permission (try running as administrator)", err)
		}
		tmp, err := os.CreateTemp(filepath.Dir(target), stagedPrefix+"*")
		if err == nil {
			err = tmp.Close()
		}
		if err == nil {
			out = append(out, stagedFile{rel: e.rel, target: target, staged: tmp.Name()})
			err = copyFile(filepath.Join(root, e.rel), tmp.Name())
		}
		if err == nil {
			if got, herr := fileSHA256(tmp.Name()); herr != nil || got != e.sha256 {
				err = errors.Join(herr, errors.New("the written copy does not match the release"))
			}
		}
		if err != nil {
			discardStaged(out)
			return nil, fmt.Errorf("couldn't write %s into your DOOM folder (%v); nothing was changed -- check you have permission (try running as administrator)", e.rel, err)
		}
	}
	return out, nil
}

// swapPair puts every staged file in place. When one cannot be, the targets already replaced are put
// back and the error names the file.
func swapPair(files []stagedFile) error {
	for i := range files {
		f := &files[i]
		if isRegular(f.target) {
			aside, err := freeName(filepath.Dir(f.target), replacePrefix)
			if err == nil {
				err = pairRename(f.target, aside)
			}
			if err != nil {
				return rollbackPair(files[:i], fmt.Errorf("%s: %v", f.rel, err))
			}
			f.aside = aside
		}
		if err := pairRename(f.staged, f.target); err != nil {
			return rollbackPair(files[:i+1], fmt.Errorf("%s: %v", f.rel, err))
		}
		f.staged = ""
	}
	for _, f := range files {
		if f.aside != "" && os.Remove(f.aside) != nil {
			ui.Warn("couldn't remove the replaced copy of %s at %s; delete it once DOOM is closed", f.rel, f.aside)
		}
	}
	return nil
}

// rollbackPair undoes the swaps of done, newest first: a placed file is removed and the target it
// replaced is renamed back.
func rollbackPair(done []stagedFile, cause error) error {
	for i := len(done) - 1; i >= 0; i-- {
		f := done[i]
		if f.staged == "" {
			os.Remove(f.target)
		}
		if f.aside != "" {
			if err := os.Rename(f.aside, f.target); err != nil {
				return fmt.Errorf("%v; the earlier %s could not be put back and is at %s -- rename it to %s", cause, f.rel, f.aside, filepath.Base(f.target))
			}
		}
	}
	return cause
}

// discardStaged removes the staged files that were not put in place.
func discardStaged(files []stagedFile) {
	for _, f := range files {
		if f.staged != "" {
			os.Remove(f.staged)
		}
	}
}

// freeName returns an unused path in dir that starts with prefix.
func freeName(dir, prefix string) (string, error) {
	f, err := os.CreateTemp(dir, prefix+"*")
	if err != nil {
		return "", err
	}
	name := f.Name()
	f.Close()
	return name, os.Remove(name)
}

// restoreKept moves the files that belonged to other programs back from the data folder, after an
// install that moved them aside did not finish.
func restoreKept(doom string, kept []backup) {
	for _, bk := range kept {
		if err := moveFile(bk.Backup, filepath.Join(doom, bk.Rel)); err != nil {
			ui.Warn("couldn't put back %s (%v); it is kept at %s -- copy it into your DOOM folder", bk.Rel, err, bk.Backup)
		}
	}
}

// pairLeftover reports a temporary name the pair's install uses, left by an install that was stopped.
func pairLeftover(name string) bool {
	n := strings.ToLower(name)
	return strings.HasPrefix(n, stagedPrefix) || strings.HasPrefix(n, replacePrefix)
}

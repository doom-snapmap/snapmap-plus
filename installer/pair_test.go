package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// pairRig is a DOOM folder holding an earlier release's pair and a bundle of a newer one.
func pairRig(t *testing.T) (doom, root string, files []manifestEntry) {
	t.Helper()
	tmp := t.TempDir()
	doom, root = filepath.Join(tmp, "DOOM"), filepath.Join(tmp, "dist")
	for _, rel := range []string{currentProxy, currentUIDll} {
		writeF(t, filepath.Join(doom, rel), "old "+rel)
		writeF(t, filepath.Join(root, rel), "new "+rel)
		files = append(files, manifestEntry{rel: rel, sha256: sha256Hex([]byte("new " + rel))})
	}
	return doom, root, files
}

// pairContents returns the DOOM folder's pair, and fails on any temporary file left in it.
func pairContents(t *testing.T, doom string) []string {
	t.Helper()
	var got []string
	for _, rel := range []string{currentProxy, currentUIDll} {
		got = append(got, readF(t, filepath.Join(doom, rel)))
	}
	for _, dir := range []string{doom, filepath.Join(doom, "snapmap-plus")} {
		entries, _ := os.ReadDir(dir)
		for _, e := range entries {
			if pairLeftover(e.Name()) {
				t.Errorf("left %s in %s", e.Name(), dir)
			}
		}
	}
	return got
}

func TestPairSwapsBothFiles(t *testing.T) {
	doom, root, files := pairRig(t)
	staged, err := stagePair(doom, root, files)
	if err != nil {
		t.Fatal(err)
	}
	if err := swapPair(staged); err != nil {
		t.Fatal(err)
	}
	if got := pairContents(t, doom); got[0] != "new "+currentProxy || got[1] != "new "+currentUIDll {
		t.Errorf("pair = %q", got)
	}
}

// A failure while the second file is put in place restores the first: the folder keeps one
// release's pair, never a mixed one.
func TestPairRollsBackWhenTheSecondFileFails(t *testing.T) {
	doom, root, files := pairRig(t)
	staged, err := stagePair(doom, root, files)
	if err != nil {
		t.Fatal(err)
	}
	saved := pairRename
	pairRename = func(src, dst string) error {
		if strings.HasPrefix(filepath.Base(src), stagedPrefix) && strings.EqualFold(filepath.Base(dst), filepath.Base(currentUIDll)) {
			return errors.New("injected failure")
		}
		return os.Rename(src, dst)
	}
	defer func() { pairRename = saved }()
	err = swapPair(staged)
	discardStaged(staged)
	if err == nil || !strings.Contains(err.Error(), "injected failure") {
		t.Fatalf("swap error = %v", err)
	}
	if got := pairContents(t, doom); got[0] != "old "+currentProxy || got[1] != "old "+currentUIDll {
		t.Errorf("pair after a failed swap = %q", got)
	}
}

// A staged copy that does not match the manifest stops the install before DOOM changes.
func TestPairStagingChecksTheHash(t *testing.T) {
	doom, root, files := pairRig(t)
	files[1].sha256 = sha256Hex([]byte("something else"))
	if _, err := stagePair(doom, root, files); err == nil || !strings.Contains(err.Error(), "nothing was changed") {
		t.Fatalf("stage error = %v", err)
	}
	if got := pairContents(t, doom); got[0] != "old "+currentProxy || got[1] != "old "+currentUIDll {
		t.Errorf("pair after a failed stage = %q", got)
	}
}

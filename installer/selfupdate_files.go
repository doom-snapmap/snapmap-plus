package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// updateBackupName reports whether name is one of the <exe>.old, <exe>.old2 ... <exe>.old10 names a
// replaced image is moved to.
func updateBackupName(exe, name string) bool {
	if filepath.Base(name) != name {
		return false
	}
	suffix, matches := strings.CutPrefix(strings.ToLower(name), strings.ToLower(filepath.Base(exe))+".old")
	if !matches {
		return false
	}
	n, err := strconv.Atoi(suffix)
	return suffix == "" || (err == nil && n >= 2 && n <= 10 && strconv.Itoa(n) == suffix)
}

// asideCandidates lists, in order, where a replaced image of exe may be moved: the data folder's
// cache\installer when there is one (a rename, so only on exe's volume), then beside exe.
func asideCandidates(exe string) []string {
	var out []string
	names := func(dir string) {
		for i := 1; i <= 10; i++ {
			n := filepath.Base(exe) + ".old"
			if i > 1 {
				n = fmt.Sprintf("%s%d", n, i)
			}
			out = append(out, filepath.Join(dir, n))
		}
	}
	if dir := appDataDir(); dir != "" {
		cache := installerCacheDir(dir)
		if os.MkdirAll(cache, 0o755) == nil {
			names(cache)
		}
	}
	names(filepath.Dir(exe))
	return out
}

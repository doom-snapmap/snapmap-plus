package main

import (
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// install.json records which release each stable installer copy in the data folder is, tied to its
// SHA-256, so an older downloaded installer does not replace a newer stable copy.

// noteCopyVersion records that the executable at path is release ver.
func noteCopyVersion(path, ver string) {
	sum, err := fileSHA256(path)
	if err != nil || ver == "" {
		return
	}
	name := filepath.Base(path)
	updateStateAt(filepath.Dir(path), func(r *installRecord) {
		if r.Executables == nil {
			r.Executables = map[string]exeEntry{}
		}
		r.Executables[name] = exeEntry{SHA256: sum, Version: ver}
	})
}

// copyVersionOf returns the recorded release of the executable at path, or "" when it is unrecorded
// or its bytes changed since.
func copyVersionOf(path string) string {
	rec, err := loadStateAt(filepath.Dir(path))
	if err != nil {
		return ""
	}
	e, ok := rec.Executables[filepath.Base(path)]
	if !ok || !ownsExecutable(path, e.SHA256) {
		if v, ok := readRetiredVersions(filepath.Dir(path))[filepath.Base(path)]; ok && ownsExecutable(path, v.SHA256) {
			return v.Version
		}
		return ""
	}
	return e.Version
}

var releaseVersionRE = regexp.MustCompile(`^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$`)

// compareVersions orders two release tags by semantic-version precedence (v0.2.1-beta.9 before
// v0.2.1-beta.10 before v0.2.1). ok is false when either is not a release tag, such as "dev".
func compareVersions(a, b string) (int, bool) {
	ma, mb := releaseVersionRE.FindStringSubmatch(a), releaseVersionRE.FindStringSubmatch(b)
	if ma == nil || mb == nil {
		return 0, false
	}
	for i := 1; i <= 3; i++ {
		x, _ := strconv.Atoi(ma[i])
		y, _ := strconv.Atoi(mb[i])
		if x != y {
			return sign(x - y), true
		}
	}
	pa, pb := ma[4], mb[4]
	switch {
	case pa == pb:
		return 0, true
	case pa == "":
		return 1, true // a release ranks above its prereleases
	case pb == "":
		return -1, true
	}
	ia, ib := strings.Split(pa, "."), strings.Split(pb, ".")
	for i := 0; i < len(ia) && i < len(ib); i++ {
		x, ex := strconv.Atoi(ia[i])
		y, ey := strconv.Atoi(ib[i])
		switch {
		case ex == nil && ey == nil:
			if x != y {
				return sign(x - y), true
			}
		case ex == nil:
			return -1, true // numeric identifiers rank below alphanumeric ones
		case ey == nil:
			return 1, true
		default:
			if c := strings.Compare(ia[i], ib[i]); c != 0 {
				return c, true
			}
		}
	}
	return sign(len(ia) - len(ib)), true
}

func sign(n int) int {
	switch {
	case n < 0:
		return -1
	case n > 0:
		return 1
	}
	return 0
}

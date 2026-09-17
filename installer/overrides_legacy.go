package main

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
)

// Readers for the legacy package policy files, matching the historical backend
// readers that consumed them (package_requirements.c, strids.c, weapon_hud.c
// and resource_bridge.c) closely enough to refuse anything they refused.

// sh_decl_server_identity_from_relative, for a path below generated/decls/.
func legacyDeclIdentity(relative string) bool {
	if relative == "" || len(relative) >= 704 || relative[0] == '/' || strings.ContainsAny(relative, `:\`) {
		return false
	}
	if len(relative) < 5 || !strings.EqualFold(relative[len(relative)-5:], ".decl") {
		return false
	}
	extStart := len(relative) - 5
	slash := strings.IndexByte(relative[:extStart], '/')
	if slash <= 0 || slash+1 >= extStart || slash+1 > 64 || extStart-slash > 512 {
		return false
	}
	for i := 0; i < slash; i++ {
		c := relative[i]
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			return false
		}
	}
	for _, segment := range strings.Split(relative[slash+1:extStart], "/") {
		if segment == "" || segment == "." || segment == ".." {
			return false
		}
		for i := 0; i < len(segment); i++ {
			c := segment[i]
			if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
				return false
			}
		}
	}
	return len("generated/decls/")+len(relative) < 768
}

// sh_package_engine_path accepts each native entry name only if Windows could
// not reinterpret it: no reserved device stem, trailing dot/space or wildcard.
func engineNameValid(name string) bool {
	if name == "" || name[len(name)-1] == '.' || name[len(name)-1] == ' ' {
		return false
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		if c < 0x20 || strings.IndexByte(`:*?"<>|\/`, c) >= 0 {
			return false
		}
	}
	lower := asciiLower(name)
	stem := lower
	if dot := strings.IndexByte(lower, '.'); dot >= 0 {
		stem = lower[:dot]
	}
	switch {
	case stem == "con" || stem == "prn" || stem == "aux" || stem == "nul":
		return false
	case len(stem) == 4 && (strings.HasPrefix(stem, "com") || strings.HasPrefix(stem, "lpt")) && stem[3] >= '1' && stem[3] <= '9':
		return false
	}
	return true
}

func enginePathValid(path string) bool {
	if path == "" || len(path) >= 32760 {
		return false
	}
	for _, segment := range strings.Split(path, "/") {
		if !engineNameValid(segment) {
			return false
		}
	}
	return true
}

type manifestRow struct {
	kind, name, provider string
	origin               string
}

// Policy readers delegate format semantics to the same native core used by
// embedded maps. These wrappers only shape results for discovery and tests.
func parseLegacyManifest(body []byte, origin string) ([]manifestRow, error) {
	plan, err := legacyPolicyPlan(body, origin, "resources/policy.manifest")
	if err != nil {
		return nil, err
	}
	var rows []manifestRow
	for _, row := range plan.Imports {
		rows = append(rows, manifestRow{row.Kind, row.Name, row.Provider, row.Origin})
	}
	line := func(s string) int { n, _ := strconv.Atoi(s[strings.LastIndexByte(s, ':')+1:]); return n }
	sort.Slice(rows, func(i, j int) bool { return line(rows[i].origin) < line(rows[j].origin) })
	return rows, nil
}

func legacyPolicySection(body []byte, origin, path, section, child string) (*jsonValue, error) {
	plan, err := legacyPolicyPlan(body, origin, path)
	if err != nil {
		return nil, err
	}
	descriptor, err := parseNativeObject(plan.Descriptor, descriptorDepth)
	if err != nil {
		return nil, err
	}
	parent := descriptor.member(section)
	if parent != nil {
		if value := parent.member(child); value != nil {
			return value, nil
		}
	}
	return &jsonValue{kind: jsonObject}, nil
}

func parseLegacyRequirements(body []byte, origin string) ([]string, error) {
	section, err := legacyPolicySection(body, origin, "requirements/policy.requirements", "requirements", "cvars")
	if err != nil {
		return nil, err
	}
	var keys []string
	for _, m := range section.members {
		keys = append(keys, m.key)
	}
	return keys, nil
}

func parseLegacyStrings(body []byte, origin string) ([]jsonMember, error) {
	section, err := legacyPolicySection(body, origin, "strings/policy.json", "strings", "en")
	if err != nil {
		return nil, err
	}
	return section.members, nil
}

func parseLegacyHud(body []byte, origin string) (*jsonValue, error) {
	return legacyPolicySection(body, origin, "hud/weapons.json", "hud", "weapons")
}

// Snapshots record every authored entry, including empty directories, so a
// change between planning, staging and publication is detected.
type snapEntry struct {
	dir  bool
	size int64
	sum  [32]byte
}

type treeSnapshot map[string]snapEntry

func hashFile(path string) (int64, [32]byte, error) {
	var sum [32]byte
	f, err := os.Open(path)
	if err != nil {
		return 0, sum, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return 0, sum, err
	}
	copy(sum[:], h.Sum(nil))
	return n, sum, nil
}

// snapshotTree hashes node below base. Keys use prefix + node-relative paths.
func snapshotTree(base string, node *libraryNode, prefix string, into treeSnapshot) error {
	if node.unsafe {
		return rejectOverride("linked or special file is not supported: %s", prefix)
	}
	if !node.dir {
		size, sum, err := hashFile(base)
		if err != nil {
			return err
		}
		into[prefix] = snapEntry{size: size, sum: sum}
		return nil
	}
	into[prefix] = snapEntry{dir: true}
	for _, child := range node.children {
		if err := snapshotTree(base+string(os.PathSeparator)+child.name, child, prefix+"/"+child.name, into); err != nil {
			return err
		}
	}
	return nil
}

func sameSnapshot(a, b treeSnapshot) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if w, ok := b[k]; !ok || w != v {
			return false
		}
	}
	return true
}

func readVerified(path string, want snapEntry) ([]byte, error) {
	body, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if int64(len(body)) != want.size || sha256.Sum256(body) != want.sum {
		return nil, fmt.Errorf("%s changed while it was being converted; run the command again", path)
	}
	return body, nil
}

func hasSuffixFold(s, suffix string) bool {
	return len(s) >= len(suffix) && strings.EqualFold(s[len(s)-len(suffix):], suffix)
}

func legacySidecar(body []byte) bool {
	if len(body) != 16 {
		return false
	}
	return bytes.IndexFunc(body, func(r rune) bool { return !(r >= '0' && r <= '9' || r >= 'a' && r <= 'f') }) < 0
}

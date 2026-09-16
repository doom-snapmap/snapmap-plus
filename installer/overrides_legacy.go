package main

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
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

// rb_parse_manifest and rb_field_valid.
func parseLegacyManifest(body []byte, origin string) ([]manifestRow, error) {
	field := func(value string, allowEmpty, typeField bool, capacity int) bool {
		if value == "" {
			return allowEmpty
		}
		if len(value) >= capacity || value[0] == '/' || value[len(value)-1] == '/' {
			return false
		}
		for i := 0; i < len(value); i++ {
			c := value[i]
			if c < 0x21 || c > 0x7e || c == '\\' || c == ':' || (typeField && c == '/') {
				return false
			}
		}
		for _, segment := range strings.Split(value, "/") {
			if segment == "" || segment == "." || segment == ".." {
				return false
			}
		}
		return true
	}
	var rows []manifestRow
	for number, line := range strings.Split(string(body), "\n") {
		line = strings.TrimSuffix(line, "\r")
		if line == "" || line[0] == '#' {
			continue
		}
		for i := 0; i < len(line); i++ {
			if line[i] != '\t' && (line[i] < 0x20 || line[i] > 0x7e) {
				return nil, rejectOverride("%s:%d: manifest rows must be printable ASCII", origin, number+1)
			}
		}
		fields := strings.Split(line, "\t")
		if len(fields) != 3 || !field(fields[0], false, true, 64) || !field(fields[1], false, false, 512) ||
			!field(fields[2], true, false, 768) {
			return nil, rejectOverride("%s:%d: invalid manifest row", origin, number+1)
		}
		provider := fields[2]
		if provider == "" {
			provider = fields[1]
		}
		rows = append(rows, manifestRow{fields[0], fields[1], provider, fmt.Sprintf("%s:%d", origin, number+1)})
	}
	return rows, nil
}

// pr_parse_file: exactly cvar<TAB>name<TAB>value rows from the audited list.
func parseLegacyRequirements(body []byte, origin string) ([]string, error) {
	var cvars []string
	for number, line := range strings.Split(string(body), "\n") {
		line = strings.TrimSuffix(line, "\r")
		for i := 0; i < len(line); i++ {
			if line[i] != '\t' && (line[i] < 0x20 || line[i] > 0x7e) {
				return nil, rejectOverride("%s:%d: requirement rows must be printable ASCII", origin, number+1)
			}
		}
		if line == "" || line[0] == '#' {
			continue
		}
		fields := strings.Split(line, "\t")
		if len(fields) != 3 || fields[0] != "cvar" || !supportedCvars[fields[1]] || fields[2] != "0" {
			return nil, rejectOverride("%s:%d: unsupported requirement; only g_useResourceBlackList and g_useImageBlackList set to 0 are allowed", origin, number+1)
		}
		cvars = append(cvars, fields[1])
	}
	return cvars, nil
}

// A package strings file is a flat object of nonempty ids and text.
func parseLegacyStrings(body []byte, origin string) ([]jsonMember, error) {
	body, _ = withoutBOM(body)
	v, err := parseNativeObject(body, 1)
	if err != nil {
		return nil, rejectOverride("%s: expected a flat JSON object of text strings: %v", origin, err)
	}
	for _, m := range v.members {
		if m.key == "" || strings.ContainsRune(m.key, 0) || m.value.kind != jsonString || strings.ContainsRune(m.value.text, 0) {
			return nil, rejectOverride("%s: string %q must have a nonempty id and text", origin, m.key)
		}
	}
	return v.members, nil
}

// weapon_hud.c before the package format change.
func parseLegacyHud(body []byte, origin string) (*jsonValue, error) {
	body, _ = withoutBOM(body)
	v, err := parseNativeObject(body, 5)
	if err != nil {
		return nil, rejectOverride("%s: invalid weapon HUD policy: %v", origin, err)
	}
	schema, weapons := v.member("schema"), v.member("weapons")
	if len(v.members) != 2 || schema == nil || schema.kind != jsonString || schema.text != "snapmap-plus.weapon-hud.v1" || weapons == nil {
		return nil, rejectOverride("%s: expected the snapmap-plus.weapon-hud.v1 schema and a weapons object", origin)
	}
	if _, err := parseNativeJSON(formatNativeJSON(*weapons), 3); err != nil {
		return nil, rejectOverride("%s: weapon HUD rules are nested too deeply", origin)
	}
	if err := validateHudWeapons(weapons); err != nil {
		return nil, rejectOverride("%s: %v", origin, err)
	}
	return weapons, nil
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

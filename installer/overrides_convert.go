package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

type overrideRejected struct{ reason string }

func (e *overrideRejected) Error() string { return e.reason }
func rejectOverride(format string, args ...any) error {
	return &overrideRejected{fmt.Sprintf(format, args...)}
}

// Decode objects without accepting duplicate native keys, trailing input, or
// deep/ambiguous JSON. A map decode alone silently loses duplicate fields.
func overrideJSON(b []byte) (map[string]any, error) {
	d := json.NewDecoder(bytes.NewReader(b))
	d.UseNumber()
	var value func(int) (any, error)
	value = func(depth int) (any, error) {
		if depth > 24 {
			return nil, rejectOverride("package JSON nesting is too deep")
		}
		tok, err := d.Token()
		if err != nil {
			return nil, rejectOverride("invalid package JSON: %v", err)
		}
		delim, container := tok.(json.Delim)
		if !container {
			return tok, nil
		}
		if delim == '{' {
			m := map[string]any{}
			seen := map[string]bool{}
			for d.More() {
				key, err := d.Token()
				if err != nil {
					return nil, err
				}
				k, ok := key.(string)
				if !ok || k == "" || strings.ContainsRune(k, 0) || seen[strings.ToLower(k)] {
					return nil, rejectOverride("duplicate or invalid JSON key %v", key)
				}
				seen[strings.ToLower(k)] = true
				v, err := value(depth + 1)
				if err != nil {
					return nil, err
				}
				m[k] = v
			}
			_, err = d.Token()
			return m, err
		}
		if delim == '[' {
			a := []any{}
			for d.More() {
				v, err := value(depth + 1)
				if err != nil {
					return nil, err
				}
				a = append(a, v)
			}
			_, err = d.Token()
			return a, err
		}
		return nil, rejectOverride("unexpected JSON delimiter")
	}
	v, err := value(0)
	if err != nil {
		return nil, rejectOverride("invalid package JSON: %v", err)
	}
	if _, err = d.Token(); err != io.EOF {
		return nil, rejectOverride("trailing package JSON")
	}
	m, ok := v.(map[string]any)
	if !ok {
		return nil, rejectOverride("package JSON must be an object")
	}
	return m, nil
}

func overrideRel(p string) bool {
	if p == "" || strings.ContainsAny(p, `\:<>"|?*`+"\x00") {
		return false
	}
	for _, part := range strings.Split(p, "/") {
		if part == "" || part == "." || part == ".." || strings.TrimRight(part, ". ") != part {
			return false
		}
		base := strings.ToUpper(strings.SplitN(part, ".", 2)[0])
		if base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || regexp.MustCompile(`^(COM|LPT)[1-9]$`).MatchString(base) {
			return false
		}
	}
	return true
}

func overrideMerge(dst, src map[string]any, where string) error {
	for k, v := range src {
		key := k
		for old := range dst {
			if strings.EqualFold(k, old) {
				key = old
				break
			}
		}
		old, exists := dst[key]
		if !exists {
			dst[k] = v
			continue
		}
		a, aok := old.(map[string]any)
		b, bok := v.(map[string]any)
		if aok && bok {
			if err := overrideMerge(a, b, where+"."+k); err != nil {
				return err
			}
			continue
		}
		left, _ := json.Marshal(old)
		right, _ := json.Marshal(v)
		if !bytes.Equal(left, right) {
			return rejectOverride("conflicting values at %s.%s", where, k)
		}
	}
	return nil
}

func overrideSection(descriptor map[string]any, key string) (map[string]any, error) {
	if v, ok := descriptor[key]; ok {
		m, ok := v.(map[string]any)
		if !ok {
			return nil, rejectOverride("%s must be an object", key)
		}
		return m, nil
	}
	m := map[string]any{}
	descriptor[key] = m
	return m, nil
}

type overrideOutput struct {
	root    string
	paths   map[string]string
	changed bool
	catalog *overrideCatalog
}

func (o *overrideOutput) put(rel string, body []byte) error {
	if !overrideRel(rel) {
		return rejectOverride("unsafe converted path %q", rel)
	}
	key := strings.ToLower(rel)
	if old, exists := o.paths[key]; exists {
		b, err := os.ReadFile(filepath.Join(o.root, filepath.FromSlash(old)))
		if err != nil {
			return err
		}
		if bytes.Equal(b, body) {
			return nil
		}
		return rejectOverride("different files map to the same path: %s and %s", old, rel)
	}
	path := filepath.Join(o.root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	_, err = f.Write(body)
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	o.paths[key] = rel
	return nil
}

func oldOverrideRoot(name string) bool {
	switch strings.ToLower(name) {
	case "assets", "decls", "images", "shaders", "shader_includes", "generated", "resources", "requirements", "strings", "hud", "sound", "soundbanks", "base", "maps", "models", "renderprogs", "textures":
		return true
	}
	return false
}

// Convert a component with its child components intact. Only explicit historical
// namespaces are translated; unknown auxiliary files retain their authored path.
func (o *overrideOutput) component(source, prefix, identity string, synthetic bool) error {
	marker := filepath.Join(source, "package.json")
	raw, err := os.ReadFile(marker)
	descriptor := map[string]any{}
	legacy := synthetic
	if err == nil {
		descriptor, err = overrideJSON(raw)
		if err != nil {
			return err
		}
	} else if !os.IsNotExist(err) || !synthetic {
		return err
	}
	if _, ok := descriptor["id"]; !ok {
		legacy = true
		sum := sha256.Sum256([]byte(strings.ToLower(identity)))
		descriptor["id"] = "local.migrated-" + hex.EncodeToString(sum[:8])
		o.changed = true
	}
	if legacy {
		if name, ok := descriptor["name"].(string); !ok || name == "" {
			descriptor["name"] = filepath.Base(source)
		}
		delete(descriptor, "schema")
		delete(descriptor, "version")
	}
	id, ok := descriptor["id"].(string)
	if !ok || len(id) >= 128 || !regexp.MustCompile(`^[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?$`).MatchString(id) || strings.Contains(id, "..") {
		return rejectOverride("package id must be a stable lowercase identifier")
	}
	name, ok := descriptor["name"].(string)
	if !ok || name == "" || strings.ContainsRune(name, 0) {
		return rejectOverride("package name must be a nonempty string")
	}
	var manifests []string
	err = filepath.WalkDir(source, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == source {
			return nil
		}
		rel, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		rel = filepath.ToSlash(rel)
		if !plainPath(path) {
			return rejectOverride("linked path is unsupported: %s", rel)
		}
		if !overrideRel(rel) {
			return rejectOverride("unsafe authored path: %s", rel)
		}
		lower := strings.ToLower(rel)
		if entry.IsDir() {
			if lower != "assets" && !strings.HasPrefix(lower, "assets/") {
				if _, err := os.Lstat(filepath.Join(path, "package.json")); err == nil {
					if err := o.component(path, prefix+rel+"/", identity+"/"+rel, false); err != nil {
						return err
					}
					return filepath.SkipDir
				} else if !os.IsNotExist(err) {
					return err
				}
			}
			return os.MkdirAll(filepath.Join(o.root, filepath.FromSlash(prefix+rel)), 0755)
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return rejectOverride("not a regular file: %s", rel)
		}
		if lower == "package.json" {
			return nil
		}
		body, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		policyPath := lower
		if strings.HasPrefix(policyPath, "generated/") {
			policyPath = strings.TrimPrefix(policyPath, "generated/")
		}
		if strings.HasPrefix(policyPath, "requirements/") && strings.HasSuffix(policyPath, ".requirements") {
			req, err := overrideSection(descriptor, "requirements")
			if err != nil {
				return err
			}
			cv, err := overrideSection(req, "cvars")
			if err != nil {
				return err
			}
			for n, line := range strings.Split(string(body), "\n") {
				line = strings.TrimSpace(line)
				if line == "" || strings.HasPrefix(line, "#") {
					continue
				}
				fields := strings.Split(line, "\t")
				if len(fields) != 3 || fields[0] != "cvar" || (fields[1] != "g_useResourceBlackList" && fields[1] != "g_useImageBlackList") || fields[2] != "0" {
					return rejectOverride("unsupported requirement in %s:%d", rel, n+1)
				}
				if err := overrideMerge(cv, map[string]any{fields[1]: json.Number("0")}, "requirements.cvars"); err != nil {
					return err
				}
			}
			o.changed = true
			return nil
		}
		if strings.HasPrefix(policyPath, "strings/") && strings.HasSuffix(policyPath, ".json") {
			old, err := overrideJSON(body)
			if err != nil {
				return rejectOverride("%s: %v", rel, err)
			}
			for key, val := range old {
				if _, ok := val.(string); !ok {
					return rejectOverride("legacy string %s in %s is not text", key, rel)
				}
			}
			stringsSection, err := overrideSection(descriptor, "strings")
			if err != nil {
				return err
			}
			en, err := overrideSection(stringsSection, "en")
			if err != nil {
				return err
			}
			if err := overrideMerge(en, old, "strings.en"); err != nil {
				return err
			}
			o.changed = true
			return nil
		}
		if policyPath == "hud/weapons.json" {
			old, err := overrideJSON(body)
			if err != nil {
				return err
			}
			delete(old, "schema")
			hud, err := overrideSection(descriptor, "hud")
			if err != nil {
				return err
			}
			if err := overrideMerge(hud, old, "hud"); err != nil {
				return err
			}
			o.changed = true
			return nil
		}
		if strings.HasPrefix(policyPath, "resources/") && strings.HasSuffix(policyPath, ".manifest") {
			manifests = append(manifests, string(body))
			o.changed = true
			return nil
		}
		// Disabled files and local backups must never become native provider inputs.
		disabled := false
		for _, suffix := range []string{".backup", ".bak", ".original", ".disabled", "~"} {
			if strings.HasSuffix(lower, suffix) {
				disabled = true
			}
		}
		if disabled && (strings.HasPrefix(lower, "assets/") || oldOverrideRoot(strings.Split(lower, "/")[0])) {
			o.changed = true
			return o.put(prefix+"backups/"+rel, body)
		}
		engine := ""
		assetRel := rel
		if strings.HasPrefix(lower, "assets/") {
			engine = rel[len("assets/"):]
			assetRel = engine
		}
		candidate := strings.ToLower(assetRel)
		switch {
		case strings.HasPrefix(candidate, "decls/"):
			engine = "generated/decls/" + assetRel[len("decls/"):]
		case strings.HasPrefix(candidate, "images/"):
			engine = "generated/image/" + assetRel[len("images/"):]
		case strings.HasPrefix(candidate, "shaders/generated/spirv/") || strings.HasPrefix(candidate, "shaders/generated/renderprogs/"):
			engine = assetRel[len("shaders/"):]
		case strings.HasPrefix(candidate, "shader_includes/"):
			paths, err := o.catalog.includes(assetRel[len("shader_includes/"):])
			if err != nil {
				return err
			}
			for _, p := range paths {
				if err := o.put(prefix+"assets/"+p, body); err != nil {
					return err
				}
			}
			o.changed = true
			return nil
		case strings.HasPrefix(candidate, "shaders/"):
			return rejectOverride("unrecognized legacy shader path: %s", rel)
		case engine == "" && oldOverrideRoot(strings.Split(candidate, "/")[0]):
			engine = assetRel
		}
		target := prefix + rel
		if engine != "" {
			target = prefix + "assets/" + engine
		}
		if target != prefix+rel {
			o.changed = true
		}
		return o.put(target, body)
	})
	if err != nil {
		return err
	}
	for _, manifest := range manifests {
		for n, line := range strings.Split(manifest, "\n") {
			line = strings.TrimSuffix(line, "\r")
			if strings.TrimSpace(line) == "" || strings.HasPrefix(strings.TrimSpace(line), "#") {
				continue
			}
			fields := strings.Split(line, "\t")
			if len(fields) != 3 || fields[0] == "" || fields[1] == "" {
				return rejectOverride("invalid manifest row %d", n+1)
			}
			provider := fields[2]
			if provider == "" {
				provider = fields[1]
			}
			provider = strings.ReplaceAll(provider, `\`, "/")
			if !overrideRel(provider) {
				return rejectOverride("unsafe manifest provider path %s", provider)
			}
			// Explicit authored replacements own the bytes. Still resolve the triple
			// first so an invalid dependency entry is never silently dropped.
			body, err := o.catalog.resolve(fields[0], fields[1], provider)
			if err != nil {
				return err
			}
			target := prefix + "assets/" + provider
			if _, ok := o.paths[strings.ToLower(target)]; !ok {
				if err := o.put(target, body); err != nil {
					return err
				}
			}
		}
	}
	if err := validateOverridePolicy(descriptor); err != nil {
		return err
	}
	b, err := json.MarshalIndent(descriptor, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	// Valid modern descriptors are byte-for-byte stable when no field changed.
	if original, err := overrideJSON(raw); err == nil {
		a, _ := json.Marshal(original)
		v, _ := json.Marshal(descriptor)
		if bytes.Equal(a, v) {
			b = raw
		}
	}
	return o.put(prefix+"package.json", b)
}

func validateOverridePolicy(d map[string]any) error {
	if value, exists := d["requirements"]; exists {
		req, ok := value.(map[string]any)
		if !ok {
			return rejectOverride("requirements must be an object")
		}
		for key, value := range req {
			if key != "cvars" {
				return rejectOverride("unsupported requirement %s", key)
			}
			cv, ok := value.(map[string]any)
			if !ok {
				return rejectOverride("requirements.cvars must be an object")
			}
			for key, v := range cv {
				number, ok := v.(json.Number)
				if (key != "g_useResourceBlackList" && key != "g_useImageBlackList") || !ok || number != "0" {
					return rejectOverride("unsupported cvar requirement %s", key)
				}
			}
		}
	}
	if value, exists := d["strings"]; exists {
		locales, ok := value.(map[string]any)
		if !ok {
			return rejectOverride("strings must map locales to text")
		}
		for locale, value := range locales {
			if len(locale) >= 48 || !regexp.MustCompile(`^[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*$`).MatchString(locale) {
				return rejectOverride("invalid string locale %s", locale)
			}
			m, ok := value.(map[string]any)
			if !ok {
				return rejectOverride("strings.%s must be an object", locale)
			}
			for key, v := range m {
				s, ok := v.(string)
				if !ok || strings.ContainsRune(s, 0) {
					return rejectOverride("strings.%s.%s must be text", locale, key)
				}
			}
		}
	}
	if value, exists := d["hud"]; exists {
		if _, ok := value.(map[string]any); !ok {
			return rejectOverride("hud must be an object")
		}
	}
	return nil
}

// Snapshot includes empty directories and every original byte, not just active
// resources. It detects edits between staging and publication.
func overrideSnapshot(root string) (string, error) {
	h := sha256.New()
	var paths []string
	err := filepath.WalkDir(root, func(p string, e os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !plainPath(p) {
			return rejectOverride("linked path: %s", p)
		}
		paths = append(paths, p)
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Strings(paths)
	for _, p := range paths {
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return "", err
		}
		info, err := os.Lstat(p)
		if err != nil {
			return "", err
		}
		fmt.Fprintf(h, "%s\x00%d\x00%t\x00", filepath.ToSlash(rel), info.Size(), info.IsDir())
		if !info.IsDir() {
			if !info.Mode().IsRegular() {
				return "", rejectOverride("not a regular file: %s", p)
			}
			f, err := os.Open(p)
			if err != nil {
				return "", err
			}
			_, err = io.Copy(h, f)
			f.Close()
			if err != nil {
				return "", err
			}
		}
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

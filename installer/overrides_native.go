package main

import (
	"encoding/json"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

type migrationInputFile struct {
	Directory bool   `json:"directory"`
	Native    bool   `json:"native"`
	Body      string `json:"body"`
}

type migrationInput struct {
	Descriptor string                          `json:"descriptor"`
	ID         string                          `json:"id"`
	Name       string                          `json:"name"`
	Legacy     bool                            `json:"legacy"`
	Files      map[string]migrationInputFile   `json:"files"`
	Policies   map[string]migrationInputPolicy `json:"policies,omitempty"`
}

type migrationInputPolicy struct {
	Path string `json:"path"`
	Body string `json:"body"`
}

type migrationOutput struct {
	Descriptor json.RawMessage `json:"descriptor"`
	Files      map[string]struct {
		Target    string `json:"target"`
		Directory bool   `json:"directory"`
	} `json:"files"`
	Imports map[string]struct {
		Kind     string `json:"type"`
		Name     string `json:"name"`
		Provider string `json:"provider"`
		Origin   string `json:"origin"`
	} `json:"imports"`
}

func (cv *componentConversion) sharedComponent() error {
	c := cv.comp
	folder := filepath.Base(filepath.FromSlash(joinRel(cv.plan.unit.rel, c.rel)))
	name := folder
	for len(name) >= packageNameCapacity {
		_, size := utf8.DecodeLastRuneInString(name)
		name = name[:len(name)-size]
	}
	id := ""
	if existing := c.descriptor.member("id"); existing != nil {
		id = existing.text
	} else {
		folders := []string{folder}
		for parent := c.parent; parent != nil; parent = parent.parent {
			folders = append([]string{filepath.Base(filepath.FromSlash(joinRel(cv.plan.unit.rel, parent.rel)))}, folders...)
		}
		id = cv.plan.ctx.assignID(folders...)
	}
	request := migrationInput{Descriptor: string(c.raw), ID: id, Name: name, Legacy: c.legacyDesc,
		Files: map[string]migrationInputFile{}}
	if c.synthetic {
		request.Descriptor = "{}"
	}
	var walk func(*libraryNode, string) error
	walk = func(node *libraryNode, local string) error {
		for _, child := range node.children {
			path := joinRel(local, child.name)
			if cv.nested[child] {
				continue
			}
			if child.dir && len(child.children) != 0 {
				if err := walk(child, path); err != nil {
					return err
				}
				continue
			}
			if strings.EqualFold(path, "package.json") || (c.sidecar && strings.EqualFold(path, "smpkg.digest")) {
				continue
			}
			if err := cv.plan.checkName(path, cv.key(path)); err != nil {
				return err
			}
			file := migrationInputFile{Directory: child.dir}
			inner := path
			if strings.HasPrefix(asciiLower(path), "assets/") {
				inner = path[7:]
				if c.legacyDesc {
					var err error
					if child.dir {
						file.Native, err = cv.plan.ctx.catalog.engineDirectory(inner)
					} else {
						file.Native, err = cv.plan.ctx.catalog.engineContent(inner)
					}
					if err != nil {
						return err
					}
				}
			}
			// Body reads are adapters only. The core decides whether this is a
			// policy; ordinary assets are copied by the verified snapshot path.
			if !child.dir && !file.Native && legacyPolicyPath(inner) {
				body, err := cv.read(path)
				if err != nil {
					return err
				}
				if !utf8.Valid(body) {
					return rejectOverride("%s: policy is not valid UTF-8", cv.key(path))
				}
				file.Body = string(body)
			}
			request.Files[path] = file
		}
		return nil
	}
	if err := walk(c.node, ""); err != nil {
		return err
	}
	response, err := runNativeMigration(request)
	if err != nil {
		return rejectOverride("%s: %v", cv.source, err)
	}
	var plan migrationOutput
	if err := json.Unmarshal(response, &plan); err != nil {
		return err
	}
	keys := make([]string, 0, len(plan.Files))
	for source := range plan.Files {
		keys = append(keys, source)
	}
	sort.Strings(keys)
	for _, source := range keys {
		file := plan.Files[source]
		key := cv.key(source)
		if err := cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, file.Target), dir: file.Directory,
			source: key, origin: key}); err != nil {
			return err
		}
	}
	for _, row := range plan.Imports {
		cv.manifests = append(cv.manifests, manifestRow{row.Kind, row.Name, row.Provider, joinRel(cv.source, row.Origin)})
	}
	if err := cv.materialize(); err != nil {
		return err
	}
	if err := validateDescriptor(plan.Descriptor); err != nil {
		return rejectOverride("%s/package.json: %v", cv.source, err)
	}
	if c.legacyDesc {
		if priority := c.descriptor.member("priority"); priority != nil && priority.kind == jsonNumber && priority.raw != "0" {
			cv.plan.note("%s/package.json: retired priority %s; overlapping packages now compose or report conflicts", cv.source, priority.raw)
		}
	}
	// Current descriptors with only a BOM/sidecar/path repair keep their exact
	// bytes when the core made no semantic changes.
	data := []byte(plan.Descriptor)
	if !c.legacyDesc {
		before, e1 := parseNativeObject(c.raw, descriptorDepth)
		after, e2 := parseNativeObject(data, descriptorDepth)
		if e1 == nil && e2 == nil && nativeJSONEqual(before, after) {
			data = c.raw
		}
	}
	return cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, "package.json"), data: data,
		origin: joinRel(cv.source, "package.json")})
}

func nativeJSONEqual(a, b jsonValue) bool {
	// Raw numbers deliberately retain their spelling in the common parser.
	var left, right any
	d1 := json.NewDecoder(strings.NewReader(string(formatNativeJSON(a))))
	d2 := json.NewDecoder(strings.NewReader(string(formatNativeJSON(b))))
	d1.UseNumber()
	d2.UseNumber()
	if d1.Decode(&left) != nil || d2.Decode(&right) != nil {
		return false
	}
	x, _ := json.Marshal(left)
	y, _ := json.Marshal(right)
	return string(x) == string(y)
}

func legacyPolicyPath(path string) bool {
	// Read potential policy bytes without deciding their interpretation here.
	// Both old namespace and wrapper paths are submitted to the shared core.
	parts := strings.Split(path, "/")
	if len(parts) != 2 {
		return false
	}
	switch asciiLower(parts[0]) {
	case "resources", "requirements", "strings", "hud":
		return true
	}
	return false
}

func legacyPolicyPlan(body []byte, origin, path string) (migrationOutput, error) {
	var plan migrationOutput
	if !utf8.Valid(body) {
		return plan, rejectOverride("%s: policy is not valid UTF-8", origin)
	}
	request := migrationInput{Descriptor: "{}", ID: "legacy.policy", Name: "Legacy policy", Legacy: true,
		Files: map[string]migrationInputFile{}, Policies: map[string]migrationInputPolicy{origin: {Path: path, Body: string(body)}}}
	response, err := runNativeMigration(request)
	if err != nil {
		return plan, err
	}
	err = json.Unmarshal(response, &plan)
	return plan, err
}

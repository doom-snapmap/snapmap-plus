package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strings"
)

type earlierGlobalName struct{ kind, name, file, reference, engine, build string }
type earlierGlobalNames struct {
	files map[string]earlierGlobalName
	names map[string]map[string]earlierGlobalName
	kinds map[string]string
}

// An opaque file can keep its bytes only when it embeds none of the original names that need moving.
func (g earlierGlobalNames) opaqueFile(data []byte) error {
	lower := bytes.ToLower(data)
	for name := range g.names {
		if bytes.Contains(lower, []byte(name)) {
			return fmt.Errorf("cannot translate the added resource name %s inside this file", name)
		}
	}
	return nil
}

// A retail model stores complete material and skeleton paths that may share a component with an added name.
func (g earlierGlobalNames) retailOpaqueFile(data []byte) error {
	lower := bytes.ToLower(data)
	for name := range g.names {
		needle := []byte(name)
		for start := 0; start < len(lower); {
			at := bytes.Index(lower[start:], needle)
			if at < 0 {
				break
			}
			at += start
			first, end := at, at+len(needle)
			for first > 0 && earlierResourceChar(lower[first-1]) {
				first--
			}
			for end < len(lower) && earlierResourceChar(lower[end]) {
				end++
			}
			token := string(lower[first:end])
			_, _, image := g.at(token, "image")
			if token == name || g.files[token].name != "" || image {
				return fmt.Errorf("cannot translate the added resource name %s inside this file", name)
			}
			start = end
		}
	}
	return nil
}

func earlierResourceChar(c byte) bool {
	return c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || strings.IndexByte("_./$+@~-", c) >= 0
}

func (g *earlierGlobalNames) add(f earlierGlobalName) error {
	if g.files == nil {
		g.files = map[string]earlierGlobalName{}
		g.names = map[string]map[string]earlierGlobalName{}
	}
	if g.names[f.name] == nil {
		g.names[f.name] = map[string]earlierGlobalName{}
	}
	if old, ok := g.names[f.name][f.kind]; ok && old != f {
		return fmt.Errorf("two earlier mods claim the added %s %s", f.kind, f.name)
	}
	g.names[f.name][f.kind] = f
	return nil
}

type earlierProvided struct {
	Request  string
	Provides struct{ Type, Name string }
	Stock    json.RawMessage
}

func (p *earlierPins) globalNames(b *earlierBundle, work string) (earlierGlobalNames, error) {
	var g earlierGlobalNames
	var doc struct {
		Extra earlierObject
		Files json.RawMessage
	}
	if err := json.Unmarshal(b.descriptor, &doc); err != nil {
		return g, err
	}
	if doc.Extra["legacy"] == nil {
		return g, nil
	}
	var files []earlierProvided
	if err := json.Unmarshal(doc.Files, &files); err != nil {
		return g, err
	}
	return p.classifiedNames(b.id, b.hash, work, files)
}

func (p *earlierPins) sourceGlobalNames(id, work string, requests []string) (earlierGlobalNames, error) {
	var files []earlierProvided
	for _, req := range requests {
		req = strings.ToLower(req)
		f := earlierProvided{Request: req}
		switch {
		case strings.HasPrefix(req, "generated/decls/") && strings.HasSuffix(req, ".decl"):
			var ok bool
			f.Provides.Type, f.Provides.Name, ok = strings.Cut(strings.TrimSuffix(strings.TrimPrefix(req, "generated/decls/"), ".decl"), "/")
			if !ok {
				return earlierGlobalNames{}, fmt.Errorf("invalid earlier declaration %s", req)
			}
		case strings.HasPrefix(req, "generated/image/") && strings.HasSuffix(req, ".bimage"):
			f.Provides.Type = "image"
			f.Provides.Name = strings.TrimSuffix(strings.TrimPrefix(req, "generated/image/"), ".bimage")
		case strings.HasPrefix(req, "generated/basemodel/md6/") && strings.HasSuffix(req, ".bmd6model"):
			f.Provides.Type = "baseModel"
			f.Provides.Name = strings.TrimSuffix(strings.TrimPrefix(req, "generated/basemodel/"), ".bmd6model") + ".md6mesh"
		default:
			continue
		}
		files = append(files, f)
	}
	return p.classifiedNames(id, "", work, files)
}

func (p *earlierPins) classifyRequests(work string, requests []string) (map[string]bool, error) {
	if err := p.ready(); err != nil {
		return nil, err
	}
	if err := os.WriteFile(filepath.Join(work, "requests.txt"), []byte(strings.Join(requests, "\n")+"\n"), 0600); err != nil {
		return nil, err
	}
	out, err := p.run(work, "classify=1")
	if err != nil {
		return nil, fmt.Errorf("cannot establish which resources the installed game owns")
	}
	found := map[string]bool{}
	for i, line := range strings.Split(strings.TrimRight(out, "\r\n"), "\n") {
		v, name, ok := strings.Cut(strings.TrimSuffix(line, "\r"), "\t")
		if !ok || i >= len(requests) || name != requests[i] || (v != "0" && v != "1") {
			return nil, fmt.Errorf("unreadable stock ownership result")
		}
		found[name] = v == "1"
	}
	if len(found) != len(requests) {
		return nil, fmt.Errorf("incomplete installed resource ownership check")
	}
	return found, nil
}

func (p *earlierPins) classifiedNames(id, build, work string, files []earlierProvided) (earlierGlobalNames, error) {
	var g earlierGlobalNames
	requests := []string{}
	for _, f := range files {
		if strings.ContainsAny(f.Request, "\t\r\n") {
			return g, fmt.Errorf("invalid earlier request")
		}
		author, err := earlierAuthorPath(id, f.Request)
		if err != nil {
			return g, err
		}
		if strings.HasPrefix(author, "native/") {
			requests = append(requests, strings.ToLower(f.Request))
		}
	}
	if len(requests) == 0 {
		return g, nil
	}
	found, err := p.classifyRequests(work, requests)
	if err != nil {
		return g, err
	}
	salt := earlierSalt(id)
	for _, f := range files {
		request := strings.ToLower(f.Request)
		known, checked := found[request]
		if !checked || known {
			continue
		}
		kind, name := strings.ToLower(f.Provides.Type), strings.ToLower(f.Provides.Name)
		if !earlierKey.MatchString(kind) || !pinRelative(name) || strings.HasPrefix(name, "mods/") || strings.Contains(name, "/mods/") {
			return g, fmt.Errorf("unowned earlier addition %s", f.Request)
		}
		var expected, folder, ext string
		stem := name
		engine := "mods/" + salt + "/" + name
		switch {
		case strings.HasPrefix(request, "generated/decls/") && kind != "sound" && kind != "renderprog":
			expected = "generated/decls/" + kind + "/" + name + ".decl"
			folder = earlierDecls[kind]
			if folder == "" {
				folder = "decls/" + kind
			}
			ext = ".decl"
		case kind == "image":
			expected = "generated/image/" + name + ".bimage"
			folder = "images"
			ext = ".bimage"
		case kind == "basemodel" && strings.HasPrefix(name, "md6/") && strings.HasSuffix(name, ".md6mesh"):
			stem = strings.TrimSuffix(strings.TrimPrefix(name, "md6/"), ".md6mesh")
			expected = "generated/basemodel/md6/" + stem + ".bmd6model"
			folder, ext = "meshes", ".bmd6model"
			engine = "md6/mods/" + salt + "/" + stem + ".md6mesh"
		default:
			return g, fmt.Errorf("the added %s cannot be translated from %s", f.Provides.Type, f.Request)
		}
		if request != expected || !pinRelative(stem) {
			return g, fmt.Errorf("the request and typed provider disagree for %s", f.Request)
		}
		ref := folder + "/" + stem
		add := earlierGlobalName{kind, name, ref + ext, ref, engine, build}
		if kind == "basemodel" {
			add.reference += ".md6mesh"
		}
		if err := g.add(add); err != nil {
			return g, err
		}
		g.files[request] = add
	}
	if len(g.names) > 0 {
		g.kinds = map[string]string{}
		for _, f := range files {
			// Borrowed stock text predates these additions. It can contain unrelated names with the same spelling.
			request := strings.ToLower(f.Request)
			if f.Stock == nil || g.files[request].kind == "basemodel" {
				g.kinds[request] = strings.ToLower(f.Provides.Type)
			}
		}
	}
	return g, nil
}

func (g earlierGlobalNames) at(name, kind string) (earlierGlobalName, string, bool) {
	n := strings.TrimPrefix(strings.ToLower(name), "native:")
	suffix := ""
	if kind == "image" {
		switch path.Ext(n) {
		case ".tga", ".png", ".dds", ".jpg", ".jpeg", ".bimage":
			suffix = path.Ext(n)
			n = strings.TrimSuffix(n, suffix)
		}
	}
	f, ok := g.names[n][kind]
	return f, suffix, ok
}

// Part of a declaration whose game type establishes the name's owner.
func earlierReferenceKind(owner, key, container string) string {
	switch strings.ToLower(key) {
	case "movementgraph":
		if owner == "aibehavior" {
			return "aimovementgraph"
		}
	case "aifsmmanagerdecllayersconfig":
		if owner == "aibehavior" {
			return "fsmLayerName"
		}
	case "layer":
		if owner == "aifsmmanager" {
			return "fsmLayerName"
		}
	case "inherit":
		return owner
	case "entitydef":
		return "entitydef"
	case "encountertospawn":
		return "snapaiencounter"
	case "previewimage", "material":
		return "material"
	case "rendermodelname":
		return "md6def"
	case "mesh":
		if owner == "md6def" {
			return "basemodel"
		}
	case "animwebdecl":
		return "animweb"
	case "fxdecl":
		return "fx"
	case "transmap", "diffusemap", "bumpmap", "specularmap":
		if owner == "material" {
			return "image"
		}
	case "item":
		switch strings.ToLower(container) {
		case "validencounters":
			return "snapaiencounter"
		case "buildgamerefentitydefs":
			return "entitydef"
		}
	}
	if owner == "animweb" && strings.EqualFold(container, "modelInfos") {
		return "md6def"
	}
	return ""
}

var earlierDeclWord = regexp.MustCompile(`//[^\r\n]*|(?s:/\*.*?\*/)|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|[A-Za-z0-9_:$./+@~-]+|[\[\]{}=;]`)

func (g earlierGlobalNames) decl(text, owner string) (string, error) {
	if len(g.names) == 0 {
		return text, nil
	}
	stack := []string{""}
	last, field := "", ""
	indexed := false
	var out strings.Builder
	copied := 0
	for _, span := range earlierDeclWord.FindAllStringIndex(text, -1) {
		token := text[span[0]:span[1]]
		if strings.HasPrefix(token, "//") || strings.HasPrefix(token, "/*") {
			continue
		}
		switch token {
		case "[":
			indexed = true
			continue
		case "]":
			indexed = false
			continue
		case "=":
			field = last
			continue
		case "{":
			if field != "" {
				last = field
			}
			stack = append(stack, last)
			last, field = "", ""
			continue
		case "}":
			if len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
			last, field = "", ""
			continue
		case ";":
			last, field = "", ""
			continue
		}
		if indexed {
			continue
		}
		value := token
		quoted := strings.HasPrefix(token, "\"") || strings.HasPrefix(token, "'")
		if quoted {
			value = token[1 : len(token)-1]
		}
		key := field
		if key == "" {
			key = last
		}
		kind := earlierReferenceKind(owner, key, stack[len(stack)-1])
		if f, suffix, ok := g.at(value, kind); ok {
			start, end := span[0], span[1]
			if quoted {
				start++
				end--
			}
			out.WriteString(text[copied:start])
			out.WriteString(f.reference + suffix)
			copied = end
		} else if kind == "" && g.names[strings.TrimPrefix(strings.ToLower(value), "native:")] != nil && (quoted || field != "") {
			switch strings.ToLower(key) {
			case "displayname", "name", "label", "description", "text":
			default:
				return "", fmt.Errorf("cannot establish the type of %s in %s", value, owner)
			}
		}
		last, field = value, ""
	}
	if copied == 0 {
		return text, nil
	}
	out.WriteString(text[copied:])
	return out.String(), nil
}

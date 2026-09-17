package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf8"
)

// Planning decides every output of one unit before anything is written.
// Current packages are only classified. A legacy package becomes a complete
// staged tree whose outputs are validated with the runtime's rules.

type plannedOutput struct {
	rel    string // slash path below the published root
	dir    bool
	source string      // snapshot key of an authored file copied unchanged
	record *catalogRow // installed record materialized into the package
	data   []byte      // generated descriptor
	sum    [32]byte
	known  bool
	origin string
}

type outputSet struct {
	items   map[string]*plannedOutput // key: lower-case rel
	plan    *unitPlan
	shadows map[string]string // lower-case rel -> origin that historically won
}

type publishRoot struct {
	rel     string // destination below the overrides root
	outputs *outputSet
}

type unitPlan struct {
	unit     overrideUnit
	outer    *componentInfo
	sources  []string // below the overrides root; moved into the backup
	snapshot treeSnapshot
	roots    []publishRoot
	notes    []string
	ctx      *migrationContext
}

type migrationContext struct {
	root    string
	catalog *overrideCatalog
	ids     map[string]string // package id -> owner
}

func (p *unitPlan) note(format string, args ...any) {
	p.notes = append(p.notes, fmt.Sprintf(format, args...))
}

func (p *unitPlan) abs(rel string) string {
	return filepath.Join(p.ctx.root, filepath.FromSlash(rel))
}

func newOutputSet(plan *unitPlan) *outputSet {
	return &outputSet{items: map[string]*plannedOutput{}, plan: plan, shadows: map[string]string{}}
}

func (o *outputSet) digest(item *plannedOutput) ([32]byte, error) {
	if item.known {
		return item.sum, nil
	}
	switch {
	case item.data != nil:
		item.sum = sha256.Sum256(item.data)
	case item.record != nil:
		sum, _, err := o.plan.ctx.catalog.recordDigest(item.record)
		if err != nil {
			return item.sum, err
		}
		item.sum = sum
	default:
		item.sum = o.plan.snapshot[item.source].sum
	}
	item.known = true
	return item.sum, nil
}

// add records one output. Identical duplicates collapse; any other collision
// leaves the unit unconverted because no historical reader chose between them.
func (o *outputSet) add(item *plannedOutput) error {
	key := asciiLower(item.rel)
	if !utf8.ValidString(item.rel) {
		return rejectOverride("%s: path is not valid Unicode", item.origin)
	}
	old, exists := o.items[key]
	if !exists {
		o.items[key] = item
		return nil
	}
	if old.dir || item.dir {
		if old.dir && item.dir {
			return nil
		}
		return rejectOverride("%s and %s both need %s as a file and a folder", old.origin, item.origin, item.rel)
	}
	a, err := o.digest(old)
	if err != nil {
		return err
	}
	b, err := o.digest(item)
	if err != nil {
		return err
	}
	if a != b {
		return rejectOverride("%s and %s supply different bytes for %s", old.origin, item.origin, item.rel)
	}
	return nil
}

func (o *outputSet) has(rel string) bool {
	_, ok := o.items[asciiLower(rel)]
	return ok
}

// Every parent of a file must be a folder, and sibling names must not differ
// only by case; the runtime inventory refuses both.
func (o *outputSet) finish() error {
	for key, item := range o.items {
		for parent := key; ; {
			slash := strings.LastIndexByte(parent, '/')
			if slash < 0 {
				break
			}
			parent = parent[:slash]
			if p, ok := o.items[parent]; ok && !p.dir {
				return rejectOverride("%s needs %s to be a folder", item.origin, p.rel)
			}
		}
	}
	return nil
}

func (o *outputSet) sorted() []*plannedOutput {
	items := make([]*plannedOutput, 0, len(o.items))
	for _, item := range o.items {
		items = append(items, item)
	}
	sort.Slice(items, func(i, j int) bool { return items[i].rel < items[j].rel })
	return items
}

type componentInfo struct {
	rel        string // below the unit root; "" for the outer component
	node       *libraryNode
	synthetic  bool
	markerName string
	raw        []byte // descriptor bytes without a byte-order mark
	descriptor jsonValue
	bom, empty bool
	legacyDesc bool // no id, empty marker or the old schema: an old descriptor
	sidecar    bool
	legacy     bool // needs conversion
	children   []*componentInfo
	parent     *componentInfo
}

var legacyNamespaces = map[string]bool{"decls": true, "images": true, "shaders": true, "resources": true,
	"requirements": true, "strings": true, "hud": true}

func joinRel(a, b string) string {
	if a == "" {
		return b
	}
	if b == "" {
		return a
	}
	return a + "/" + b
}

// Identity comes from the folder name, like the fresh starter package
// local.my-overrides, and is made unique within this library.
func (ctx *migrationContext) assignID(folders ...string) string {
	var parts []string
	for _, folder := range folders {
		var b strings.Builder
		for _, r := range strings.ToLower(folder) {
			switch {
			case r >= 'a' && r <= 'z', r >= '0' && r <= '9':
				b.WriteRune(r)
			case r == '_' || r == '-' || r == '.':
				b.WriteRune(r)
			default:
				b.WriteByte('-')
			}
		}
		slug := b.String()
		for strings.Contains(slug, "--") {
			slug = strings.ReplaceAll(slug, "--", "-")
		}
		for strings.Contains(slug, "..") {
			slug = strings.ReplaceAll(slug, "..", ".")
		}
		slug = strings.Trim(slug, "._-")
		if slug == "" {
			slug = "package"
		}
		parts = append(parts, slug)
	}
	base := "local." + strings.Join(parts, ".")
	if len(base) > 100 {
		base = strings.TrimRight(base[:100], "._-")
	}
	id := base
	for i := 2; ctx.ids[id] != "" || !packageIDValid(id); i++ {
		id = fmt.Sprintf("%s-%d", base, i)
	}
	ctx.ids[id] = strings.Join(folders, "/")
	return id
}

// inspectComponent classifies one component and finds its nested components
// with the runtime scanner's rules. Legacy namespace folders are content.
func (p *unitPlan) inspectComponent(unitAbs string, node *libraryNode, rel string, synthetic bool, parent *componentInfo) (*componentInfo, error) {
	c := &componentInfo{rel: rel, node: node, synthetic: synthetic, parent: parent}
	origin := joinRel(p.unit.rel, rel)
	if synthetic {
		c.descriptor = jsonValue{kind: jsonObject}
		c.legacyDesc = true
	} else {
		marker := node.child("package.json")
		if marker == nil || marker.dir || marker.unsafe {
			return nil, rejectOverride("%s: package.json is not a regular file", origin)
		}
		c.markerName = marker.name
		raw, err := os.ReadFile(filepath.Join(unitAbs, filepath.FromSlash(marker.rel)))
		if err != nil {
			return nil, err
		}
		body, bom := withoutBOM(raw)
		c.bom, c.raw = bom, body
		if len(bytes.Trim(body, " \t\r\n")) == 0 {
			// Before the package format change the marker was never parsed.
			c.empty, c.legacyDesc = true, true
			c.descriptor = jsonValue{kind: jsonObject}
		} else {
			c.descriptor, err = parseNativeObject(body, descriptorDepth)
			if err != nil {
				return nil, rejectOverride("%s/package.json is not a valid JSON object (%v); fix it, then run migrate-overrides again", origin, err)
			}
			schema := c.descriptor.member("schema")
			c.legacyDesc = c.descriptor.member("id") == nil ||
				(schema != nil && schema.kind == jsonString && strings.HasPrefix(schema.text, "snapmap-plus.override-package."))
			if id := c.descriptor.member("id"); id != nil {
				if id.kind != jsonString || strings.ContainsRune(id.text, 0) {
					return nil, rejectOverride("%s/package.json id must be a string without embedded NULs", origin)
				}
				// Route noncanonical spelling through the shared planner. It owns
				// normalization and rejects identities that cannot be repaired.
				c.legacy = !packageIDValid(id.text)
			}
			if !c.legacyDesc && !c.legacy {
				if err := validateDescriptor(body); err != nil {
					return nil, rejectOverride("%s/package.json: %v", origin, err)
				}
			}
			if id := c.descriptor.member("id"); id != nil {
				key := asciiLower(strings.Trim(id.text, " \t\r\n"))
				if owner, taken := p.ctx.ids[key]; !taken || owner == "" {
					p.ctx.ids[key] = origin
				}
			}
		}
	}
	if sidecar := node.child("smpkg.digest"); sidecar != nil && !sidecar.dir && !sidecar.unsafe && sidecar.size == 16 {
		body, err := os.ReadFile(filepath.Join(unitAbs, filepath.FromSlash(sidecar.rel)))
		if err != nil {
			return nil, err
		}
		c.sidecar = legacySidecar(body)
	}
	evidence, err := p.structuralEvidence(unitAbs, c)
	if err != nil {
		return nil, err
	}
	// Normalize descriptor spelling and encoding together with legacy layout.
	c.legacy = c.legacy || c.legacyDesc || c.bom || c.sidecar || evidence
	var visit func(n *libraryNode, local string) error
	visit = func(n *libraryNode, local string) error {
		for _, child := range n.children {
			if !child.dir || child.unsafe {
				continue
			}
			childLocal := joinRel(local, child.name)
			first := strings.ToLower(strings.SplitN(childLocal, "/", 2)[0])
			if first == "assets" || (c.legacy && legacyNamespaces[first]) {
				continue
			}
			if child.marked {
				nested, err := p.inspectComponent(unitAbs, child, joinRel(rel, childLocal), false, c)
				if err != nil {
					return err
				}
				c.children = append(c.children, nested)
				continue
			}
			if err := visit(child, childLocal); err != nil {
				return err
			}
		}
		return nil
	}
	return c, visit(node, "")
}

// Root-level legacy namespaces are evidence only when their content has the
// legacy meaning; for a current descriptor unrelated files stay authored data.
func (p *unitPlan) structuralEvidence(unitAbs string, c *componentInfo) (bool, error) {
	read := func(n *libraryNode) ([]byte, error) {
		return os.ReadFile(filepath.Join(unitAbs, filepath.FromSlash(n.rel)))
	}
	for _, ns := range c.node.children {
		if !ns.dir || ns.unsafe {
			continue
		}
		switch strings.ToLower(ns.name) {
		case "decls":
			found := false
			ns.walk(func(f *libraryNode) bool {
				found = found || (!f.dir && !f.unsafe && legacyDeclIdentity(f.rel[len(ns.rel)+1:]))
				return !found
			})
			if found {
				return true, nil
			}
		case "images":
			if ns.hasFile() {
				return true, nil
			}
		case "shaders":
			if generated := ns.child("generated"); generated != nil {
				for _, family := range []string{"spirv", "renderprogs"} {
					if f := generated.child(family); f != nil && f.dir && f.hasFile() {
						return true, nil
					}
				}
			}
		case "resources", "requirements", "strings":
			for _, f := range ns.children {
				if f.dir || f.unsafe {
					continue
				}
				lower := strings.ToLower(ns.name)
				suffix := map[string]string{"resources": ".manifest", "requirements": ".requirements", "strings": ".json"}[lower]
				if !hasSuffixFold(f.name, suffix) {
					continue
				}
				if c.legacyDesc {
					return true, nil
				}
				body, err := read(f)
				if err != nil {
					return false, err
				}
				switch lower {
				case "resources":
					_, err = parseLegacyManifest(body, f.rel)
				case "requirements":
					_, err = parseLegacyRequirements(body, f.rel)
				case "strings":
					_, err = parseLegacyStrings(body, f.rel)
				}
				if err == nil {
					return true, nil
				}
			}
		case "hud":
			if f := ns.child("weapons.json"); f != nil && !f.dir && !f.unsafe {
				if c.legacyDesc {
					return true, nil
				}
				body, err := read(f)
				if err != nil {
					return false, err
				}
				if _, err := parseLegacyHud(body, f.rel); err == nil {
					return true, nil
				}
			}
		}
	}
	return false, nil
}

// changeRoots are the topmost legacy components; everything else is unchanged.
func changeRoots(c *componentInfo) []*componentInfo {
	if c.legacy {
		return []*componentInfo{c}
	}
	var roots []*componentInfo
	for _, child := range c.children {
		roots = append(roots, changeRoots(child)...)
	}
	return roots
}

// inspectUnit lists and classifies a package or synthetic unit and records
// every existing package id. It reads descriptors and legacy policy files only.
func inspectUnit(ctx *migrationContext, unit overrideUnit) (*unitPlan, error) {
	p := &unitPlan{unit: unit, ctx: ctx, snapshot: treeSnapshot{}}
	if unit.kind == unitLoose {
		return p, nil
	}
	unitAbs := p.abs(unit.rel)
	tree, err := listLibrary(unitAbs, "", false)
	if err != nil {
		return nil, err
	}
	p.outer, err = p.inspectComponent(unitAbs, tree, "", unit.kind == unitSynthetic, nil)
	return p, err
}

// changed reports whether building this plan produces a new tree.
func (p *unitPlan) changed() bool {
	return p.unit.kind == unitLoose || len(changeRoots(p.outer)) > 0
}

// build snapshots the sources and plans every output of a changed unit.
func (p *unitPlan) build() error {
	if p.unit.kind == unitLoose {
		return p.planLoose()
	}
	roots := changeRoots(p.outer)
	for _, root := range roots {
		source := joinRel(p.unit.rel, root.rel)
		if root.node.hasUnsafe() {
			return rejectOverride("%s contains a link or special file; replace it with a regular folder or file", source)
		}
		p.sources = append(p.sources, source)
		if err := snapshotTree(p.abs(source), root.node, source, p.snapshot); err != nil {
			return err
		}
	}
	for _, root := range roots {
		outputs := newOutputSet(p)
		if err := p.convertTree(root, "", outputs); err != nil {
			return err
		}
		if err := outputs.finish(); err != nil {
			return err
		}
		p.roots = append(p.roots, publishRoot{rel: joinRel(p.unit.rel, root.rel), outputs: outputs})
	}
	return nil
}

func (p *unitPlan) planLoose() error {
	outputs := newOutputSet(p)
	cv := &componentConversion{plan: p, outputs: outputs, wrappers: true,
		nested: map[*libraryNode]bool{},
		comp:   &componentInfo{descriptor: jsonValue{kind: jsonObject}, synthetic: true, legacyDesc: true, empty: true}}
	type pending struct{ target, local string }
	var namespaced []pending
	file := func(role looseRole, local string) error {
		segments := splitRel(local)
		switch role {
		case looseGenerated:
			second := strings.ToLower(segments[1])
			switch {
			case second == "decls" && len(segments) > 2:
				if legacyDeclIdentity(strings.Join(segments[2:], "/")) {
					return cv.put("assets/"+local, local)
				}
				return cv.aux(local, local)
			case (second == "resources" || second == "requirements") && len(segments) == 3:
				if handled, err := cv.policy(local, segments[1:]); handled || err != nil {
					return err
				}
			case second == "shaders":
				// The tree was also a package before 94041c7; its shader
				// namespace is used unless a loose exact file shadowed it.
				if target := namespaceTarget(segments[1:]); target != "" && len(segments) > 4 {
					namespaced = append(namespaced, pending{target, local})
					return nil
				}
			}
			if includeTerminal(asciiLower(local)) {
				return cv.aux(local, local)
			}
			return cv.put("assets/"+local, local)
		case looseInclude:
			aliases, err := p.ctx.catalog.includeAliases(strings.Join(segments[1:], "/"))
			if err != nil {
				return err
			}
			if len(aliases) == 0 {
				p.note("%s matches no installed shader include; it is kept, inactive, in the package", local)
				return cv.aux(local, local)
			}
			for _, alias := range aliases {
				if err := cv.put("assets/"+alias, local); err != nil {
					return err
				}
			}
			return nil
		case looseHybrid:
			return cv.wrapped(local, segments[1:])
		}
		if includeTerminal(asciiLower(local)) {
			return cv.aux(local, local)
		}
		return cv.put("assets/"+local, local)
	}
	emptyDir := func(role looseRole, local string) error {
		segments := splitRel(local)
		switch {
		case role == looseGenerated && len(segments) > 1 && strings.EqualFold(segments[1], "shaders"):
			if target := namespaceTarget(segments[1:]); target != "" {
				return cv.dir(target, local)
			}
			return cv.dir(local, local)
		case role == looseInclude:
			return cv.dir(local, local)
		case role == looseHybrid:
			return cv.emptyDir(local)
		}
		return cv.dir("assets/"+local, local)
	}
	for _, source := range p.unit.loose {
		abs := p.abs(source.rel)
		node := &libraryNode{rel: source.rel, name: filepath.Base(abs)}
		if source.dir {
			listed, err := listLibrary(abs, source.rel, false)
			if err != nil {
				return err
			}
			node = listed
		} else {
			info, err := os.Lstat(abs)
			if err != nil {
				return err
			}
			node.size = info.Size()
		}
		p.sources = append(p.sources, source.rel)
		if err := snapshotTree(abs, node, source.rel, p.snapshot); err != nil {
			return err
		}
		if !node.dir {
			if err := file(source.role, source.rel); err != nil {
				return err
			}
			continue
		}
		if len(node.children) == 0 {
			if err := emptyDir(source.role, source.rel); err != nil {
				return err
			}
		}
		var failure error
		node.walk(func(n *libraryNode) bool {
			switch {
			case n.dir && len(n.children) == 0:
				failure = emptyDir(source.role, n.rel)
			case !n.dir:
				failure = file(source.role, n.rel)
			}
			return failure == nil
		})
		if failure != nil {
			return failure
		}
	}
	for _, item := range namespaced {
		if existing, ok := outputs.items[asciiLower(item.target)]; ok && !existing.dir {
			a, err := outputs.digest(existing)
			if err != nil {
				return err
			}
			if a != p.snapshot[item.local].sum {
				p.note("%s was shadowed by %s before packages changed format; it is kept, inactive, in the package", item.local, existing.origin)
				if err := cv.aux(item.local, item.local); err != nil {
					return err
				}
			}
			continue
		}
		if err := cv.put(item.target, item.local); err != nil {
			return err
		}
	}
	if err := cv.descriptor(); err != nil {
		return err
	}
	if err := cv.materialize(); err != nil {
		return err
	}
	if err := outputs.finish(); err != nil {
		return err
	}
	p.roots = []publishRoot{{rel: p.unit.rel, outputs: outputs}}
	return nil
}

// componentConversion accumulates policy and manifests for one legacy component.
type componentConversion struct {
	plan      *unitPlan
	comp      *componentInfo
	outputs   *outputSet
	prefix    string // output prefix of this component
	source    string // snapshot key of this component root
	policies  map[string]migrationInputPolicy
	manifests []manifestRow
	wrappers  bool // assets/ may contain legacy namespaces
	nested    map[*libraryNode]bool
}

func (p *unitPlan) convertTree(c *componentInfo, prefix string, outputs *outputSet) error {
	source := joinRel(p.unit.rel, c.rel)
	if !c.legacy {
		// A current component inside a converted bundle is copied unchanged.
		var copyNode func(n *libraryNode, rel string) error
		copyNode = func(n *libraryNode, rel string) error {
			for _, child := range n.children {
				childRel := joinRel(rel, child.name)
				key := joinRel(source, childRel)
				if err := p.checkName(childRel, key); err != nil {
					return err
				}
				if child.dir {
					if err := outputs.add(&plannedOutput{rel: joinRel(prefix, childRel), dir: true, origin: key}); err != nil {
						return err
					}
					if err := copyNode(child, childRel); err != nil {
						return err
					}
					continue
				}
				if err := outputs.add(&plannedOutput{rel: joinRel(prefix, childRel), source: key, origin: key}); err != nil {
					return err
				}
			}
			return nil
		}
		return copyNode(c.node, "")
	}
	cv := &componentConversion{plan: p, comp: c, outputs: outputs, prefix: prefix, source: source,
		wrappers: c.legacyDesc, nested: map[*libraryNode]bool{}}
	for _, child := range c.children {
		cv.nested[child.node] = true
	}
	if err := cv.sharedComponent(); err != nil {
		return err
	}
	for _, child := range c.children {
		if err := p.convertTree(child, joinRel(prefix, strings.TrimPrefix(child.rel[len(c.rel):], "/")), outputs); err != nil {
			return err
		}
	}
	return nil
}

func (p *unitPlan) checkName(rel, origin string) error {
	for _, segment := range strings.Split(rel, "/") {
		if !engineNameValid(segment) {
			return rejectOverride("%s: the name %q is not usable in a package", origin, segment)
		}
	}
	return nil
}

func (cv *componentConversion) key(local string) string { return joinRel(cv.source, local) }

// put adds an engine resource below assets/.
func (cv *componentConversion) put(target, local string) error {
	origin := cv.key(local)
	engine := strings.TrimPrefix(target, "assets/")
	if !enginePathValid(engine) {
		return rejectOverride("%s: %s is not a usable engine path", origin, engine)
	}
	if lower := asciiLower(engine); strings.HasPrefix(lower, "generated/decls/") && !legacyDeclIdentity(engine[len("generated/decls/"):]) {
		return rejectOverride("%s: %s is not a valid declaration path; rename or move it, then run migrate-overrides again", origin, engine)
	}
	return cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, target), source: origin, origin: origin})
}

// aux keeps inactive authored data outside assets/.
func (cv *componentConversion) aux(target, local string) error {
	origin := cv.key(local)
	if err := cv.plan.checkName(target, origin); err != nil {
		return err
	}
	if strings.EqualFold(filepath.Base(target), "package.json") {
		return rejectOverride("%s: an inactive package.json cannot stay inside the package; move it out, then run migrate-overrides again", origin)
	}
	return cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, target), source: origin, origin: origin})
}

func (cv *componentConversion) dir(target, local string) error {
	origin := cv.key(local)
	if err := cv.plan.checkName(target, origin); err != nil {
		return err
	}
	return cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, target), dir: true, origin: origin})
}

func (cv *componentConversion) read(local string) ([]byte, error) {
	key := cv.key(local)
	return readVerified(cv.plan.abs(key), cv.plan.snapshot[key])
}

func splitRel(rel string) []string { return strings.Split(rel, "/") }

// Historical namespaces of a package root (overrides.c g_ov_namespaces and the
// policy readers). The returned target is "" when the path is not translated.
func namespaceTarget(segments []string) string {
	switch strings.ToLower(segments[0]) {
	case "decls":
		return joinRel("assets/generated/decls", strings.Join(segments[1:], "/"))
	case "images":
		return joinRel("assets/generated/image", strings.Join(segments[1:], "/"))
	case "shaders":
		if len(segments) >= 3 && strings.EqualFold(segments[1], "generated") &&
			(strings.EqualFold(segments[2], "spirv") || strings.EqualFold(segments[2], "renderprogs")) {
			return "assets/" + strings.Join(segments[1:], "/")
		}
	}
	return ""
}

func (cv *componentConversion) policy(local string, segments []string) (bool, error) {
	if len(segments) != 2 {
		return false, nil
	}
	first := asciiLower(segments[0])
	recognized := (first == "resources" && hasSuffixFold(segments[1], ".manifest")) ||
		(first == "requirements" && hasSuffixFold(segments[1], ".requirements")) ||
		(first == "strings" && hasSuffixFold(segments[1], ".json")) ||
		(first == "hud" && strings.EqualFold(segments[1], "weapons.json"))
	if !recognized {
		return false, nil
	}
	body, err := cv.read(local)
	if err != nil {
		return true, err
	}
	if !utf8.Valid(body) {
		return true, rejectOverride("%s: policy is not valid UTF-8", cv.key(local))
	}
	if cv.policies == nil {
		cv.policies = map[string]migrationInputPolicy{}
	}
	cv.policies[cv.key(local)] = migrationInputPolicy{Path: strings.Join(segments, "/"), Body: string(body)}
	return true, nil
}

// The partially migrated layout moved legacy namespaces under assets/. Paths
// that are installed engine content keep their current meaning.
func (cv *componentConversion) wrapped(local string, inner []string) error {
	engine := strings.Join(inner, "/")
	content, err := cv.plan.ctx.catalog.engineContent(engine)
	if err != nil {
		return err
	}
	if content {
		return cv.put(local, local)
	}
	first := strings.ToLower(inner[0])
	switch {
	case first == "decls" && len(inner) > 1:
		if legacyDeclIdentity(strings.Join(inner[1:], "/")) {
			return cv.put(namespaceTarget(inner), local)
		}
		return cv.aux(engine, local)
	case first == "images" && len(inner) > 1:
		return cv.put(namespaceTarget(inner), local)
	case first == "shaders" && namespaceTarget(inner) != "" && len(inner) > 3:
		return cv.put(namespaceTarget(inner), local)
	}
	if handled, err := cv.policy(local, inner); handled || err != nil {
		return err
	}
	return cv.put(local, local)
}

func (cv *componentConversion) emptyDir(local string) error {
	segments := splitRel(local)
	if strings.EqualFold(segments[0], "assets") && cv.wrappers && len(segments) > 1 {
		inner := segments[1:]
		content, err := cv.plan.ctx.catalog.engineDirectory(strings.Join(inner, "/"))
		if err != nil {
			return err
		}
		if !content {
			if target := namespaceTarget(inner); target != "" {
				return cv.dir(target, local)
			}
		}
		return cv.dir(local, local)
	}
	if target := namespaceTarget(segments); target != "" {
		return cv.dir(target, local)
	}
	return cv.dir(local, local)
}

// Manifest rows reference installed records. Authored files win exactly as
// they did in the provider, and records identical to stock SnapMap data remain
// installed dependencies.
func (cv *componentConversion) materialize() error {
	if len(cv.manifests) == 0 {
		return nil
	}
	rows := append([]manifestRow(nil), cv.manifests...)
	sort.SliceStable(rows, func(i, j int) bool {
		a, b := rows[i], rows[j]
		if !strings.EqualFold(a.kind, b.kind) {
			return asciiLower(a.kind) < asciiLower(b.kind)
		}
		if !strings.EqualFold(a.name, b.name) {
			return asciiLower(a.name) < asciiLower(b.name)
		}
		return asciiLower(a.provider) < asciiLower(b.provider)
	})
	claimed := map[string]manifestRow{}
	var unique []manifestRow
	for _, row := range rows {
		key := asciiLower(row.provider)
		if old, ok := claimed[key]; ok {
			if strings.EqualFold(old.kind, row.kind) && strings.EqualFold(old.name, row.name) {
				continue
			}
			return rejectOverride("%s and %s claim %s for different resources", old.origin, row.origin, row.provider)
		}
		claimed[key] = row
		unique = append(unique, row)
	}
	stock := 0
	for _, row := range unique {
		record, err := cv.plan.ctx.catalog.resolveManifest(row.kind, row.name, row.provider)
		if err != nil {
			var rejected *overrideRejected
			if errors.As(err, &rejected) {
				return rejectOverride("%s: %v", row.origin, err)
			}
			return err
		}
		target := "assets/" + row.provider
		if !enginePathValid(row.provider) {
			return rejectOverride("%s: %s is not a usable engine path", row.origin, row.provider)
		}
		if cv.outputs.has(joinRel(cv.prefix, target)) {
			continue
		}
		identical, err := cv.plan.ctx.catalog.stockIdentical(record)
		if err != nil {
			return err
		}
		if identical {
			stock++
			continue
		}
		if lower := asciiLower(row.provider); strings.HasPrefix(lower, "generated/decls/") && !legacyDeclIdentity(row.provider[len("generated/decls/"):]) {
			return rejectOverride("%s: %s is not a valid declaration path", row.origin, row.provider)
		}
		if err := cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, target), record: record, origin: row.origin}); err != nil {
			return err
		}
	}
	if stock > 0 {
		cv.plan.note("%s: %d manifest record(s) are identical to installed SnapMap resources and remain installed dependencies", cv.source, stock)
	}
	return nil
}

// Loose-source discovery supplies historical path aliases; descriptor and
// policy conversion still use the same planner as every marked package.
func (cv *componentConversion) descriptor() error {
	request := migrationInput{Descriptor: "{}", ID: cv.plan.ctx.assignID(cv.plan.unit.rel),
		Name: "My overrides", Legacy: true, Files: map[string]migrationInputFile{}, Policies: cv.policies}
	response, err := runNativeMigration(request)
	if err != nil {
		return err
	}
	var plan migrationOutput
	if err := json.Unmarshal(response, &plan); err != nil {
		return err
	}
	for _, row := range plan.Imports {
		cv.manifests = append(cv.manifests, manifestRow{row.Kind, row.Name, row.Provider, row.Origin})
	}
	if err := validateDescriptor(plan.Descriptor); err != nil {
		return err
	}
	return cv.outputs.add(&plannedOutput{rel: joinRel(cv.prefix, "package.json"), data: plan.Descriptor,
		origin: joinRel(cv.source, "package.json")})
}

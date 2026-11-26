package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
)

type earlierOwner struct {
	Salt     string            `json:"mod"`
	Folder   string            `json:"folder"`
	Entities map[string]string `json:"entities"`
}

type earlierPlacements struct {
	plan    *migrationPlan
	owners  map[string]*earlierOwner
	byName  map[string][]string
	staged  map[string]string
	preview func([]string) error
	width   int
}

func (p *migrationPlan) readPlacements() (*earlierPlacements, error) {
	a := &earlierPlacements{plan: p, owners: map[string]*earlierOwner{}, byName: map[string][]string{}, staged: map[string]string{}}
	file := filepath.Join(p.data, "cache", "legacy-placements.json")
	if !plainPath(file) {
		return nil, fmt.Errorf("linked earlier placement inventory")
	}
	body, err := os.ReadFile(file)
	if os.IsNotExist(err) {
		return a, nil
	}
	if err != nil {
		return nil, err
	}
	var inv struct {
		Format int
		Owners []earlierOwner
	}
	if json.Unmarshal(body, &inv) != nil || inv.Format != 1 {
		return nil, fmt.Errorf("unrecognized earlier placement inventory")
	}
	for _, o := range inv.Owners {
		if !modSalt.MatchString(o.Salt) || !pinRelative(o.Folder) || strings.HasPrefix(o.Folder, ".") {
			return nil, fmt.Errorf("invalid earlier placement owner")
		}
		for name, ref := range o.Entities {
			if !pinRelative(name) || !strings.HasPrefix(ref, "entities/") || !pinRelative(ref) {
				return nil, fmt.Errorf("invalid earlier entity reference")
			}
			if err := a.add(o.Salt, o.Folder, name, ref); err != nil {
				return nil, err
			}
		}
	}
	return a, nil
}

func (a *earlierPlacements) add(salt, folder, name, ref string) error {
	o := a.owners[salt]
	if o == nil {
		o = &earlierOwner{salt, folder, map[string]string{}}
		a.owners[salt] = o
	}
	if o.Folder != folder || (o.Entities[name] != "" && o.Entities[name] != ref) {
		return fmt.Errorf("conflicting earlier placement owner for %s", name)
	}
	if o.Entities[name] == "" {
		a.byName[name] = append(a.byName[name], salt)
		o.Entities[name] = ref
		a.width = max(a.width, len(name)+2)
	}
	return nil
}

func (a *earlierPlacements) save() error {
	if len(a.owners) == 0 {
		return nil
	}
	var owners []*earlierOwner
	for _, o := range a.owners {
		owners = append(owners, o)
	}
	sort.Slice(owners, func(i, j int) bool { return owners[i].Salt < owners[j].Salt })
	return a.plan.putBytes(filepath.Join(a.plan.data, "cache", "legacy-placements.json"), pinJSON(map[string]any{"format": 1, "owners": owners}), "Mods", true)
}

// Retain the earlier name only when its own installed file becomes an authored entity.
func (p *migrationPlan) translateLocalAuthor(src, dst, folder string, b *earlierBundle, consumed map[string]bool, recovered map[string]string) (string, error) {
	rel, err := filepath.Rel(filepath.Join(p.data, "mods"), folder)
	if err != nil || !filepath.IsLocal(rel) || rel == "." {
		return "", fmt.Errorf("invalid earlier mod folder %s", folder)
	}
	discover := func(id string, requests []string) (earlierGlobalNames, error) {
		var g earlierGlobalNames
		if b != nil {
			g = b.added
		} else {
			g, err = p.pins.sourceGlobalNames(id, filepath.Dir(dst), requests)
			if err != nil {
				return g, err
			}
		}
		salt := earlierSalt(id)
		for _, req := range requests {
			old, ok := strings.CutPrefix(req, "generated/decls/entitydef/")
			if !ok || !strings.HasSuffix(old, ".decl") {
				continue
			}
			old = strings.TrimSuffix(old, ".decl")
			author, err := earlierAuthorPath(id, req)
			if err != nil {
				return g, err
			}
			if f, ok := g.files[req]; ok {
				author = f.file
			}
			if !strings.HasPrefix(author, "entities/") {
				continue
			}
			ref := strings.TrimSuffix(author, ".decl")
			if old != "mods/"+salt+"/"+strings.TrimPrefix(ref, "entities/") {
				if err = p.pins.placements.add(salt, filepath.ToSlash(rel), old, ref); err != nil {
					return g, err
				}
			}
		}
		return g, nil
	}
	return translateEarlierFiles(src, dst, earlierGlobalNames{}, consumed, recovered, discover, p.pins.doom)
}

func (a *earlierPlacements) couldMention(raw []byte) bool {
	if bytes.IndexByte(raw, '\\') >= 0 {
		return true
	}
	folded := bytes.ToLower(raw)
	for name := range a.byName {
		if bytes.Contains(folded, []byte(name)) {
			return true
		}
	}
	return false
}

func earlierEntityPlacements(body []byte, known map[string][]string, allowCurrent bool) (string, []string) {
	var root struct {
		Type         string          `json:"~type"`
		MapMember    json.RawMessage `json:"snapmap-plus"`
		PrefabMember json.RawMessage `json:"snapmapPlus"`
		Entities     []struct {
			Type      string `json:"~type"`
			EntityDef struct {
				Type    string `json:"~type"`
				Inherit string
			}
		}
	}
	if json.Unmarshal(earlierJSONText(bytes.TrimPrefix(body, []byte{0xef, 0xbb, 0xbf})), &root) != nil ||
		(root.Type != "idSnapMap" && root.Type != "idSnapEntityPrefab") || !allowCurrent && (root.MapMember != nil || root.PrefabMember != nil) {
		return "", nil
	}
	var names []string
	for _, e := range root.Entities {
		k := strings.ToLower(e.EntityDef.Inherit)
		if e.Type == "idSnapEntity" && e.EntityDef.Type == "idDeclEntityDef" && pinRelative(k) && (known == nil || len(known[k]) > 0) {
			names = append(names, k)
		}
	}
	sort.Strings(names)
	return root.Type, slices.Compact(names)
}

func (a *earlierPlacements) matches(body []byte) (string, []string) {
	if len(a.byName) == 0 || !a.couldMention(body) {
		return "", nil
	}
	return earlierEntityPlacements(body, a.byName, false)
}

func hasHistoricalEvidence(body, side []byte, pins *earlierPins) bool {
	if documentEvidence(body, side) {
		return true
	}
	if pins != nil && pins.placements != nil {
		_, names := pins.placements.matches(body)
		return len(names) > 0
	}
	return false
}

// Materialize the final author folder from the staged changes before asking the normal compiler for a unit.
func (a *earlierPlacements) source(o *earlierOwner) (string, error) {
	if dir := a.staged[o.Salt]; dir != "" {
		return dir, nil
	}
	folder := filepath.Join(a.plan.data, "mods", filepath.FromSlash(o.Folder))
	if !plainPath(folder) {
		return "", fmt.Errorf("linked earlier mod folder %s", folder)
	}
	work, err := os.MkdirTemp(a.plan.work, "placement-*")
	if err != nil {
		return "", err
	}
	dst := filepath.Join(work, "mod")
	if err = os.Mkdir(dst, 0755); err != nil {
		return "", err
	}
	type input struct{ original, staged string }
	files := map[string]input{}
	err = walkLibrary(folder, func(f string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if f != folder && e.IsDir() && strings.HasPrefix(e.Name(), ".") {
			return filepath.SkipDir
		}
		if !e.IsDir() {
			if !e.Type().IsRegular() {
				return fmt.Errorf("invalid author file %s", f)
			}
			files[strings.ToLower(f)] = input{f, f}
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	for i, c := range a.plan.Changes {
		if !under(folder, c.Path) {
			continue
		}
		key := strings.ToLower(c.Path)
		if c.After == "" {
			delete(files, key)
			continue
		}
		files[key] = input{c.Path, filepath.Join(a.plan.work, fmt.Sprintf("new-%d", i))}
	}
	for _, file := range files {
		rel, err := filepath.Rel(folder, file.original)
		if err != nil || !filepath.IsLocal(rel) {
			return "", fmt.Errorf("invalid placed entity source")
		}
		to := filepath.Join(dst, rel)
		if err = os.MkdirAll(filepath.Dir(to), 0755); err != nil {
			return "", err
		}
		if err = copyFile(file.staged, to); err != nil {
			return "", err
		}
	}
	raw, err := os.ReadFile(filepath.Join(dst, "mod.json"))
	if err != nil {
		return "", fmt.Errorf("earlier placement needs its installed mod %s", folder)
	}
	var doc struct {
		Block struct{ Salt string } `json:"snapmap-plus"`
	}
	if json.Unmarshal(raw, &doc) != nil || doc.Block.Salt != o.Salt {
		return "", fmt.Errorf("another mod occupies %s", folder)
	}
	a.staged[o.Salt] = work
	return work, nil
}

func (p *earlierPins) translatePlacements(original, out []byte) ([]byte, error) {
	a := p.placements
	var kind string
	var names []string
	if a.preview != nil {
		kind, names = earlierEntityPlacements(original, nil, false)
	} else {
		kind, names = a.matches(original)
	}
	if len(names) == 0 {
		return out, nil
	}
	if !bytes.Equal(original, out) {
		_, remaining := earlierEntityPlacements(out, nil, true)
		names = slices.DeleteFunc(names, func(name string) bool { _, present := slices.BinarySearch(remaining, name); return !present })
	}
	if a.preview != nil {
		if err := a.preview(names); err != nil {
			return nil, err
		}
		names = slices.DeleteFunc(names, func(name string) bool { return len(a.byName[name]) == 0 })
	}
	if len(names) == 0 {
		return out, nil
	}
	var requests []string
	for _, name := range names {
		requests = append(requests, "generated/decls/entitydef/"+name+".decl")
	}
	stock, err := p.classifyRequests(a.plan.work, requests)
	if err != nil {
		return nil, err
	}
	byOwner := map[string][]string{}
	var g earlierGlobalNames
	for _, name := range names {
		if stock["generated/decls/entitydef/"+name+".decl"] {
			continue
		}
		owners := a.byName[name]
		if len(owners) != 1 {
			return nil, fmt.Errorf("multiple earlier mods provided the placed entity %s; kept the original", name)
		}
		o := a.owners[owners[0]]
		ref := o.Entities[name]
		f := earlierGlobalName{"entitydef", name, ref + ".decl", ref, "mods/" + o.Salt + "/" + strings.TrimPrefix(ref, "entities/"), ""}
		if err := g.add(f); err != nil {
			return nil, err
		}
		byOwner[o.Salt] = append(byOwner[o.Salt], ref)
	}
	if len(byOwner) == 0 {
		return out, nil
	}
	var rows []earlierObject
	for _, salt := range slices.Sorted(maps.Keys(byOwner)) {
		o := a.owners[salt]
		w, err := a.source(o)
		if err != nil {
			return nil, err
		}
		var doc struct{ Name string }
		raw, err := os.ReadFile(filepath.Join(w, "mod", "mod.json"))
		if err != nil {
			return nil, err
		}
		if err = json.Unmarshal(raw, &doc); err != nil {
			return nil, err
		}
		for _, ref := range byOwner[salt] {
			if !isRegular(filepath.Join(w, "mod", filepath.FromSlash(ref+".decl"))) {
				return nil, fmt.Errorf("the mod no longer provides %s", ref)
			}
		}
		row, err := p.slice(w, salt, doc.Name, byOwner[salt], false, "")
		if err != nil {
			return nil, err
		}
		if row["whole"] != nil {
			return nil, fmt.Errorf("could not resolve the exact placed entity in %s", o.Folder)
		}
		rows = append(rows, row)
	}
	next, err := g.placedReferences(out)
	if err != nil {
		return nil, err
	}
	member, format := "snapmap-plus", 2
	if kind == "idSnapEntityPrefab" {
		member, format = "snapmapPlus", 1
	}
	var root earlierObject
	if json.Unmarshal(earlierJSONText(bytes.TrimPrefix(next, []byte{0xef, 0xbb, 0xbf})), &root) != nil {
		return nil, fmt.Errorf("invalid earlier placement document")
	}
	block := earlierObject{"format": pinJSON(format)}
	if r := root[member]; r != nil {
		if err = json.Unmarshal(r, &block); err != nil {
			return nil, err
		}
	}
	var existing []earlierObject
	if r := block["mods"]; r != nil {
		if err = json.Unmarshal(r, &existing); err != nil {
			return nil, err
		}
	}
	for _, row := range rows {
		for _, prior := range existing {
			if pinString(row, "mod") == pinString(prior, "mod") {
				return nil, fmt.Errorf("the placed entity already has a different recorded mod requirement")
			}
		}
		existing = append(existing, row)
	}
	block["mods"] = pinJSON(existing)
	return earlierSetMember(next, member, pinJSON(block))
}

// A dropped export can precede the full profile update. Stage only installed owners named by its placements.
func (p *migrationPlan) previewPlacementOwners(file string) error {
	var raw []byte
	var err error
	if strings.EqualFold(filepath.Ext(file), ".zip") {
		z, e := zip.OpenReader(file)
		if e != nil {
			return nil
		}
		defer z.Close()
		doc, _, _, e := earlierArchiveDocument(z)
		if e != nil {
			return nil
		}
		raw, err = earlierEntry(doc)
	} else {
		raw, err = os.ReadFile(file)
	}
	if err != nil {
		return err
	}
	if documentEvidence(raw, nil) {
		// Recorded content takes precedence over sources installed later.
		p.pins.placements.preview = p.stagePlacementOwners
		return nil
	}
	_, names := earlierEntityPlacements(raw, nil, false)
	return p.stagePlacementOwners(names)
}

func (p *migrationPlan) stagePlacementOwners(names []string) error {
	if len(names) == 0 {
		return nil
	}
	requests := map[string]bool{}
	for _, name := range names {
		requests["generated/decls/entitydef/"+name+".decl"] = true
	}
	work, err := os.MkdirTemp(p.work, "sources-*")
	if err != nil {
		return err
	}
	var retail map[string]bool
	hasAddition := func(present func(string) bool) (bool, error) {
		for req := range requests {
			if !present(req) {
				continue
			}
			if retail == nil {
				// Ordinary maps with no matching earlier source do not need installed game archives.
				var err error
				retail, err = p.pins.classifyRequests(work, slices.Sorted(maps.Keys(requests)))
				if err != nil {
					return false, err
				}
			}
			if !retail[req] {
				return true, nil
			}
		}
		return false, nil
	}
	stage := &migrationPlan{Roots: p.Roots, data: p.data, work: work, destinations: map[string]int{}, pins: p.pins}
	if err = stage.preferences(); err != nil {
		return err
	}
	stage.pins.placements.plan = stage
	mods := filepath.Join(p.data, "mods")
	for _, home := range p.legacyHomes() {
		for _, group := range []string{"mods", "overrides"} {
			if stage.keepOverrides && group == "overrides" {
				continue
			}
			base := filepath.Join(home, group)
			err = walkLibrary(base, func(dir string, e fs.DirEntry, _ error) error {
				if !e.IsDir() || dir == base {
					return nil
				}
				if strings.HasPrefix(e.Name(), ".") {
					return filepath.SkipDir
				}
				var doc map[string]any
				var descriptor string
				if isRegular(filepath.Join(dir, "package.json")) {
					doc, descriptor, err = earlierDescriptor(dir)
					if err != nil {
						return filepath.SkipDir
					}
					if descriptor != "package.json" {
						return filepath.SkipDir
					}
					files, e := earlierPackageFiles(dir, doc, &earlierResourceRoot{doom: p.pins.doom, namesOnly: true})
					if e != nil {
						// A damaged sibling cannot identify a placed object it does not contain.
						possible := map[string]bool{}
						walkErr := walkLibrary(dir, func(f string, entry fs.DirEntry, _ error) error {
							if entry.Type().IsRegular() {
								path := strings.ToLower(filepath.ToSlash(f))
								for req := range requests {
									if strings.HasSuffix(path, "/"+req) || strings.HasSuffix(path, "/"+strings.TrimPrefix(req, "generated/")) {
										possible[req] = true
									}
								}
							}
							return nil
						})
						if walkErr != nil {
							return walkErr
						}
						implicated, err := hasAddition(func(req string) bool { return possible[req] })
						if err != nil {
							return err
						}
						if implicated {
							return e
						}
						return filepath.SkipDir
					}
					found, e := hasAddition(func(req string) bool { return files[req] != "" })
					if e != nil {
						return e
					}
					if !found {
						return filepath.SkipDir
					}
				} else if isRegular(filepath.Join(dir, "mod.json")) || isRegular(filepath.Join(dir, "bundle.json")) {
					if !earlierPathExists(filepath.Join(dir, "assets")) {
						return filepath.SkipDir
					}
					found, e := hasAddition(func(req string) bool { return isRegular(filepath.Join(dir, "assets", filepath.FromSlash(req))) })
					if e != nil {
						return e
					}
					if !found {
						return filepath.SkipDir
					}
					doc, _, err = earlierDescriptor(dir)
					if err != nil {
						return err
					}
				} else {
					return nil
				}
				rel, err := filepath.Rel(base, dir)
				if err != nil {
					return err
				}
				if err = stage.convertMod(dir, filepath.Join(mods, rel), prunedAuthorPaths(doc)); err != nil {
					return err
				}
				return filepath.SkipDir
			})
			if err != nil {
				return err
			}
		}
		if !stage.keepOverrides {
			found, err := hasAddition(func(req string) bool { return isRegular(filepath.Join(home, "overrides", filepath.FromSlash(req))) })
			if err != nil {
				return err
			}
			if found {
				if err = stage.loose(home); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

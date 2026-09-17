package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Discovery reads only folder listings, never file contents. A folder with
// package.json is one outer package, exactly as sh_packages_enumerate finds
// it; unmarked folders are groups. Loose files outside packages are legacy
// exact-path overrides only where the historical resolver served them.

type overrideRejected struct{ reason string }

func (e *overrideRejected) Error() string { return e.reason }

func rejectOverride(format string, args ...any) error {
	return &overrideRejected{fmt.Sprintf(format, args...)}
}

type libraryNode struct {
	rel      string // slash-separated, relative to the scanned root
	name     string
	dir      bool
	unsafe   bool // link, junction or other reparse point
	marked   bool // directory containing a regular package.json
	markers  bool // marked itself or anywhere below
	size     int64
	children []*libraryNode
}

// listLibrary lists a tree without following links. Marked folders are not
// descended into when stopAtMarkers is set: discovery treats them as leaves.
func listLibrary(abs, rel string, stopAtMarkers bool) (*libraryNode, error) {
	node := &libraryNode{rel: rel, name: filepath.Base(abs), dir: true}
	entries, err := os.ReadDir(abs)
	if err != nil {
		return nil, err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
	for _, e := range entries {
		info, err := e.Info()
		if err != nil {
			return nil, err
		}
		childRel := e.Name()
		if rel != "" {
			childRel = rel + "/" + e.Name()
		}
		child := &libraryNode{rel: childRel, name: e.Name(), size: info.Size()}
		// Junctions report a directory bit together with ModeIrregular.
		switch mode := info.Mode(); {
		case mode&(os.ModeSymlink|os.ModeIrregular) != 0:
			child.unsafe = true
		case mode.IsDir():
			child.dir = true
		case !mode.IsRegular():
			child.unsafe = true
		}
		if child.dir {
			marker, err := os.Lstat(filepath.Join(abs, e.Name(), "package.json"))
			if err == nil && marker.Mode().IsRegular() {
				child.marked = true
			} else if err != nil && !os.IsNotExist(err) {
				return nil, err
			}
			if !child.marked || !stopAtMarkers {
				listed, err := listLibrary(filepath.Join(abs, e.Name()), childRel, stopAtMarkers)
				if err != nil {
					return nil, err
				}
				listed.marked = child.marked
				child = listed
			}
			child.markers = child.marked
			for _, grandchild := range child.children {
				child.markers = child.markers || grandchild.markers
			}
		}
		node.children = append(node.children, child)
	}
	return node, nil
}

func (n *libraryNode) child(name string) *libraryNode {
	for _, c := range n.children {
		if strings.EqualFold(c.name, name) {
			return c
		}
	}
	return nil
}

func (n *libraryNode) walk(visit func(*libraryNode) bool) {
	for _, c := range n.children {
		if visit(c) && c.dir {
			c.walk(visit)
		}
	}
}

func (n *libraryNode) hasFile() bool {
	found := false
	n.walk(func(c *libraryNode) bool {
		found = found || (!c.dir && !c.unsafe)
		return !found
	})
	return found
}

func (n *libraryNode) hasUnsafe() bool {
	found := n.unsafe
	n.walk(func(c *libraryNode) bool {
		found = found || c.unsafe
		return !found
	})
	return found
}

type unitKind int

const (
	unitPackage   unitKind = iota // marked folder, converted in place
	unitSynthetic                 // unmarked package-shaped folder, converted in place
	unitLoose                     // loose engine content, published as a new package
)

type looseRole int

const (
	looseGenerated looseRole = iota // pre-package overrides/generated tree
	looseInclude                    // overrides/shader_includes
	looseHybrid                     // unmarked overrides/assets in the current layout
	looseEngine                     // exact engine paths below an engine root
)

type looseSource struct {
	rel  string
	dir  bool
	role looseRole
}

type overrideUnit struct {
	kind  unitKind
	rel   string
	loose []looseSource
}

type libraryDiscovery struct {
	units   []overrideUnit
	skipped []string
}

// Legacy namespaces inside a package root, as consumed by the old readers.
func packageShaped(n *libraryNode) bool {
	for _, c := range n.children {
		if !c.dir || c.unsafe {
			continue
		}
		switch strings.ToLower(c.name) {
		case "assets", "images":
			if c.hasFile() {
				return true
			}
		case "decls":
			found := false
			c.walk(func(d *libraryNode) bool {
				if !d.dir && !d.unsafe && legacyDeclIdentity(strings.TrimPrefix(d.rel, c.rel+"/")) {
					found = true
				}
				return !found
			})
			if found {
				return true
			}
		case "shaders":
			if generated := c.child("generated"); generated != nil {
				for _, family := range []string{"spirv", "renderprogs"} {
					if f := generated.child(family); f != nil && f.dir && f.hasFile() {
						return true
					}
				}
			}
		case "resources", "requirements", "strings":
			suffix := map[string]string{"resources": ".manifest", "requirements": ".requirements", "strings": ".json"}[strings.ToLower(c.name)]
			for _, f := range c.children {
				if !f.dir && !f.unsafe && strings.HasSuffix(strings.ToLower(f.name), suffix) {
					return true
				}
			}
		case "hud":
			if f := c.child("weapons.json"); f != nil && !f.dir && !f.unsafe {
				return true
			}
		}
	}
	return false
}

// A path whose first ".inc" ends it was always resolved through
// shader_includes, never as an exact loose file.
func includeTerminal(path string) bool {
	return strings.Index(path, ".inc") == len(path)-4
}

func discoverOverrides(root string, catalog *overrideCatalog) (*libraryDiscovery, error) {
	tree, err := listLibrary(root, "", true)
	if err != nil {
		return nil, fmt.Errorf("cannot read the overrides folder completely: %w", err)
	}
	d := &libraryDiscovery{}
	loose := overrideUnit{kind: unitLoose}
	addLoose := func(n *libraryNode, role looseRole) {
		loose.loose = append(loose.loose, looseSource{n.rel, n.dir, role})
	}
	skip := func(n *libraryNode) {
		d.skipped = append(d.skipped, n.rel)
	}

	// Content of a strong loose root: every non-package item belongs to the
	// loose unit. Subtrees without markers or links move as one source.
	var strong func(n *libraryNode, role looseRole)
	strong = func(n *libraryNode, role looseRole) {
		for _, c := range n.children {
			switch {
			case c.unsafe:
				skip(c)
			case c.dir && c.marked:
				d.units = append(d.units, overrideUnit{kind: unitPackage, rel: c.rel})
			case c.dir && (c.markers || c.hasUnsafe()):
				strong(c, role)
			default:
				addLoose(c, role)
			}
		}
	}

	// A loose root without packages or links moves as one source.
	looseRoot := func(n *libraryNode, role looseRole) {
		if !n.markers && !n.hasUnsafe() {
			addLoose(n, role)
			return
		}
		strong(n, role)
	}

	// Groups: packages below are units; other content is inert unless it is
	// an exact installed engine path.
	var group func(n *libraryNode) error
	group = func(n *libraryNode) error {
		for _, c := range n.children {
			switch {
			case c.unsafe:
				skip(c)
			case c.dir && c.marked:
				d.units = append(d.units, overrideUnit{kind: unitPackage, rel: c.rel})
			case c.dir && c.markers:
				if err := group(c); err != nil {
					return err
				}
			case c.dir && packageShaped(c):
				d.units = append(d.units, overrideUnit{kind: unitSynthetic, rel: c.rel})
			case c.dir:
				var failure error
				c.walk(func(f *libraryNode) bool {
					if failure != nil || f.dir || f.unsafe || includeTerminal(asciiLower(f.rel)) {
						return failure == nil
					}
					exact, err := catalog.hasPath(f.rel)
					if err != nil {
						failure = err
					} else if exact {
						addLoose(f, looseEngine)
					}
					return failure == nil
				})
				if failure != nil {
					return failure
				}
			default:
				if includeTerminal(asciiLower(c.rel)) {
					continue
				}
				exact, err := catalog.hasPath(c.rel)
				if err != nil {
					return err
				}
				if exact {
					addLoose(c, looseEngine)
				}
			}
		}
		return nil
	}

	for _, c := range tree.children {
		lower := strings.ToLower(c.name)
		switch {
		case c.unsafe:
			skip(c)
		case c.dir && c.marked:
			d.units = append(d.units, overrideUnit{kind: unitPackage, rel: c.rel})
		case c.dir && lower == "generated":
			looseRoot(c, looseGenerated)
		case c.dir && lower == "shader_includes":
			looseRoot(c, looseInclude)
		case c.dir && lower == "assets" && !c.markers:
			looseRoot(c, looseHybrid)
		case c.dir && c.markers:
			if err := group(c); err != nil {
				return nil, err
			}
		case c.dir && packageShaped(c):
			d.units = append(d.units, overrideUnit{kind: unitSynthetic, rel: c.rel})
		case c.dir:
			root, err := catalog.isRootIfNeeded(lower, c)
			if err != nil {
				return nil, err
			}
			if root {
				looseRoot(c, looseEngine)
			}
		default:
			if includeTerminal(asciiLower(c.rel)) {
				continue
			}
			exact, err := catalog.hasPath(c.rel)
			if err != nil {
				return nil, err
			}
			if exact {
				addLoose(c, looseEngine)
			}
		}
	}
	if len(loose.loose) > 0 {
		name := starterPackageName
		for i := 2; tree.child(name) != nil; i++ {
			name = fmt.Sprintf("%s-%d", starterPackageName, i)
		}
		loose.rel = name
		d.units = append(d.units, loose)
	}
	return d, nil
}

// Only folders that contain files need the catalog to classify them.
func (c *overrideCatalog) isRootIfNeeded(segment string, n *libraryNode) (bool, error) {
	if !n.hasFile() {
		return false, nil
	}
	return c.isRoot(segment)
}

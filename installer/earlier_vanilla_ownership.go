package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Only recorded names or a recorded owning namespace implicate a placement or a typed override.
type vanillaOwners struct {
	historical map[string]vanillaBinding
	sites      map[int][]vanillaSite
	names      map[string]bool
	ids        map[string]bool
	stock      map[string]map[string]bool
	doom       string
}

func (o *vanillaOwners) row(r earlierObject) error {
	id := pinString(r, "id")
	m := pinString(r, "mod")
	if id != "" || r["bundle"] != nil {
		if !earlierID.MatchString(id) || !pinHash(pinString(r, "bundle")) {
			return fmt.Errorf("invalid earlier mod identity")
		}
	} else {
		var h string
		var old bool
		id, h, old = strings.Cut(m, "@")
		if !modSalt.MatchString(id) || old && !pinHash(h) {
			return fmt.Errorf("invalid saved mod identity")
		}
	}
	o.ids[strings.ToLower(id)] = true
	o.ids[strings.ReplaceAll(strings.ToLower(id), "-", "_")] = true
	var names []string
	if raw := r["names"]; raw != nil && json.Unmarshal(raw, &names) != nil {
		return fmt.Errorf("invalid earlier selected names")
	}
	for _, s := range names {
		if strings.TrimSpace(s) == "" {
			return fmt.Errorf("invalid empty selected name")
		}
		o.names[strings.ToLower(s)] = true
	}
	var objects map[string]string
	if raw := r["objects"]; raw != nil {
		if json.Unmarshal(raw, &objects) != nil {
			return fmt.Errorf("invalid saved object units")
		}
		for s, h := range objects {
			if !pinHash(h) {
				return fmt.Errorf("invalid saved object hash")
			}
			o.names[strings.ToLower(s)] = true
		}
	}
	return nil
}

func (o *vanillaOwners) carried(root earlierObject, sets map[string]*earlierCarrier, _ map[string]string) ([]json.RawMessage, error) {
	for _, c := range sets {
		tmp, e := os.MkdirTemp("", "snapmap-plus-vanilla-*")
		if e != nil {
			return nil, e
		}
		m, e := c.readSource(tmp)
		removeErr := os.RemoveAll(tmp)
		if e != nil {
			return nil, e
		}
		if removeErr != nil {
			return nil, removeErr
		}
		o.ids[strings.ToLower(m.id)] = true
		o.ids[strings.ReplaceAll(strings.ToLower(m.id), "-", "_")] = true
		for _, request := range m.requests {
			r := strings.ToLower(request)
			if rest, ok := strings.CutPrefix(r, "generated/decls/"); ok {
				_, name, ok := strings.Cut(rest, "/")
				if ok {
					o.names[strings.TrimSuffix(name, ".decl")] = true
				}
			} else {
				o.names[r] = true
			}
		}
	}
	return nil, nil
}

func (o *vanillaOwners) implicated(value string) bool {
	name := strings.ToLower(value)
	if o.names[name] {
		return true
	}
	parts := strings.Split(name, "/")
	for i := 0; i+2 < len(parts); i++ {
		if parts[i] == "mods" && o.ids[parts[i+1]] {
			return true
		}
	}
	return false
}

// SnapMap has its own typed retail catalog. A campaign-only asset can have exactly the same logical name.
func (o *vanillaOwners) unavailable(kind, name string) (bool, error) {
	if !o.implicated(name) {
		return false, nil
	}
	if o.stock == nil {
		doom := o.doom
		if doom == "" {
			var err error
			doom, err = resolveDoom("")
			if err != nil {
				return false, fmt.Errorf("need the installed SnapMap resource catalog: %w", err)
			}
		}
		index := map[string]map[string]bool{}
		err := visitEarlierResourceFile(filepath.Join(doom, "base", "snap_gameresources.pindex"), func(k, n, provider []byte, row earlierResourceSlice) error {
			if row.size == 0 || row.stored == 0 {
				return nil
			}
			low := strings.ToLower(string(n))
			// All explicitly requested names and any recorded namespace may have a retail provider.
			if !o.implicated(low) {
				return nil
			}
			if index[low] == nil {
				index[low] = map[string]bool{}
			}
			index[low][strings.ToLower(string(k))] = true
			return nil
		})
		if err != nil {
			return false, fmt.Errorf("cannot check the SnapMap resource catalog: %w", err)
		}
		o.stock = index
	}
	kinds := o.stock[strings.ToLower(name)]
	if kind == "model" {
		return !kinds["model"] && !kinds["md6def"], nil
	}
	return !kinds[strings.ToLower(kind)], nil
}

func (o *vanillaOwners) edit(raw []byte, container string) ([]byte, error) {
	if len(raw) == 0 || raw[0] != '{' {
		return raw, nil
	}
	return vanillaObject(raw, func(key string, value []byte) ([]byte, error) {
		return o.editValue(value, key, container)
	})
}

func (o *vanillaOwners) editValue(raw []byte, key, container string) ([]byte, error) {
	switch raw[0] {
	case '{':
		return o.edit(raw, key)
	case '[':
		incomplete := false
		out, err := vanillaArray(raw, func(_ int, item []byte) ([]byte, error) {
			next, e := o.editValue(item, key, container)
			incomplete = incomplete || next == nil
			return next, e
		})
		if err != nil {
			return nil, err
		}
		// Keep the native element positions and other authored fields within each element.
		if incomplete {
			return nil, fmt.Errorf("cannot remove a typed native array slot without changing later indices")
		}
		return out, nil
	case '"':
	default:
		return raw, nil
	}
	var name string
	if json.Unmarshal(raw, &name) != nil || !o.implicated(name) {
		return raw, nil
	}
	kind := earlierReferenceKind("entitydef", key, container)
	switch {
	case strings.EqualFold(container, "renderModelInfo") && strings.EqualFold(key, "model"):
		kind = "model"
	case strings.EqualFold(key, "customMaterial"):
		kind = "material"
	case strings.EqualFold(key, "soundShader"):
		kind = "sound"
	case strings.EqualFold(key, "animWeb"):
		kind = "animweb"
	}
	if kind == "" {
		switch strings.ToLower(key) {
		case "text", "caption", "description", "displayname", "name", "initialvalue", "jointname", "meshname", "statename":
			return raw, nil
		}
		if strings.Contains(name, "/") {
			return nil, fmt.Errorf("cannot establish the native type of the selected override %s", key)
		}
		return raw, nil
	}
	missing, err := o.unavailable(kind, name)
	if err != nil || missing {
		return nil, err
	}
	return raw, nil
}

func (o *vanillaOwners) entity(index int, raw []byte, g *vanillaLinks, seen map[int]bool) ([]byte, error) {
	var e struct {
		Type      string `json:"~type"`
		UniqueID  *int
		EntityDef struct {
			Type    string `json:"~type"`
			Inherit string
		}
	}
	if err := json.Unmarshal(earlierJSONText(raw), &e); err != nil {
		return nil, err
	}
	if e.UniqueID != nil {
		id := *e.UniqueID
		if id < 0 || seen[id] {
			return nil, fmt.Errorf("invalid or duplicate entity ID %d", id)
		}
		seen[id] = true
		if g.removed[id] {
			return nil, nil
		}
	}
	name := e.EntityDef.Inherit
	if (o.sites != nil || o.historical != nil) && strings.HasPrefix(name, "smp/") && pinHash(strings.TrimPrefix(name, "smp/")) {
		matches := o.historical[name].alias == name
		for _, site := range o.sites[index] {
			if len(site.path) == 2 && site.path[1] == "inherit" && site.binding.alias == name {
				matches = true
			}
		}
		if !matches {
			return nil, fmt.Errorf("no matching bound site for %s", name)
		}
	}
	if o.implicated(name) {
		if e.Type != "idSnapEntity" || e.EntityDef.Type != "idDeclEntityDef" {
			return nil, fmt.Errorf("unknown placed definition for %s", name)
		}
		missing, err := o.unavailable("entitydef", name)
		if err != nil {
			return nil, err
		}
		if missing {
			if e.UniqueID == nil {
				return nil, fmt.Errorf("placed mod entity %s has no uniqueId", name)
			}
			g.removed[*e.UniqueID] = true
			return nil, nil
		}
	}
	raw, err := vanillaObject(raw, func(k string, v []byte) ([]byte, error) {
		if k == "customIcon" {
			var icon struct {
				Type       string `json:"~type"`
				TargetType string
				Value      string
			}
			if json.Unmarshal(earlierJSONText(v), &icon) == nil && o.implicated(icon.Value) {
				if icon.Type != "|pointer" || icon.TargetType != "idDeclSnapCustomIcon" {
					return nil, fmt.Errorf("unknown custom icon type")
				}
				missing, err := o.unavailable("snapcustomicon", icon.Value)
				if err != nil {
					return nil, err
				}
				if missing {
					return earlierSetMember(v, "value", []byte("null"))
				}
			}
		}
		if k != "entityDef" {
			return v, nil
		}
		return vanillaObject(v, func(k string, v []byte) ([]byte, error) {
			if k != "state" {
				return v, nil
			}
			return vanillaObject(v, func(k string, v []byte) ([]byte, error) {
				if k == "edit" {
					return o.edit(v, "edit")
				}
				return v, nil
			})
		})
	})
	if err != nil {
		return nil, err
	}
	raw, drop, err := o.boundEntity(index, raw, name)
	if drop {
		if e.UniqueID == nil {
			return nil, fmt.Errorf("bound placement has no uniqueId")
		}
		g.removed[*e.UniqueID] = true
	}
	return raw, err
}

package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

type vanillaBinding struct{ family, original, alias string }
type vanillaSite struct {
	path    []string
	binding vanillaBinding
}
type vanillaBindingReader struct {
	*bytes.Buffer
	bad bool
}

func (r *vanillaBindingReader) number() int {
	b := r.Next(4)
	if len(b) != 4 {
		r.bad = true
		return 0
	}
	return int(binary.LittleEndian.Uint32(b))
}
func (r *vanillaBindingReader) word() string {
	n := r.number()
	if n > r.Len() {
		r.bad = true
		return ""
	}
	s := r.Next(n)
	if bytes.IndexByte(s, 0) >= 0 {
		r.bad = true
	}
	return string(s)
}
func (r *vanillaBindingReader) contextIDs(total int) {
	n := r.number()
	if n == 0 || n > r.Len()/4 {
		r.bad = true
		return
	}
	for range n {
		if r.number() >= total {
			r.bad = true
			return
		}
	}
}

// Decode the recorded declaration family, original name and exact object site from the SMPBIND writer.
func (o *vanillaOwners) bindings(body []byte) error {
	invalid := fmt.Errorf("cannot establish the prototype's recorded placement and original declarations")
	if len(body) < 8 || string(body[:7]) != "SMPBIND" || body[7] > 2 {
		return invalid
	}
	version := body[7]
	r := vanillaBindingReader{bytes.NewBuffer(body[8:]), false}
	contexts := r.number()
	if contexts > r.Len()/44 {
		return invalid
	}
	for range contexts {
		mode, n := r.number(), r.number()
		if (mode != 1 && mode != 2) || n == 0 || n > r.Len()/32 {
			return invalid
		}
		r.Next(n * 32)
		component := r.word()
		if mode == 2 && component != "" || mode == 1 && !pinRelative(component) {
			return invalid
		}
	}
	n := r.number()
	if r.bad || n > r.Len()/23 {
		return invalid
	}
	bindings := make([]vanillaBinding, n)
	for i := range bindings {
		b := vanillaBinding{r.word(), r.word(), r.word()}
		r.contextIDs(contexts)
		if r.bad || !pinRelative(b.family) || !pinRelative(b.original) || !pinRelative(b.alias) {
			return invalid
		}
		bindings[i] = b
		o.names[strings.ToLower(b.original)] = true
	}
	// Aggregate records have no per-placement sites. Use their original only on the matching typed parent.
	if version == 0 {
		for _, b := range bindings {
			if !strings.EqualFold(b.family, "entitydef") {
				return invalid
			}
			if o.historical == nil {
				o.historical = map[string]vanillaBinding{}
			}
			if prior, ok := o.historical[b.alias]; ok && prior != b {
				return invalid
			}
			o.historical[b.alias] = b
		}
	} else {
		n = r.number()
		if n > r.Len()/24 {
			return invalid
		}
		o.sites = map[int][]vanillaSite{}
		for range n {
			kind := r.number()
			owner, ref := r.word(), r.word()
			b := r.number()
			r.contextIDs(contexts)
			if version == 2 {
				row := r.word()
				if row != "" && !pinHash(row) {
					return invalid
				}
			}
			idx, ok := strings.CutPrefix(owner, "entity/")
			i, err := strconv.Atoi(idx)
			if r.bad || b >= len(bindings) || kind != 1 || !ok || err != nil || i < 0 || strconv.Itoa(i) != idx || !strings.HasPrefix(ref, "/k:entityDef/") {
				return invalid
			}
			keys := strings.Split(ref[1:], "/")
			for j, token := range keys {
				name, ok := strings.CutPrefix(token, "k:")
				if !ok || name == "" {
					return invalid
				}
				keys[j] = name
			}
			if len(keys) == 2 && keys[1] == "inherit" {
				if !strings.EqualFold(bindings[b].family, "entitydef") {
					return invalid
				}
			} else if len(keys) < 4 || keys[1] != "state" || keys[2] != "edit" {
				return invalid
			}
			o.sites[i] = append(o.sites[i], vanillaSite{keys, bindings[b]})
		}
	}
	if r.bad || r.Len() != 0 {
		return invalid
	}
	return nil
}

func vanillaReplaceSite(raw []byte, keys []string, alias string, next []byte) ([]byte, error) {
	if len(keys) == 0 {
		var current string
		if json.Unmarshal(raw, &current) != nil || current != alias {
			return nil, fmt.Errorf("recorded name %s does not match this placement", alias)
		}
		return next, nil
	}
	found := false
	out, err := vanillaObject(raw, func(k string, v []byte) ([]byte, error) {
		if k != keys[0] {
			return v, nil
		}
		found = true
		return vanillaReplaceSite(v, keys[1:], alias, next)
	})
	if err == nil && !found {
		return nil, fmt.Errorf("missing bound field %s", keys[0])
	}
	return out, err
}

func (o *vanillaOwners) boundEntity(index int, raw []byte, inherit string) ([]byte, bool, error) {
	sites := o.sites[index]
	if b, exists := o.historical[inherit]; exists {
		sites = append(sites, vanillaSite{[]string{"entityDef", "inherit"}, b})
	}
	for _, s := range sites {
		missing, err := o.unavailable(s.binding.family, s.binding.original)
		if err != nil {
			return nil, false, err
		}
		replacement := []byte(pinJSON(s.binding.original))
		if missing {
			replacement = nil
		}
		raw, err = vanillaReplaceSite(raw, s.path, s.binding.alias, replacement)
		if err != nil {
			return nil, false, err
		}
		if missing && len(s.path) == 2 && s.path[1] == "inherit" {
			return nil, true, nil
		}
	}
	return raw, false, nil
}

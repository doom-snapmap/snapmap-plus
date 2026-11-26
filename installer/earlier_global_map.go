package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"sort"
	"strings"
)

// Use only resource slots in placed entity declarations. Keep saved labels, comments and application data byte-for-byte.
func (g earlierGlobalNames) placedReferences(src []byte) ([]byte, error) {
	if len(g.names) == 0 {
		return src, nil
	}
	start := len(src) - len(bytes.TrimPrefix(src, []byte{0xef, 0xbb, 0xbf}))
	view := earlierJSONText(src[start:])
	d := json.NewDecoder(bytes.NewReader(view))
	d.UseNumber()
	type value struct {
		start, end int
		text       string
	}
	changes := map[int]value{}
	record := func(v value, kind string) {
		if kind == "" {
			return
		}
		if f, suffix, ok := g.at(v.text, kind); ok {
			v.text = f.engine + suffix
			changes[v.start] = v
		}
	}
	var visit func([]string, *value) error
	visit = func(path []string, direct *value) error {
		before := int(d.InputOffset())
		tok, err := d.Token()
		if err != nil {
			return err
		}
		switch v := tok.(type) {
		case json.Delim:
			switch v {
			case '{':
				var target, typ, ref value
				for d.More() {
					key, err := d.Token()
					if err != nil {
						return err
					}
					field, ok := key.(string)
					if !ok {
						return fmt.Errorf("invalid saved member")
					}
					var member *value
					switch field {
					case "targetType":
						member = &target
					case "~type":
						member = &typ
					case "value":
						member = &ref
					}
					if err := visit(append(path, field), member); err != nil {
						return err
					}
				}
				if len(path) > 0 && path[0] == "entities" && typ.text == "|pointer" && strings.HasPrefix(target.text, "idDecl") && ref.end > 0 {
					record(ref, strings.ToLower(strings.TrimPrefix(target.text, "idDecl")))
				}
			case '[':
				for d.More() {
					if err := visit(append(path, "[]"), nil); err != nil {
						return err
					}
				}
			}
			_, err := d.Token()
			return err
		case string:
			end := int(d.InputOffset())
			at := bytes.IndexByte(view[before:end], '"')
			if at < 0 {
				return fmt.Errorf("unreadable saved string")
			}
			item := value{start + before + at, start + end, v}
			if direct != nil {
				*direct = item
			}
			if len(path) < 4 || path[0] != "entities" || path[1] != "[]" || path[2] != "entityDef" {
				return nil
			}
			key := path[len(path)-1]
			if len(path) == 4 && key == "inherit" {
				record(item, "entitydef")
				return nil
			}
			if (len(path) > 5 && path[3] == "state" && path[4] == "edit") || (len(path) > 4 && path[3] == "edit") {
				parent := path[len(path)-2]
				if key == "[]" || strings.HasPrefix(key, "item[") {
					key = "item"
				}
				kind := earlierReferenceKind("entitydef", key, parent)
				if kind == "" && g.names[strings.ToLower(v)] != nil {
					switch strings.ToLower(key) {
					case "displayname", "name", "label", "description", "text", "value":
					default:
						return fmt.Errorf("cannot establish the type of the saved %s reference %s", key, v)
					}
				}
				record(item, kind)
			}
		}
		return nil
	}
	if err := visit(nil, nil); err != nil {
		return nil, err
	}
	if len(changes) == 0 {
		return src, nil
	}
	starts := make([]int, 0, len(changes))
	for at := range changes {
		starts = append(starts, at)
	}
	sort.Ints(starts)
	last := 0
	var out bytes.Buffer
	for _, at := range starts {
		v := changes[at]
		out.Write(src[last:v.start])
		out.Write(pinJSON(v.text))
		last = v.end
	}
	out.Write(src[last:])
	return out.Bytes(), nil
}

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
)

// A nil replacement removes a member. Unchanged values, including native comments, retain their bytes.
func vanillaObject(raw []byte, visit func(string, []byte) ([]byte, error)) ([]byte, error) {
	view := earlierJSONText(raw)
	d := json.NewDecoder(bytes.NewReader(view))
	if t, e := d.Token(); e != nil || t != json.Delim('{') {
		return nil, fmt.Errorf("expected a native object")
	}
	out := append([]byte{}, raw[:d.InputOffset()]...)
	seen := map[string]bool{}
	count := 0
	changed := false
	pos := int(d.InputOffset())
	for d.More() {
		key, err := d.Token()
		if err != nil {
			return nil, err
		}
		name := key.(string)
		if seen[name] {
			return nil, fmt.Errorf("duplicate native field %q", name)
		}
		seen[name] = true
		var v json.RawMessage
		if err := d.Decode(&v); err != nil {
			return nil, err
		}
		end := int(d.InputOffset())
		start := end - len(v)
		next, err := visit(name, raw[start:end])
		if err != nil {
			return nil, fmt.Errorf("%s: %w", name, err)
		}
		changed = changed || !bytes.Equal(next, raw[start:end])
		if next != nil {
			head := pos
			for head < start && view[head] != ',' && view[head] != '"' {
				head++
			}
			if view[head] == ',' {
				head++
			} else {
				head = pos
			}
			if count > 0 {
				out = append(out, ',')
			}
			out = append(out, raw[head:start]...)
			out = append(out, next...)
			count++
		}
		pos = end
	}
	if _, err := d.Token(); err != nil {
		return nil, err
	}
	if !changed {
		return raw, nil
	}
	return append(out, raw[pos:]...), nil
}

func vanillaArray(raw []byte, visit func(int, []byte) ([]byte, error)) ([]byte, error) {
	spans, err := earlierVariableSpans(raw)
	if err != nil {
		return nil, err
	}
	changed := false
	kept := make([][]byte, 0, len(spans))
	for i, s := range spans {
		item := raw[s[0]:s[1]]
		next, err := visit(i, item)
		if err != nil {
			return nil, fmt.Errorf("item %d: %w", i, err)
		}
		if next != nil {
			kept = append(kept, next)
		}
		changed = changed || !bytes.Equal(next, item)
	}
	if !changed {
		return raw, nil
	}
	return append(append([]byte{'['}, bytes.Join(kept, []byte{','})...), ']'), nil
}

func vanillaInts(raw []byte) ([]int, error) {
	var n []int
	if err := json.Unmarshal(earlierJSONText(raw), &n); err != nil || n == nil {
		return nil, fmt.Errorf("expected native integer array")
	}
	return n, nil
}

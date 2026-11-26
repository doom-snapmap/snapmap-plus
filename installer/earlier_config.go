package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strconv"
	"unicode/utf8"
)

const maxConfigBytes = 64 * 1024

// encoding/json replaces lone UTF-16 surrogates. The runtime rejects them.
// JSON syntax has already been checked, so every escape is complete.
func configSurrogatesValid(data []byte) bool {
	for {
		i := bytes.IndexByte(data, '\\')
		if i < 0 {
			return true
		}
		data = data[i+1:]
		if data[0] != 'u' {
			data = data[1:]
			continue
		}
		code, _ := strconv.ParseUint(string(data[1:5]), 16, 16)
		data = data[5:]
		if code < 0xd800 || code > 0xdfff {
			continue
		}
		if code >= 0xdc00 || len(data) < 6 || !bytes.HasPrefix(data, []byte(`\u`)) {
			return false
		}
		low, _ := strconv.ParseUint(string(data[2:6]), 16, 16)
		if low < 0xdc00 || low > 0xdfff {
			return false
		}
		data = data[6:]
	}
}

// Keep the original member text so unrelated preferences and extensions survive byte for byte.
type configMember struct {
	name              string
	start, value, end int
}

// Match the runtime's depth limit and reject duplicate keys in nested extensions.
func checkConfigValue(d *json.Decoder, depth int) error {
	token, err := d.Token()
	if err != nil {
		return err
	}
	kind, container := token.(json.Delim)
	if !container {
		return nil
	}
	if depth >= 64 {
		return fmt.Errorf("config nesting exceeds 64 levels")
	}
	var seen map[string]bool
	if kind == '{' {
		seen = make(map[string]bool)
	}
	for d.More() {
		if seen != nil {
			key, err := d.Token()
			name, ok := key.(string)
			if err != nil || !ok || seen[name] {
				return fmt.Errorf("invalid or repeated config member %q", name)
			}
			seen[name] = true
		}
		if err := checkConfigValue(d, depth+1); err != nil {
			return err
		}
	}
	_, err = d.Token()
	return err
}

func configMembers(data []byte) ([]configMember, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	if token, err := dec.Token(); err != nil || token != json.Delim('{') {
		return nil, fmt.Errorf("expected a config object")
	}
	var members []configMember
	start := int(dec.InputOffset())
	for dec.More() {
		key, err := dec.Token()
		name, ok := key.(string)
		if err != nil || !ok {
			return nil, fmt.Errorf("invalid config member %q", name)
		}
		var raw json.RawMessage
		if err := dec.Decode(&raw); err != nil {
			return nil, err
		}
		end := int(dec.InputOffset())
		members = append(members, configMember{name, start, end - len(raw), end})
		start = end
	}
	return members, nil
}

// The flag tells the installer to leave loose overrides in place.
func migratePlayerConfig(input []byte, hasLoose bool) (output []byte, keepLoose bool, err error) {
	data := bytes.TrimPrefix(input, []byte{0xef, 0xbb, 0xbf})
	if len(input) > maxConfigBytes || !utf8.Valid(data) || !json.Valid(data) || !configSurrogatesValid(data) {
		return input, hasLoose, fmt.Errorf("config.json could not be read as a supported preferences file")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	if err := checkConfigValue(decoder, 0); err != nil {
		return input, hasLoose, err
	}
	root, err := configMembers(data)
	if err != nil {
		return input, hasLoose, err
	}
	supported := false
	var prefs configMember
	for _, f := range root {
		switch f.name {
		case "schema_version":
			supported = string(data[f.value:f.end]) == "1"
		case "settings":
			prefs = f
		}
	}
	if !supported || prefs.end == 0 {
		return input, hasLoose, nil
	}
	settings := data[prefs.value:prefs.end]
	fields, err := configMembers(settings)
	if err != nil {
		return input, hasLoose, err
	}

	var clean bytes.Buffer
	clean.WriteByte('{')
	kept, last := 0, 1
	for _, f := range fields {
		last = f.end
		switch f.name {
		case "packages.embed_in_saved_maps", "mods.upload_confirmed":
			continue
		case "overrides.user_enabled":
			value := string(settings[f.value:f.end])
			keepLoose = hasLoose && value != "true" && value != "1"
			if !keepLoose {
				continue
			}
		}
		fragment := settings[f.start:f.end]
		if kept == 0 && f != fields[0] {
			fragment = fragment[bytes.IndexByte(fragment, ',')+1:]
		}
		clean.Write(fragment)
		kept++
	}
	if kept == len(fields) {
		return input, keepLoose, nil
	}
	clean.Write(settings[last:])
	var out bytes.Buffer
	out.Write(input[:len(input)-len(data)+prefs.value])
	out.Write(clean.Bytes())
	out.Write(data[prefs.end:])
	return out.Bytes(), keepLoose, nil
}

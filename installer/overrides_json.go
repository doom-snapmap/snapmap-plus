package main

import (
	"bytes"
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"
)

// Package descriptors and legacy policy files are read with the backend's JSON
// rules from src/backend/config_json.c: exact duplicate keys are invalid at
// every level, strings must be valid UTF-8 without lone surrogates, and object
// or array nesting has a hard depth. encoding/json would silently repair or
// accept inputs that the runtime refuses.

type jsonKind int

const (
	jsonNull jsonKind = iota
	jsonBool
	jsonNumber
	jsonString
	jsonArray
	jsonObject
)

type jsonValue struct {
	kind    jsonKind
	raw     string // literal and number spelling
	text    string // decoded string
	items   []jsonValue
	members []jsonMember
}

type jsonMember struct {
	key   string
	value jsonValue
}

func (v *jsonValue) member(key string) *jsonValue {
	for i := range v.members {
		if v.members[i].key == key {
			return &v.members[i].value
		}
	}
	return nil
}

func (v *jsonValue) remove(key string) bool {
	for i := range v.members {
		if v.members[i].key == key {
			v.members = append(v.members[:i], v.members[i+1:]...)
			return true
		}
	}
	return false
}

type jsonReader struct {
	b        []byte
	p        int
	maxDepth int
}

// Parse one complete value. Depth follows scan_object/scan_array: a container
// at depth d is refused when d >= maxDepth.
func parseNativeJSON(b []byte, maxDepth int) (jsonValue, error) {
	r := jsonReader{b: b, maxDepth: maxDepth}
	r.space()
	v, ok := r.value(0)
	if !ok {
		return jsonValue{}, errors.New("invalid JSON")
	}
	r.space()
	if r.p != len(r.b) {
		return jsonValue{}, errors.New("unexpected text after the JSON value")
	}
	return v, nil
}

func parseNativeObject(b []byte, maxDepth int) (jsonValue, error) {
	v, err := parseNativeJSON(b, maxDepth)
	if err == nil && v.kind != jsonObject {
		err = errors.New("expected a JSON object")
	}
	return v, err
}

func (r *jsonReader) space() {
	for r.p < len(r.b) && (r.b[r.p] == ' ' || r.b[r.p] == '\t' || r.b[r.p] == '\r' || r.b[r.p] == '\n') {
		r.p++
	}
}

func (r *jsonReader) value(depth int) (jsonValue, bool) {
	if r.p >= len(r.b) {
		return jsonValue{}, false
	}
	switch c := r.b[r.p]; {
	case c == 'n':
		return jsonValue{kind: jsonNull, raw: "null"}, r.literal("null")
	case c == 't':
		return jsonValue{kind: jsonBool, raw: "true"}, r.literal("true")
	case c == 'f':
		return jsonValue{kind: jsonBool, raw: "false"}, r.literal("false")
	case c == '"':
		s, ok := r.str()
		return jsonValue{kind: jsonString, text: s}, ok
	case c == '[':
		return r.array(depth)
	case c == '{':
		return r.object(depth)
	case c == '-' || (c >= '0' && c <= '9'):
		start := r.p
		ok := r.number()
		return jsonValue{kind: jsonNumber, raw: string(r.b[start:r.p])}, ok
	}
	return jsonValue{}, false
}

func (r *jsonReader) literal(s string) bool {
	if !bytes.HasPrefix(r.b[r.p:], []byte(s)) {
		return false
	}
	r.p += len(s)
	return true
}

func (r *jsonReader) number() bool {
	start := r.p
	digits := func() bool {
		if r.p >= len(r.b) || r.b[r.p] < '0' || r.b[r.p] > '9' {
			return false
		}
		for r.p < len(r.b) && r.b[r.p] >= '0' && r.b[r.p] <= '9' {
			r.p++
		}
		return true
	}
	if r.b[r.p] == '-' {
		r.p++
	}
	if r.p >= len(r.b) {
		return false
	}
	if r.b[r.p] == '0' {
		r.p++
		if r.p < len(r.b) && r.b[r.p] >= '0' && r.b[r.p] <= '9' {
			return false
		}
	} else if !digits() {
		return false
	}
	if r.p < len(r.b) && r.b[r.p] == '.' {
		r.p++
		if !digits() {
			return false
		}
	}
	if r.p < len(r.b) && (r.b[r.p] == 'e' || r.b[r.p] == 'E') {
		r.p++
		if r.p < len(r.b) && (r.b[r.p] == '+' || r.b[r.p] == '-') {
			r.p++
		}
		if !digits() {
			return false
		}
	}
	return r.p > start
}

func hex4(b []byte) (rune, bool) {
	if len(b) < 4 {
		return 0, false
	}
	var v rune
	for _, c := range b[:4] {
		switch {
		case c >= '0' && c <= '9':
			v = v<<4 | rune(c-'0')
		case c >= 'a' && c <= 'f':
			v = v<<4 | rune(c-'a'+10)
		case c >= 'A' && c <= 'F':
			v = v<<4 | rune(c-'A'+10)
		default:
			return 0, false
		}
	}
	return v, true
}

func (r *jsonReader) str() (string, bool) {
	var out strings.Builder
	r.p++
	for r.p < len(r.b) {
		c := r.b[r.p]
		switch {
		case c == '"':
			r.p++
			return out.String(), true
		case c < 0x20:
			return "", false
		case c == '\\':
			if r.p+1 >= len(r.b) {
				return "", false
			}
			e := r.b[r.p+1]
			r.p += 2
			switch e {
			case '"', '\\', '/':
				out.WriteByte(e)
			case 'b':
				out.WriteByte('\b')
			case 'f':
				out.WriteByte('\f')
			case 'n':
				out.WriteByte('\n')
			case 'r':
				out.WriteByte('\r')
			case 't':
				out.WriteByte('\t')
			case 'u':
				high, ok := hex4(r.b[r.p:])
				if !ok {
					return "", false
				}
				r.p += 4
				switch {
				case high >= 0xD800 && high <= 0xDBFF:
					if len(r.b)-r.p < 6 || r.b[r.p] != '\\' || r.b[r.p+1] != 'u' {
						return "", false
					}
					low, ok := hex4(r.b[r.p+2:])
					if !ok || low < 0xDC00 || low > 0xDFFF {
						return "", false
					}
					r.p += 6
					out.WriteRune(0x10000 + (high-0xD800)<<10 + (low - 0xDC00))
				case high >= 0xDC00 && high <= 0xDFFF:
					return "", false
				default:
					out.WriteRune(high)
				}
			default:
				return "", false
			}
		case c < 0x80:
			out.WriteByte(c)
			r.p++
		default:
			n := nativeUTF8Length(r.b[r.p:])
			if n == 0 {
				return "", false
			}
			out.Write(r.b[r.p : r.p+n])
			r.p += n
		}
	}
	return "", false
}

// valid_utf8_at: shortest forms only, no UTF-16 surrogates, at most U+10FFFF.
func nativeUTF8Length(b []byte) int {
	r, n := utf8.DecodeRune(b)
	if r == utf8.RuneError && n <= 1 {
		return 0
	}
	return n
}

func (r *jsonReader) array(depth int) (jsonValue, bool) {
	v := jsonValue{kind: jsonArray}
	if depth >= r.maxDepth {
		return v, false
	}
	r.p++
	r.space()
	if r.p < len(r.b) && r.b[r.p] == ']' {
		r.p++
		return v, true
	}
	for {
		item, ok := r.value(depth + 1)
		if !ok {
			return v, false
		}
		v.items = append(v.items, item)
		r.space()
		if r.p >= len(r.b) {
			return v, false
		}
		if r.b[r.p] == ']' {
			r.p++
			return v, true
		}
		if r.b[r.p] != ',' {
			return v, false
		}
		r.p++
		r.space()
	}
}

func (r *jsonReader) object(depth int) (jsonValue, bool) {
	v := jsonValue{kind: jsonObject}
	if depth >= r.maxDepth {
		return v, false
	}
	r.p++
	r.space()
	if r.p < len(r.b) && r.b[r.p] == '}' {
		r.p++
		return v, true
	}
	seen := map[string]bool{}
	for {
		if r.p >= len(r.b) || r.b[r.p] != '"' {
			return v, false
		}
		key, ok := r.str()
		if !ok || seen[key] {
			return v, false
		}
		seen[key] = true
		r.space()
		if r.p >= len(r.b) || r.b[r.p] != ':' {
			return v, false
		}
		r.p++
		r.space()
		item, ok := r.value(depth + 1)
		if !ok {
			return v, false
		}
		v.members = append(v.members, jsonMember{key, item})
		r.space()
		if r.p >= len(r.b) {
			return v, false
		}
		if r.b[r.p] == '}' {
			r.p++
			return v, true
		}
		if r.b[r.p] != ',' {
			return v, false
		}
		r.p++
		r.space()
	}
}

// Pretty output keeps decoded text and number spellings; only whitespace and
// the escaping of already valid strings can differ from an authored file.
func formatNativeJSON(v jsonValue) []byte {
	var b bytes.Buffer
	writeNativeJSON(&b, v, 0)
	b.WriteByte('\n')
	return b.Bytes()
}

func writeNativeJSON(b *bytes.Buffer, v jsonValue, indent int) {
	pad := func(n int) {
		b.WriteByte('\n')
		for i := 0; i < n; i++ {
			b.WriteString("  ")
		}
	}
	switch v.kind {
	case jsonString:
		writeJSONString(b, v.text)
	case jsonArray:
		if len(v.items) == 0 {
			b.WriteString("[]")
			return
		}
		b.WriteByte('[')
		for i, item := range v.items {
			if i > 0 {
				b.WriteByte(',')
			}
			pad(indent + 1)
			writeNativeJSON(b, item, indent+1)
		}
		pad(indent)
		b.WriteByte(']')
	case jsonObject:
		if len(v.members) == 0 {
			b.WriteString("{}")
			return
		}
		b.WriteByte('{')
		for i, m := range v.members {
			if i > 0 {
				b.WriteByte(',')
			}
			pad(indent + 1)
			writeJSONString(b, m.key)
			b.WriteString(": ")
			writeNativeJSON(b, m.value, indent+1)
		}
		pad(indent)
		b.WriteByte('}')
	default:
		b.WriteString(v.raw)
	}
}

func writeJSONString(b *bytes.Buffer, s string) {
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		default:
			if c < 0x20 {
				fmt.Fprintf(b, `\u%04x`, c)
			} else {
				b.WriteByte(c)
			}
		}
	}
	b.WriteByte('"')
}

func jsonText(s string) jsonValue { return jsonValue{kind: jsonString, text: s} }

// A leading UTF-8 byte-order mark is common in files saved by Windows editors.
func withoutBOM(b []byte) ([]byte, bool) {
	if bytes.HasPrefix(b, []byte{0xEF, 0xBB, 0xBF}) {
		return b[3:], true
	}
	return b, false
}

const (
	packageIDCapacity   = 128
	packageNameCapacity = 256
	descriptorDepth     = 24
)

// sh_package_id_valid in package_descriptor.c.
func packageIDValid(id string) bool {
	if id == "" || len(id) >= packageIDCapacity {
		return false
	}
	for i := 0; i < len(id); i++ {
		c := id[i]
		if (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') {
			continue
		}
		if i == 0 || i+1 == len(id) || (c != '.' && c != '_' && c != '-') {
			return false
		}
		if c == '.' && id[i-1] == '.' {
			return false
		}
	}
	return true
}

func asciiLower(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + 'a' - 'A'
		}
	}
	return string(b)
}

func localeValid(locale string) bool {
	if locale == "" || len(locale) >= 48 {
		return false
	}
	for i := 0; i < len(locale); i++ {
		c := locale[i]
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') {
			continue
		}
		if c != '-' || i == 0 || i+1 == len(locale) || locale[i-1] == '-' {
			return false
		}
	}
	return true
}

// Keys compare with ASCII case folding like pd_key_equal; empty keys and
// keys containing NUL are refused like pd_unique_keys.
func uniqueNativeKeys(members []jsonMember) error {
	seen := map[string]string{}
	for _, m := range members {
		if m.key == "" || strings.ContainsRune(m.key, 0) {
			return fmt.Errorf("empty or invalid key")
		}
		folded := asciiLower(m.key)
		if other, ok := seen[folded]; ok {
			return fmt.Errorf("keys %q and %q differ only by letter case", other, m.key)
		}
		seen[folded] = m.key
	}
	return nil
}

func validateStringsSection(v *jsonValue) error {
	if v.kind != jsonObject {
		return errors.New("strings must map locales to text")
	}
	if err := uniqueNativeKeys(v.members); err != nil {
		return fmt.Errorf("strings locales: %v", err)
	}
	for _, locale := range v.members {
		if !localeValid(locale.key) {
			return fmt.Errorf("invalid string locale %q", locale.key)
		}
		if locale.value.kind != jsonObject {
			return fmt.Errorf("strings.%s must be an object", locale.key)
		}
		if err := uniqueNativeKeys(locale.value.members); err != nil {
			return fmt.Errorf("strings.%s: %v", locale.key, err)
		}
		for _, s := range locale.value.members {
			if s.value.kind != jsonString || strings.ContainsRune(s.value.text, 0) {
				return fmt.Errorf("strings.%s.%s must be text", locale.key, s.key)
			}
		}
	}
	return nil
}

var supportedCvars = map[string]bool{"g_useResourceBlackList": true, "g_useImageBlackList": true}

// pc_policies accepts exactly {"cvars": {...}} with audited cvars set to 0.
func validateRequirementsSection(v *jsonValue) error {
	if v.kind != jsonObject {
		return errors.New("requirements must be an object")
	}
	if len(v.members) == 0 {
		return nil
	}
	cvars := v.member("cvars")
	if len(v.members) != 1 || cvars == nil || cvars.kind != jsonObject {
		return errors.New("requirements contain an unsupported setting")
	}
	for _, m := range cvars.members {
		if !supportedCvars[m.key] || m.value.kind != jsonNumber || m.value.raw != "0" {
			return fmt.Errorf("unsupported cvar requirement %s", m.key)
		}
		if m.value.items != nil || m.value.members != nil {
			return fmt.Errorf("unsupported cvar requirement %s", m.key)
		}
	}
	return nil
}

// hud_name_valid in weapon_hud.c.
func hudWeaponNameValid(name string) bool {
	if name == "" || len(name) >= 192 || name[0] == '/' || name[len(name)-1] == '/' {
		return false
	}
	for _, part := range strings.Split(name, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
		for i := 0; i < len(part); i++ {
			c := part[i]
			if !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
				return false
			}
		}
	}
	return true
}

func validateHudWeapons(weapons *jsonValue) error {
	if weapons.kind != jsonObject {
		return errors.New("hud weapons must be an object")
	}
	if len(weapons.members) > 256 {
		return errors.New("hud declares more weapon rules than the runtime supports")
	}
	for _, w := range weapons.members {
		if !hudWeaponNameValid(w.key) || w.value.kind != jsonObject || len(w.value.members) != 1 {
			return fmt.Errorf("invalid hud rule for weapon %q", w.key)
		}
		mode := w.value.member("ammo_display")
		if mode == nil || mode.kind != jsonString || (mode.text != "weapon" && mode.text != "engine") {
			return fmt.Errorf("hud weapon %q needs ammo_display weapon or engine", w.key)
		}
	}
	return nil
}

// hud_parse accepts an empty policy or exactly {"weapons": {...}}.
func validateHudSection(v *jsonValue) error {
	if v.kind != jsonObject {
		return errors.New("hud must be an object")
	}
	if len(v.members) == 0 {
		return nil
	}
	weapons := v.member("weapons")
	if len(v.members) != 1 || weapons == nil {
		return errors.New("hud supports only weapons")
	}
	if len(formatNativeJSON(*v)) > 64*1024 {
		return errors.New("hud policy exceeds the runtime size limit")
	}
	return validateHudWeapons(weapons)
}

// validateDescriptor applies package_descriptor.c and the compiler's policy
// checks to the exact bytes that will be written.
func validateDescriptor(b []byte) error {
	v, err := parseNativeObject(b, descriptorDepth)
	if err != nil {
		return fmt.Errorf("package.json must contain one valid JSON object: %v", err)
	}
	id := v.member("id")
	if id == nil || id.kind != jsonString || strings.ContainsRune(id.text, 0) || !packageIDValid(id.text) {
		return errors.New("package id must be a stable lowercase identifier")
	}
	name := v.member("name")
	if name == nil || name.kind != jsonString || name.text == "" || strings.ContainsRune(name.text, 0) ||
		len(name.text) >= packageNameCapacity {
		return errors.New("package name must be nonempty text shorter than 256 bytes")
	}
	if s := v.member("requirements"); s != nil {
		if err := validateRequirementsSection(s); err != nil {
			return err
		}
	}
	if s := v.member("hud"); s != nil {
		if err := validateHudSection(s); err != nil {
			return err
		}
	}
	if s := v.member("strings"); s != nil {
		if err := validateStringsSection(s); err != nil {
			return err
		}
	}
	// Compiled sections are reparsed with depth 16 in pc_policies.
	for _, key := range []string{"requirements", "strings", "hud"} {
		if s := v.member(key); s != nil {
			if _, err := parseNativeJSON(formatNativeJSON(*s), 16); err != nil {
				return fmt.Errorf("%s nesting exceeds the compiler limit", key)
			}
		}
	}
	return nil
}

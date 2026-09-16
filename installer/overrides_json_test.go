package main

import (
	"strings"
	"testing"
)

// These cases mirror src/backend/config_json.c and package_descriptor.c so a
// converted descriptor is accepted exactly when the runtime accepts it.
func TestDescriptorValidationMatchesRuntime(t *testing.T) {
	name255 := strings.Repeat("n", 255)
	deep := strings.Repeat(`{"a":`, 23) + "1" + strings.Repeat("}", 23)
	tooDeep := strings.Repeat(`{"a":`, 24) + "1" + strings.Repeat("}", 24)
	for _, tc := range []struct {
		name, json string
		valid      bool
	}{
		{"minimal", `{"id":"local.pkg","name":"Package"}`, true},
		{"unknown metadata", `{"id":"a","name":"A","author":{"x":[1,2.5e3,null,true]}}`, true},
		{"name at the byte limit", `{"id":"a","name":"` + name255 + `"}`, true},
		{"name over the byte limit", `{"id":"a","name":"` + name255 + `n"}`, false},
		{"multibyte name over the limit", `{"id":"a","name":"` + strings.Repeat("é", 128) + `"}`, false},
		{"empty name", `{"id":"a","name":""}`, false},
		{"NUL in name", `{"id":"a","name":"a\u0000b"}`, false},
		{"missing id", `{"name":"A"}`, false},
		{"uppercase id", `{"id":"Local.pkg","name":"A"}`, false},
		{"double dot id", `{"id":"local..pkg","name":"A"}`, false},
		{"id ending in punctuation", `{"id":"local.pkg-","name":"A"}`, false},
		{"id at the limit", `{"id":"` + strings.Repeat("a", 127) + `","name":"A"}`, true},
		{"id over the limit", `{"id":"` + strings.Repeat("a", 128) + `","name":"A"}`, false},
		{"exact duplicate key", `{"id":"a","name":"A","name":"B"}`, false},
		{"duplicate nested key", `{"id":"a","name":"A","x":{"k":1,"k":2}}`, false},
		{"keys differing by case", `{"id":"a","name":"A","Name":"B"}`, true},
		{"byte order mark", "\xef\xbb\xbf{\"id\":\"a\",\"name\":\"A\"}", false},
		{"trailing comma", `{"id":"a","name":"A",}`, false},
		{"trailing text", `{"id":"a","name":"A"} x`, false},
		{"invalid UTF-8", "{\"id\":\"a\",\"name\":\"\xff\"}", false},
		{"lone surrogate", `{"id":"a","name":"\ud800"}`, false},
		{"surrogate pair", `{"id":"a","name":"\ud83d\ude00"}`, true},
		{"control character", "{\"id\":\"a\",\"name\":\"a\tb\"}", false},
		{"leading zero", `{"id":"a","name":"A","n":01}`, false},
		{"nesting at the limit", `{"id":"a","name":"A","x":` + deep + `}`, true},
		{"nesting over the limit", `{"id":"a","name":"A","x":` + tooDeep + `}`, false},
		{"requirements", `{"id":"a","name":"A","requirements":{"cvars":{"g_useImageBlackList":0,"g_useResourceBlackList":0}}}`, true},
		{"requirement value spelling", `{"id":"a","name":"A","requirements":{"cvars":{"g_useImageBlackList":0.0}}}`, false},
		{"unsupported cvar", `{"id":"a","name":"A","requirements":{"cvars":{"developer":0}}}`, false},
		{"unsupported requirement", `{"id":"a","name":"A","requirements":{"maps":{}}}`, false},
		{"empty requirements", `{"id":"a","name":"A","requirements":{}}`, true},
		{"strings", `{"id":"a","name":"A","strings":{"en":{"k":"v"},"pt-BR":{"k":"v"}}}`, true},
		{"strings key case duplicate", `{"id":"a","name":"A","strings":{"en":{"k":"v","K":"w"}}}`, false},
		{"strings locale case duplicate", `{"id":"a","name":"A","strings":{"en":{},"EN":{}}}`, false},
		{"invalid locale", `{"id":"a","name":"A","strings":{"en_US":{}}}`, false},
		{"non-text string", `{"id":"a","name":"A","strings":{"en":{"k":1}}}`, false},
		{"empty string id", `{"id":"a","name":"A","strings":{"en":{"":"v"}}}`, false},
		{"hud", `{"id":"a","name":"A","hud":{"weapons":{"weapon/zion/player/sp/pistol":{"ammo_display":"engine"}}}}`, true},
		{"empty hud", `{"id":"a","name":"A","hud":{}}`, true},
		{"hud extra section", `{"id":"a","name":"A","hud":{"weapons":{},"other":{}}}`, false},
		{"hud invalid weapon name", `{"id":"a","name":"A","hud":{"weapons":{"Weapon":{"ammo_display":"engine"}}}}`, false},
		{"hud invalid mode", `{"id":"a","name":"A","hud":{"weapons":{"weapon":{"ammo_display":"none"}}}}`, false},
		{"hud extra setting", `{"id":"a","name":"A","hud":{"weapons":{"weapon":{"ammo_display":"engine","x":1}}}}`, false},
		{"requirements not object", `{"id":"a","name":"A","requirements":[]}`, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			err := validateDescriptor([]byte(tc.json))
			if (err == nil) != tc.valid {
				t.Fatalf("valid=%v, got error %v", tc.valid, err)
			}
		})
	}
}

func TestNativeJSONRoundTripKeepsValues(t *testing.T) {
	source := `{"id":"a","name":"Caf\u00e9 \"quoted\" \\ tab\t","n":-1.5e+10,"list":[true,false,null],"o":{}}`
	source = strings.Replace(source, "\t", `\t`, 1)
	v, err := parseNativeObject([]byte(source), descriptorDepth)
	if err != nil {
		t.Fatal(err)
	}
	formatted := formatNativeJSON(v)
	again, err := parseNativeObject(formatted, descriptorDepth)
	if err != nil {
		t.Fatalf("formatted output does not parse: %v\n%s", err, formatted)
	}
	if again.member("name").text != "Café \"quoted\" \\ tab\t" || again.member("n").raw != "-1.5e+10" ||
		len(again.member("list").items) != 3 || again.member("o").kind != jsonObject {
		t.Fatalf("values changed:\n%s", formatted)
	}
}

func TestLegacyPolicyReadersMatchHistoricalRules(t *testing.T) {
	if cvars, err := parseLegacyRequirements([]byte("# c\r\n\r\ncvar\tg_useImageBlackList\t0\n"), "r"); err != nil || len(cvars) != 1 {
		t.Fatalf("valid requirements refused: %v", err)
	}
	for _, bad := range []string{" cvar\tg_useImageBlackList\t0", "cvar\tg_useImageBlackList\t1", "cvar g_useImageBlackList 0",
		"cvar\tg_useImageBlackList\t0\textra", "cvar\tdeveloper\t0", "cvar\tg_useImageBlackList\t0\x7f", "    "} {
		if _, err := parseLegacyRequirements([]byte(bad), "r"); err == nil {
			t.Errorf("requirement accepted: %q", bad)
		}
	}
	rows, err := parseLegacyManifest([]byte("# comment\r\ntype\tname/x\t\r\ntype\tname\tpath/file.ext\n"), "m")
	if err != nil || len(rows) != 2 || rows[0].provider != "name/x" {
		t.Fatalf("valid manifest refused: %v %+v", err, rows)
	}
	for _, bad := range []string{"type\tname", "ty/pe\tname\tpath", "type\t\tpath", "type\tname\t../path", "type\tna me\tpath",
		"type\tname\tpath\\file", "type\tname\tc:path", "type\tname\t/path"} {
		if _, err := parseLegacyManifest([]byte(bad), "m"); err == nil {
			t.Errorf("manifest row accepted: %q", bad)
		}
	}
	if members, err := parseLegacyStrings([]byte("\xef\xbb\xbf{\"a\":\"x\",\"b\":\"\\u00e9\"}"), "s"); err != nil || len(members) != 2 || members[1].value.text != "é" {
		t.Fatalf("valid strings refused: %v", err)
	}
	for _, bad := range []string{`{"a":1}`, `{"a":{"b":"c"}}`, `{"":"x"}`, `["a"]`, `{"a":"x"`, `{"a":"x\u0000"}`} {
		if _, err := parseLegacyStrings([]byte(bad), "s"); err == nil {
			t.Errorf("strings accepted: %q", bad)
		}
	}
	if _, err := parseLegacyHud([]byte(`{"schema":"snapmap-plus.weapon-hud.v1","weapons":{"w":{"ammo_display":"weapon"}}}`), "h"); err != nil {
		t.Fatalf("valid HUD refused: %v", err)
	}
	for _, bad := range []string{`{"weapons":{}}`, `{"schema":"snapmap-plus.weapon-hud.v2","weapons":{}}`,
		`{"schema":"snapmap-plus.weapon-hud.v1","weapons":{},"x":1}`, `{"schema":"snapmap-plus.weapon-hud.v1","weapons":{"w":{"ammo_display":"all"}}}`} {
		if _, err := parseLegacyHud([]byte(bad), "h"); err == nil {
			t.Errorf("HUD accepted: %q", bad)
		}
	}
	for path, want := range map[string]bool{"entitydef/a.decl": true, "entitydef/sub/A-b_c.1.DECL": true, "a.decl": false,
		"entitydef/.decl": false, "entitydef/../x.decl": false, "entity def/x.decl": false, "entitydef/x y.decl": false,
		"entitydef/x.decl.backup": false, strings.Repeat("t", 64) + "/x.decl": false, strings.Repeat("t", 63) + "/x.decl": true} {
		if legacyDeclIdentity(path) != want {
			t.Errorf("declaration identity %q = %v", path, !want)
		}
	}
	for name, want := range map[string]bool{"file.decl": true, "aux.decl": false, "COM1": false, "com10": true, "lpt0": true,
		"trailing.": false, "space ": false, "wild*": false, "Ünicode": true} {
		if engineNameValid(name) != want {
			t.Errorf("engine name %q = %v", name, !want)
		}
	}
}

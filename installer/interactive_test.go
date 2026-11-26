package main

import (
	"io"
	"strings"
	"testing"
	"testing/iotest"
)

func TestConfirmationRequiresFreshY(t *testing.T) {
	old := confirmationInput
	t.Cleanup(func() { confirmationInput = old })
	for _, tc := range []struct {
		input string
		want  bool
	}{
		{"y\n", true}, {"Y\r\n", true}, {"n\n", false}, {"\r\n", false},
		{"maybe\nY\n", false}, {"yes\n", false}, {"y", false}, {"", false},
	} {
		confirmationInput = func() io.ReadCloser { return io.NopCloser(strings.NewReader(tc.input)) }
		if got := confirm(false, "Continue?"); got != tc.want {
			t.Fatalf("answer %q: %v", tc.input, got)
		}
	}
	confirmationInput = func() io.ReadCloser { return io.NopCloser(iotest.ErrReader(io.ErrClosedPipe)) }
	if confirm(false, "Continue?") {
		t.Fatal("accepted a failed keyboard read")
	}
	confirmationInput = func() io.ReadCloser { t.Fatal("--yes opened the keyboard"); return nil }
	if !confirm(true, "Continue?") {
		t.Fatal("--yes did not confirm")
	}
}

// TestEnterDefault covers what a bare Enter does at the interactive prompt.
func TestEnterDefault(t *testing.T) {
	cases := []struct {
		name             string
		installed        bool
		installedVersion string
		latestTag        string
		want             string
	}{
		{"not installed -> install", false, "", "", "install"},
		{"not installed ignores latest", false, "", "v0.3.0", "install"},
		{"installed, newer available -> update", true, "v0.2.0-beta.4", "v0.2.0-beta.5", "update"},
		{"installed, up to date -> nothing", true, "v0.2.0-beta.5", "v0.2.0-beta.5", ""},
		{"installed, check failed -> nothing (no false notice)", true, "v0.2.0-beta.5", "", ""},
		{"installed, latest is older -> nothing (never a downgrade)", true, "v1.1.0-beta.1", "v1.0.0", ""},
		{"installed beta, stable is newer -> update", true, "v1.0.0-beta.26", "v1.0.0", "update"},
	}
	for _, c := range cases {
		if got := enterDefault(c.installed, c.installedVersion, c.latestTag); got != c.want {
			t.Errorf("%s: enterDefault(%v,%q,%q)=%q want %q", c.name, c.installed, c.installedVersion, c.latestTag, got, c.want)
		}
	}
}

// TestSplitArgs covers the interactive tokenizer: quoted paths with spaces stay one token, quotes
// never leak into tokens.
func TestSplitArgs(t *testing.T) {
	cases := []struct {
		in   string
		want []string
	}{
		{"", nil},
		{"   ", nil},
		{"update", []string{"update"}},
		{"install --beta --yes", []string{"install", "--beta", "--yes"}},
		{`install --doom "C:\Program Files (x86)\Steam\steamapps\common\DOOM"`,
			[]string{"install", "--doom", `C:\Program Files (x86)\Steam\steamapps\common\DOOM`}},
		{`install --doom "C:\DOOM"`, []string{"install", "--doom", `C:\DOOM`}},
		{`--release v0.2.0-beta.4`, []string{"--release", "v0.2.0-beta.4"}},
		{`say "unterminated quote`, []string{"say", "unterminated quote"}},
	}
	for _, c := range cases {
		got := splitArgs(c.in)
		if len(got) != len(c.want) {
			t.Errorf("splitArgs(%q)=%v want %v", c.in, got, c.want)
			continue
		}
		for i := range got {
			if got[i] != c.want[i] {
				t.Errorf("splitArgs(%q)[%d]=%q want %q", c.in, i, got[i], c.want[i])
			}
		}
	}
}

// A 2018 install whose record names another release is offered the restoring update.
func TestLegacyNeedsUpdate(t *testing.T) {
	old := doomBuild{build: build2018}
	cur := doomBuild{build: build2024}
	for _, c := range []struct {
		b    doomBuild
		v    string
		want bool
	}{
		{old, "v0.2.1-beta.20", true},
		{old, legacyReleaseTag, false},
		{cur, "v0.2.1-beta.19", false},
	} {
		if got := legacyNeedsUpdate(c.b, c.v); got != c.want {
			t.Errorf("legacyNeedsUpdate(%s, %s) = %v", c.b.build, c.v, got)
		}
	}
}

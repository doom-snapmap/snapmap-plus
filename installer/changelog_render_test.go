package main

import (
	"os"
	"strings"
	"testing"
)

// entryFixture is the SHARED golden fixture: installer/testdata/entry.md is produced by the Python
// renderer (tools/draft_changelog.py) and asserted byte-for-byte by its test suite. Both sides read
// this one file, so a format change fails the Go and the Python tests together instead of silently
// rotting one of two hand-written parsers of the same grammar.
func entryFixture(t *testing.T) string {
	t.Helper()
	b, err := os.ReadFile("testdata/entry.md")
	if err != nil {
		t.Fatalf("reading the shared fixture: %v", err)
	}
	return string(b)
}

func TestParseEntryReadsTheSharedFixture(t *testing.T) {
	e := parseEntry(entryFixture(t))

	if e.Headline != "Mod packs install and play in one step" {
		t.Errorf("headline = %q", e.Headline)
	}
	if !strings.HasPrefix(e.Summary, "A map that needs mod packs") {
		t.Errorf("summary = %q", e.Summary)
	}
	if !strings.HasSuffix(e.Summary, "restart DOOM.") {
		t.Errorf("summary truncated: %q", e.Summary)
	}
	if e.Collapsed != "Plus 15 smaller fixes and internal changes." {
		t.Errorf("collapsed = %q", e.Collapsed)
	}

	want := []struct {
		title string
		items int
	}{{"New", 2}, {"Improved", 2}, {"Fixed", 1}}
	if len(e.Groups) != len(want) {
		t.Fatalf("got %d groups, want %d: %+v", len(e.Groups), len(want), e.Groups)
	}
	for i, w := range want {
		if e.Groups[i].Title != w.title {
			t.Errorf("group %d title = %q, want %q", i, e.Groups[i].Title, w.title)
		}
		if len(e.Groups[i].Items) != w.items {
			t.Errorf("group %q has %d items, want %d", w.title, len(e.Groups[i].Items), w.items)
		}
	}
	if got := e.Groups[0].Items[0]; got != "Install every mod pack a map needs from a single prompt." {
		t.Errorf("first bullet = %q", got)
	}
}

func TestParseEntryKeepsMarkupOutOfTheText(t *testing.T) {
	e := parseEntry(entryFixture(t))
	for _, s := range append([]string{e.Headline, e.Summary, e.Collapsed},
		e.Groups[0].Items...) {
		for _, marker := range []string{"**", "###", "_Plus", "- "} {
			if strings.HasPrefix(s, marker) {
				t.Errorf("markup %q survived parsing into %q", marker, s)
			}
		}
	}
}

func TestParseEntryJoinsAHandWrappedBullet(t *testing.T) {
	// CHANGELOG.md is hand-editable, so a maintainer may wrap a long bullet.
	body := "**H**\n\nS.\n\n### New\n- A bullet that runs on\n  and continues here.\n"
	e := parseEntry(body)
	if got := e.Groups[0].Items[0]; got != "A bullet that runs on and continues here." {
		t.Errorf("continuation dropped: %q", got)
	}
}

func TestParseEntryHandlesASkeletonEntry(t *testing.T) {
	// What the drafter emits when the API call fails; the CLI must still render it.
	body := "**NEEDS WRITING**\n\nAutomatic drafting did not run.\n\n### New\n- Some commit subject\n"
	e := parseEntry(body)
	if e.Headline != "NEEDS WRITING" || len(e.Groups) != 1 {
		t.Errorf("skeleton parsed wrong: %+v", e)
	}
}

func TestParseEntryOnUnstructuredLegacyNotesKeepsTheText(t *testing.T) {
	// Releases published before CHANGELOG.md existed have plain bullet lists and no
	// headline. Their text must survive rather than vanish.
	body := "Changes since v0.1.0:\n\n- did a thing\n- did another thing\n"
	e := parseEntry(body)
	if e.Headline != "" {
		t.Errorf("invented a headline: %q", e.Headline)
	}
	if !strings.Contains(e.Summary, "Changes since") {
		t.Errorf("summary lost the intro: %q", e.Summary)
	}
	if len(e.Loose) != 2 {
		t.Fatalf("got %d loose bullets, want 2: %+v", len(e.Loose), e.Loose)
	}
	if e.Loose[0] != "did a thing" {
		t.Errorf("loose bullet = %q", e.Loose[0])
	}
}

func TestWrapHangingIndent(t *testing.T) {
	got := wrap("one two three four five six", 16, "  ", "      ")
	want := "  one two three\n      four five\n      six"
	if got != want {
		t.Errorf("wrap =\n%q\nwant\n%q", got, want)
	}
}

func TestWrapNeverSplitsAWordThatExceedsTheWidth(t *testing.T) {
	got := wrap("short https://example.invalid/a/very/long/url/that/exceeds", 20, "", "")
	if strings.Contains(got, "https://exampl\n") {
		t.Errorf("split a long token:\n%s", got)
	}
}

func TestRenderEntryLooksLikeTheDesign(t *testing.T) {
	e := parseEntry(entryFixture(t))
	var b strings.Builder
	renderEntry(&b, releaseHeader{Tag: "v0.2.1-beta.6", Beta: true, Date: "2026-09-03",
		Installed: true, URL: "https://example.invalid/r"}, e, 72)
	out := b.String()

	for _, want := range []string{
		"v0.2.1-beta.6",
		"beta",
		"<- installed",
		"3 September 2026",
		"Mod packs install and play in one step",
		"New",
		"- Install every mod pack a map needs from a single prompt.",
		"Plus 15 smaller fixes and internal changes.",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	for _, line := range strings.Split(out, "\n") {
		if len(line) > 72 {
			t.Errorf("line exceeds the width (%d): %q", len(line), line)
		}
	}
	if strings.Contains(out, "\x1b[") {
		t.Error("emitted an ANSI escape; this renderer is deliberately colourless")
	}
}

func TestRenderIndexIsOneLinePerRelease(t *testing.T) {
	var b strings.Builder
	renderIndex(&b, []indexRow{
		{Tag: "v0.2.1-beta.5", Date: "2026-08-26", Headline: "Packages carry their own images and text"},
		{Tag: "v0.2.1-beta.4", Date: "2026-08-21", Headline: "Override packages and a real asset browser"},
	}, 78)
	out := b.String()

	if !strings.Contains(out, "v0.2.1-beta.5") || !strings.Contains(out, "26 Aug 2026") {
		t.Errorf("index row malformed:\n%s", out)
	}
	if !strings.Contains(out, "snapmap-plus changelog <version>") {
		t.Errorf("missing the how-to-read-one hint:\n%s", out)
	}
	for _, tag := range []string{"v0.2.1-beta.5", "v0.2.1-beta.4"} {
		if n := strings.Count(out, tag); n != 1 {
			t.Errorf("%s appears %d times, want 1", tag, n)
		}
	}
}

func TestSelectRelease(t *testing.T) {
	list := []ghRelease{
		{TagName: "v0.3.0-beta.1", Prerelease: true},
		{TagName: "v0.2.1", Prerelease: false},
		{TagName: "v0.2.1-beta.6", Prerelease: true},
	}
	cases := []struct {
		arg, want string
	}{
		{"v0.2.1-beta.6", "v0.2.1-beta.6"},
		{"0.2.1-beta.6", "v0.2.1-beta.6"}, // a user will type it without the v
		{"V0.2.1-BETA.6", "v0.2.1-beta.6"},
		{"v0.2.1", "v0.2.1"},  // must NOT resolve to v0.2.1-beta.6
		{"latest", "v0.2.1"},  // newest STABLE, not newest overall
		{"", "v0.3.0-beta.1"}, // no argument: newest of any kind
	}
	for _, c := range cases {
		got, err := selectRelease(list, c.arg)
		if err != nil {
			t.Errorf("selectRelease(%q): %v", c.arg, err)
			continue
		}
		if got.TagName != c.want {
			t.Errorf("selectRelease(%q) = %s, want %s", c.arg, got.TagName, c.want)
		}
	}
}

func TestSelectReleaseUnknownVersionListsWhatExists(t *testing.T) {
	list := []ghRelease{{TagName: "v0.2.1-beta.6"}, {TagName: "v0.2.1-beta.5"}}
	_, err := selectRelease(list, "v9.9.9")
	if err == nil {
		t.Fatal("expected an error for an unknown version")
	}
	if !strings.Contains(err.Error(), "v0.2.1-beta.6") {
		t.Errorf("error should name the versions that exist: %v", err)
	}
}

func TestPrettyDate(t *testing.T) {
	for in, want := range map[string]string{
		"2026-09-03T00:00:00Z": "3 September 2026",
		"2026-08-26":           "26 August 2026",
		"":                     "",
		"not-a-date":           "not-a-date",
	} {
		if got := prettyDate(in); got != want {
			t.Errorf("prettyDate(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestShortDate(t *testing.T) {
	if got := shortDate("2026-08-26T00:00:00Z"); got != "26 Aug 2026" {
		t.Errorf("shortDate = %q", got)
	}
}

func TestTerminalWidthIsClampedToSomethingReadable(t *testing.T) {
	t.Setenv("COLUMNS", "500")
	if w := terminalWidth(); w > 100 {
		t.Errorf("width %d is too wide to read comfortably", w)
	}
	t.Setenv("COLUMNS", "12")
	if w := terminalWidth(); w < 40 {
		t.Errorf("width %d is too narrow to render", w)
	}
	t.Setenv("COLUMNS", "")
	if w := terminalWidth(); w != 80 {
		t.Errorf("default width = %d, want 80", w)
	}
}

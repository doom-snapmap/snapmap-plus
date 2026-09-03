package main

import (
	"strings"
	"testing"
)

// The notes a release carries are its reviewed CHANGELOG.md entry (see release.yml). These tests
// drive formatChangelog through the same shapes the GitHub API returns.

func entryBody(headline string) string {
	return "**" + headline + "**\n\nA short summary of the release.\n\n" +
		"### New\n- Something a player can see.\n\n" +
		"_Plus 3 smaller fixes and internal changes._"
}

func sampleReleases() []ghRelease {
	return []ghRelease{
		{TagName: "v0.2.1-beta.6", Prerelease: true, PublishedAt: "2026-09-03T00:00:00Z",
			Body:    entryBody("Mod packs install and play in one step"),
			HTMLURL: "https://github.com/doom-snapmap/snapmap-plus/releases/tag/v0.2.1-beta.6"},
		{TagName: "v0.2.1-beta.5", Prerelease: true, PublishedAt: "2026-08-26T00:00:00Z",
			Body:    entryBody("Packages carry their own images and text"),
			HTMLURL: "https://github.com/doom-snapmap/snapmap-plus/releases/tag/v0.2.1-beta.5"},
		{TagName: "v0.2.1-beta.4", Prerelease: true, PublishedAt: "2026-08-21T00:00:00Z",
			Body:    entryBody("Override packages and a real asset browser"),
			HTMLURL: "https://github.com/doom-snapmap/snapmap-plus/releases/tag/v0.2.1-beta.4"},
	}
}

// TestFormatChangelogDefaultShowsNewestInFullThenAnIndex: the default view is the newest release
// complete, then one line per earlier release. Regression guard for the closed-beta report where
// the console showed only links, and for the opposite failure -- dumping every entry in full.
func TestFormatChangelogDefaultShowsNewestInFullThenAnIndex(t *testing.T) {
	out := formatChangelog(sampleReleases(), "", "", 78)

	for _, want := range []string{
		"v0.2.1-beta.6",
		"Mod packs install and play in one step",
		"A short summary of the release.",
		"- Something a player can see.",
		"Plus 3 smaller fixes and internal changes.",
		"Earlier releases",
		"Packages carry their own images and text",
		"Override packages and a real asset browser",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	// Only the newest is expanded: the others contribute a headline, not a body.
	if n := strings.Count(out, "A short summary of the release."); n != 1 {
		t.Errorf("expanded %d entries, want 1:\n%s", n, out)
	}
	if n := strings.Count(out, "- Something a player can see."); n != 1 {
		t.Errorf("printed %d bullet lists, want 1:\n%s", n, out)
	}
}

func TestFormatChangelogMarksTheInstalledRelease(t *testing.T) {
	out := formatChangelog(sampleReleases(), "v0.2.1-beta.6", "", 78)
	if !strings.Contains(out, "<- installed") {
		t.Errorf("missing the installed marker:\n%s", out)
	}
}

// TestFormatChangelogMarksTheInstalledReleaseInTheIndex: someone on the stable channel is behind
// every beta, so the release they have is usually an older one listed in the index, not the newest.
func TestFormatChangelogMarksTheInstalledReleaseInTheIndex(t *testing.T) {
	out := formatChangelog(sampleReleases(), "v0.2.1-beta.4", "", 78)
	for _, line := range strings.Split(out, "\n") {
		if strings.Contains(line, "v0.2.1-beta.4") && strings.Contains(line, "21 Aug 2026") {
			if !strings.Contains(line, "<- installed") {
				t.Errorf("index row for the installed release is unmarked: %q", line)
			}
			return
		}
	}
	t.Errorf("no index row for the installed release:\n%s", out)
}

// TestFormatChangelogSpecificVersion: `snapmap-plus changelog v0.2.1-beta.4` shows that release and
// nothing else -- no index, no other release's text.
func TestFormatChangelogSpecificVersion(t *testing.T) {
	out := formatChangelog(sampleReleases(), "", "v0.2.1-beta.4", 78)

	if !strings.Contains(out, "Override packages and a real asset browser") {
		t.Errorf("missing the requested release:\n%s", out)
	}
	for _, unwanted := range []string{"Earlier releases", "Mod packs install", "beta.5"} {
		if strings.Contains(out, unwanted) {
			t.Errorf("unexpected %q when one version was requested:\n%s", unwanted, out)
		}
	}
	if !strings.Contains(out, "releases/tag/v0.2.1-beta.4") {
		t.Errorf("missing the release link:\n%s", out)
	}
}

func TestFormatChangelogAcceptsAVersionWithoutTheLeadingV(t *testing.T) {
	out := formatChangelog(sampleReleases(), "", "0.2.1-beta.4", 78)
	if !strings.Contains(out, "Override packages and a real asset browser") {
		t.Errorf("a version typed without the leading v should resolve:\n%s", out)
	}
}

func TestFormatChangelogAllExpandsEveryRelease(t *testing.T) {
	out := formatChangelog(sampleReleases(), "", "all", 78)
	if n := strings.Count(out, "A short summary of the release."); n != 3 {
		t.Errorf("expanded %d entries, want 3:\n%s", n, out)
	}
	if strings.Contains(out, "Earlier releases") {
		t.Errorf("--all should not also print the index:\n%s", out)
	}
}

func TestFormatChangelogUnknownVersionIsAHelpfulMessage(t *testing.T) {
	out := formatChangelog(sampleReleases(), "", "v9.9.9", 78)
	if !strings.Contains(out, "v9.9.9") || !strings.Contains(out, "v0.2.1-beta.6") {
		t.Errorf("should name the missing version and what exists:\n%s", out)
	}
}

// TestFormatChangelogRendersPreCHANGELOGNotes: releases published before CHANGELOG.md existed have
// an intro line and a flat bullet list. Their text must still render rather than vanish.
func TestFormatChangelogRendersPreCHANGELOGNotes(t *testing.T) {
	list := []ghRelease{{
		TagName: "v0.1.0-beta.3", Prerelease: true, PublishedAt: "2026-06-29T00:00:00Z",
		Body:    "Changes since v0.1.0-beta.2:\n\n- did a thing\n- did another thing",
		HTMLURL: "https://example.invalid/r",
	}}
	out := formatChangelog(list, "", "", 78)
	for _, want := range []string{"Changes since", "- did a thing", "- did another thing"} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
}

func TestFormatChangelogEmptyNotesSaysSo(t *testing.T) {
	list := []ghRelease{{TagName: "v0.1.0", PublishedAt: "2026-06-29T00:00:00Z", Body: ""}}
	out := formatChangelog(list, "", "", 78)
	if !strings.Contains(out, "no notes were published") {
		t.Errorf("an empty body should say so:\n%s", out)
	}
}

func TestFormatChangelogWrapsToTheGivenWidth(t *testing.T) {
	list := []ghRelease{{
		TagName: "v0.2.2", PublishedAt: "2026-09-10T00:00:00Z",
		Body: "**A headline**\n\n" + strings.Repeat("word ", 60) + "\n",
	}}
	for _, width := range []int{60, 78, 100} {
		for _, line := range strings.Split(formatChangelog(list, "", "", width), "\n") {
			if len(line) > width {
				t.Errorf("width %d: line is %d chars: %q", width, len(line), line)
			}
		}
	}
}

func TestChangelogArgPrefersThePositionalAndAcceptsRelease(t *testing.T) {
	if got := changelogArg(flags{}, []string{"v0.2.1-beta.4"}); got != "v0.2.1-beta.4" {
		t.Errorf("positional = %q", got)
	}
	if got := changelogArg(flags{release: "v0.2.1"}, nil); got != "v0.2.1" {
		t.Errorf("--release = %q", got)
	}
	if got := changelogArg(flags{}, []string{"--beta"}); got != "" {
		t.Errorf("a flag must not be read as a version: %q", got)
	}
	if got := changelogArg(flags{}, nil); got != "" {
		t.Errorf("no argument = %q", got)
	}
}

package main

import "testing"

// TestNewestRelease covers the channel choice by version, whatever order the list is in: the
// default takes the newest stable release (the newest beta while none is stable); --beta takes the
// newest release of either kind, so a stable release newer than every beta wins there too.
func TestNewestRelease(t *testing.T) {
	stable := func(tag string) ghRelease { return ghRelease{TagName: tag} }
	beta := func(tag string) ghRelease { return ghRelease{TagName: tag, Prerelease: true} }
	draft := func(tag string) ghRelease { return ghRelease{TagName: tag, Draft: true} }

	cases := []struct {
		name    string
		list    []ghRelease
		beta    bool
		wantTag string // "" = expect nil
	}{
		{"empty list", nil, false, ""},
		{"only betas -> newest beta", []ghRelease{beta("v0.2.0-beta.9"), beta("v0.2.0-beta.10")}, false, "v0.2.0-beta.10"},
		{"newer beta above a stable -> still the stable", []ghRelease{beta("v0.4.0-beta.1"), stable("v0.3.0")}, false, "v0.3.0"},
		{"stables out of order -> the highest", []ghRelease{stable("v0.3.0"), stable("v0.3.1"), stable("v0.2.9")}, false, "v0.3.1"},
		{"--beta after 1.0 -> the stable, not an older beta", []ghRelease{beta("v1.0.0-beta.26"), stable("v1.0.0")}, true, "v1.0.0"},
		{"--beta with a newer beta -> the beta", []ghRelease{stable("v1.0.0"), beta("v1.1.0-beta.1")}, true, "v1.1.0-beta.1"},
		{"drafts and other tags are skipped", []ghRelease{draft("v9.0.0"), stable("nightly"), stable("v1.0.0")}, true, "v1.0.0"},
	}
	for _, c := range cases {
		got := newestRelease(c.list, c.beta)
		if got == nil && c.wantTag == "" {
			continue
		}
		if got == nil || got.TagName != c.wantTag {
			t.Errorf("%s: got %v, want %q", c.name, got, c.wantTag)
		}
	}
}

// The changelog and the channel choice read releases newest version first.
func TestSortReleasesByVersion(t *testing.T) {
	list := []ghRelease{{TagName: "v1.0.0-beta.9"}, {TagName: "other"}, {TagName: "v1.0.0"}, {TagName: "v1.0.0-beta.10"}}
	sortReleases(list)
	want := []string{"v1.0.0", "v1.0.0-beta.10", "v1.0.0-beta.9", "other"}
	for i, r := range list {
		if r.TagName != want[i] {
			t.Fatalf("sorted = %v, want %v", list, want)
		}
	}
}

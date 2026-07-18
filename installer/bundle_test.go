package main

import "testing"

// TestPickDefaultRelease covers the no-flags channel choice: newest stable wins; with only
// pre-releases published (the beta era) the newest beta is used and reported as the fallback.
func TestPickDefaultRelease(t *testing.T) {
	stable := func(tag string) ghRelease { return ghRelease{TagName: tag} }
	beta := func(tag string) ghRelease { return ghRelease{TagName: tag, Prerelease: true} }

	cases := []struct {
		name     string
		list     []ghRelease // newest-first, like the GitHub API
		wantTag  string      // "" = expect nil
		wantBeta bool
	}{
		{"empty list", nil, "", false},
		{"only betas -> newest beta, flagged", []ghRelease{beta("v0.2.0-beta.2"), beta("v0.2.0-beta.1")}, "v0.2.0-beta.2", true},
		{"stable newest -> stable", []ghRelease{stable("v0.3.0"), beta("v0.2.0-beta.2")}, "v0.3.0", false},
		{"newer beta above a stable -> still the stable", []ghRelease{beta("v0.4.0-beta.1"), stable("v0.3.0")}, "v0.3.0", false},
		{"only stables -> newest stable", []ghRelease{stable("v0.3.1"), stable("v0.3.0")}, "v0.3.1", false},
	}
	for _, c := range cases {
		got, isBeta := pickDefaultRelease(c.list)
		if c.wantTag == "" {
			if got != nil {
				t.Errorf("%s: got %q, want nil", c.name, got.TagName)
			}
			continue
		}
		if got == nil || got.TagName != c.wantTag || isBeta != c.wantBeta {
			gotTag := "<nil>"
			if got != nil {
				gotTag = got.TagName
			}
			t.Errorf("%s: got (%s, beta=%v), want (%s, beta=%v)", c.name, gotTag, isBeta, c.wantTag, c.wantBeta)
		}
	}
}

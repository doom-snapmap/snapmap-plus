package main

import (
	"archive/zip"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// repoSlug is the GitHub "owner/repo" the installer downloads releases from.
const repoSlug = "doom-snapmap/snapmap-plus"

// releaseAsset is the stable asset name CI publishes on every release (so latest/download works).
const releaseAsset = "snapmap-plus-bundle.zip"

// bundle is a ready-to-deploy overlay tree (a dist/ dir) plus its MANIFEST.sha256 file list.
type bundle struct {
	root    string
	files   []manifestEntry
	version string // the resolved release tag (e.g. v0.1.0-beta.1), or "local"
}

type manifestEntry struct {
	rel    string // overlay-relative path, e.g. "snapmap-plus\snapmap-plus-ui.dll"
	sha256 string
}

// acquireBundle returns a verified bundle (from --local, else a downloaded release) plus a cleanup func.
func acquireBundle(f flags) (*bundle, func(), error) {
	noop := func() {}
	if f.local != "" {
		b, err := loadBundle(f.local)
		if b != nil {
			b.version = "local"
		}
		return b, noop, err
	}
	dir, tag, cleanup, err := downloadRelease(f)
	if err != nil {
		return nil, noop, err
	}
	b, err := loadBundle(dir)
	if err != nil {
		cleanup()
		return nil, noop, err
	}
	b.version = tag
	return b, cleanup, nil
}

// loadBundle reads dist/MANIFEST.sha256 and verifies every listed file is present and hash-correct,
// so we never start writing into a DOOM install from a partial or tampered bundle.
func loadBundle(root string) (*bundle, error) {
	f, err := os.Open(filepath.Join(root, "MANIFEST.sha256"))
	if err != nil {
		return nil, fmt.Errorf("%q isn't a Snapmap+ build folder (no MANIFEST.sha256) -- point --local at a dist/ folder built by package.ps1", root)
	}
	defer f.Close()

	var entries []manifestEntry
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return nil, fmt.Errorf("the build's MANIFEST is malformed (%q) -- re-download or rebuild with package.ps1", line)
		}
		entries = append(entries, manifestEntry{
			rel:    filepath.FromSlash(fields[1]),
			sha256: strings.ToLower(fields[0]),
		})
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("%q has an empty MANIFEST -- the build looks incomplete", root)
	}
	for _, e := range entries {
		got, err := fileSHA256(filepath.Join(root, e.rel))
		if err != nil {
			return nil, fmt.Errorf("the build is incomplete -- %s is missing. Rebuild with package.ps1, or re-download", e.rel)
		}
		if got != e.sha256 {
			return nil, fmt.Errorf("integrity check failed on %s (hash mismatch) -- the download may be corrupted or tampered. Try again", e.rel)
		}
	}
	return &bundle{root: root, files: entries}, nil
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// --- GitHub release resolution ------------------------------------------------------------------------
// install/update download the release bundle from GitHub via the API. A PRIVATE repo (closed beta) needs a
// token -- set once with `snapmap-plus set-token <tok>`, or via SNAPMAP_PLUS_TOKEN / --token; it also enables --beta.
// When the repo is public, no token is needed.

type ghRelease struct {
	TagName     string    `json:"tag_name"`
	Name        string    `json:"name"`
	Draft       bool      `json:"draft"`
	Prerelease  bool      `json:"prerelease"`
	PublishedAt string    `json:"published_at"`
	Body        string    `json:"body"`
	HTMLURL     string    `json:"html_url"`
	Assets      []ghAsset `json:"assets"`
}

type ghAsset struct {
	Name               string `json:"name"`
	URL                string `json:"url"`                  // API URL (token-authed private download)
	BrowserDownloadURL string `json:"browser_download_url"` // public URL
	Digest             string `json:"digest"`               // "sha256:<hex>" (may be empty)
}

// downloadRelease resolves the requested release (stable / --beta pre-release / --release tag), downloads
// its snapmap-plus-bundle.zip asset (with the token if set), and extracts it into a temp dir.
func downloadRelease(f flags) (dir string, tag string, cleanup func(), err error) {
	noop := func() {}
	token := resolveToken(f)

	rel, err := fetchRelease(f, token)
	if err != nil {
		return "", "", noop, err
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == releaseAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return "", "", noop, fmt.Errorf("release %s is missing its download -- it may still be publishing, so try again in a minute", rel.TagName)
	}

	tmp, err := os.MkdirTemp("", "snapmap-plus-bundle-")
	if err != nil {
		return "", "", noop, err
	}
	cleanup = func() { os.RemoveAll(tmp) }
	zipPath := filepath.Join(tmp, releaseAsset)
	if err := downloadAsset(asset, token, zipPath); err != nil {
		cleanup()
		return "", "", noop, err
	}
	if err := unzip(zipPath, tmp); err != nil {
		cleanup()
		return "", "", noop, err
	}
	return tmp, rel.TagName, cleanup, nil
}

// fetchRelease picks: an explicit --release tag, the latest --beta pre-release, or the latest stable
// (falling back to the newest beta while no stable has been published yet).
func fetchRelease(f flags, token string) (*ghRelease, error) {
	base := "https://api.github.com/repos/" + repoSlug + "/releases"
	switch {
	case f.release != "":
		var r ghRelease
		return &r, apiGet(base+"/tags/"+f.release, token, &r)
	case f.beta:
		var list []ghRelease
		if err := apiGet(base+"?per_page=30", token, &list); err != nil {
			return nil, err
		}
		for i := range list {
			if list[i].Prerelease {
				return &list[i], nil
			}
		}
		return nil, fmt.Errorf("no beta build is available yet")
	default:
		var r ghRelease
		err := apiGet(base+"/latest", token, &r)
		if err == nil {
			return &r, nil
		}
		// GitHub's "latest" endpoint excludes pre-releases, so before the first stable release it
		// 404s even though betas exist. Fall back to the release list: prefer a stable if one shows
		// up after all (a transient /latest failure), else take the newest beta and say so. Once a
		// stable release exists this fallback never picks a beta again.
		var list []ghRelease
		if lerr := apiGet(base+"?per_page=30", token, &list); lerr == nil {
			if pick, isBeta := pickDefaultRelease(list); pick != nil {
				if isBeta {
					fmt.Printf("No stable release has been published yet -- using the newest beta (%s).\n", pick.TagName)
				}
				return pick, nil
			}
		}
		return nil, err
	}
}

// pickDefaultRelease picks what a no-flags install/update should get from the release list
// (newest-first, the GitHub API order): the newest stable release, or -- while no stable has been
// published yet -- the newest pre-release. The bool reports that beta fallback so the caller can tell
// the user. Returns nil when the list is empty. Pure (no I/O) so it is unit-testable.
func pickDefaultRelease(list []ghRelease) (*ghRelease, bool) {
	for i := range list {
		if !list[i].Prerelease {
			return &list[i], false
		}
	}
	for i := range list {
		if list[i].Prerelease {
			return &list[i], true
		}
	}
	return nil, false
}

func apiGet(url, token string, out interface{}) error {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snapmap-plus-installer")
	req.Header.Set("Accept", "application/vnd.github+json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("couldn't reach GitHub (%v) -- check your internet connection and try again", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%s", friendlyHTTP(resp.StatusCode, token))
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

// friendlyHTTP turns a GitHub API status code into a plain-English, actionable message.
func friendlyHTTP(code int, token string) string {
	switch code {
	case http.StatusUnauthorized: // 401
		return "your access token was rejected (401) -- double-check it with whoever gave it to you, then run: snapmap-plus set-token <token>"
	case http.StatusForbidden: // 403
		return "GitHub denied or rate-limited the request (403) -- wait a few minutes and try again"
	case http.StatusNotFound: // 404
		if token == "" {
			return "couldn't find the release (404) -- if no stable version has been released yet, run:  snapmap-plus update --beta"
		}
		return "release not found (404) -- check the version, or your token may not have access to this repository"
	default:
		return fmt.Sprintf("GitHub returned an error (HTTP %d) -- try again in a moment", code)
	}
}

// downloadAsset fetches the asset: with a token, the API asset URL + octet-stream (the only way to pull a
// private-repo asset); without, the public browser_download_url.
func downloadAsset(a *ghAsset, token, dest string) error {
	url, accept := a.BrowserDownloadURL, ""
	if token != "" {
		url, accept = a.URL, "application/octet-stream"
	}
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snapmap-plus-installer")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("couldn't download %s (%v) -- check your internet connection and try again", a.Name, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("couldn't download %s (HTTP %d) -- try again in a moment", a.Name, resp.StatusCode)
	}
	out, err := os.Create(dest)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, resp.Body)
	return err
}

// --- token storage ------------------------------------------------------------------------------------

func tokenPath() (string, error) {
	dir := appDataDir()
	if dir == "" {
		return "", fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
	return filepath.Join(dir, "token"), nil
}

// resolveToken: --token flag > SNAPMAP_PLUS_TOKEN env > the saved token file (new location, then the pre-rename one).
func resolveToken(f flags) string {
	if f.token != "" {
		return f.token
	}
	if t := os.Getenv("SNAPMAP_PLUS_TOKEN"); t != "" {
		return t
	}
	if p, err := tokenPath(); err == nil {
		if b, err := os.ReadFile(p); err == nil {
			return strings.TrimSpace(string(b))
		}
	}
	// Pre-rename installs saved the token under %LOCALAPPDATA%\open-snaphak\.
	if op := oldTokenPath(); op != "" {
		if b, err := os.ReadFile(op); err == nil {
			return strings.TrimSpace(string(b))
		}
	}
	return ""
}

// cmdSetToken saves a GitHub token so install/update can pull from a private repo (closed beta).
func cmdSetToken(args []string) error {
	if len(args) < 1 || strings.TrimSpace(args[0]) == "" {
		return fmt.Errorf("usage: snapmap-plus set-token <github-token>")
	}
	selfInstall() // a tester's first command -> keep a stable copy of snapmap-plus.exe
	p, err := tokenPath()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return fmt.Errorf("couldn't create the Snapmap+ settings folder (%v)", err)
	}
	if err := os.WriteFile(p, []byte(strings.TrimSpace(args[0])), 0o600); err != nil {
		return fmt.Errorf("couldn't save your token (%v)", err)
	}
	fmt.Println("Token saved. You can now run:  snapmap-plus update --beta")
	return nil
}

// cmdChangelog prints the release history for a terminal. With no argument it shows the newest
// release in full and then a one-line index of the earlier ones; with a version it shows just that
// release. Notes come from each release's body, which is its reviewed CHANGELOG.md entry (see
// release.yml), so the CLI, the website and the GitHub releases page all say the same thing.
//
// It reads them from the API rather than from a shipped file so it can show a release NEWER than
// the installed one -- which is most of the reason to run the command.
func cmdChangelog(f flags, args []string) error {
	token := resolveToken(f)
	var list []ghRelease
	if err := apiGet("https://api.github.com/repos/"+repoSlug+"/releases?per_page=30", token, &list); err != nil {
		return err
	}
	// Drafts are not published releases; nobody can install one.
	published := list[:0]
	for _, r := range list {
		if !r.Draft {
			published = append(published, r)
		}
	}
	list = published
	if len(list) == 0 {
		fmt.Println("No releases have been published yet.")
		return nil
	}

	installed := ""
	if rec, err := loadRecord(); err == nil {
		installed = rec.Version
	}
	fmt.Print(formatChangelog(list, installed, changelogArg(f, args), terminalWidth()))
	return nil
}

// changelogArg picks the version the user asked for: a bare positional ("changelog v0.2.1-beta.4"),
// or --release, which already means "this specific version" on install.
func changelogArg(f flags, args []string) string {
	if f.release != "" {
		return f.release
	}
	for _, a := range args {
		if !strings.HasPrefix(a, "-") {
			return a
		}
	}
	return ""
}

// formatChangelog renders the history. Pure (no I/O) so it is unit-testable. `list` is newest-first
// (the GitHub API order), `installed` is the currently-installed tag, `want` is the requested
// version ("" for the default view, "all" for every entry in full).
func formatChangelog(list []ghRelease, installed, want string, width int) string {
	var b strings.Builder

	if strings.EqualFold(strings.TrimSpace(want), "all") {
		for i, r := range list {
			if i > 0 {
				b.WriteString("\n")
			}
			renderEntry(&b, headerFor(r, installed), parseEntry(r.Body), width)
		}
		return b.String()
	}

	chosen, err := selectRelease(list, want)
	if err != nil {
		return wrap(err.Error(), width, margin, margin) + "\n"
	}
	renderEntry(&b, headerFor(chosen, installed), parseEntry(chosen.Body), width)
	fmt.Fprintf(&b, "  %s\n", chosen.HTMLURL)

	// A specific version was asked for: show that one and stop.
	if strings.TrimSpace(want) != "" {
		return b.String()
	}

	var rows []indexRow
	for _, r := range list {
		if r.TagName == chosen.TagName {
			continue
		}
		rows = append(rows, indexRow{Tag: r.TagName, Date: r.PublishedAt,
			Headline: parseEntry(r.Body).Headline, Installed: r.TagName == installed})
	}
	if len(rows) > 0 {
		b.WriteString("\n")
		renderIndex(&b, rows, width)
	}
	return b.String()
}

func headerFor(r ghRelease, installed string) releaseHeader {
	return releaseHeader{
		Tag:       r.TagName,
		Beta:      r.Prerelease,
		Date:      r.PublishedAt,
		Installed: r.TagName == installed,
		URL:       r.HTMLURL,
	}
}

// releaseHeadline formats a one-line "tag (beta)   date   <- you have this" summary for a release.
func releaseHeadline(r ghRelease, installed string) string {
	kind := ""
	if r.Prerelease {
		kind = " (beta)"
	}
	date := r.PublishedAt
	if len(date) >= 10 {
		date = date[:10]
	}
	you := ""
	if r.TagName == installed {
		you = "   <- you have this"
	}
	return fmt.Sprintf("%s%s   %s%s", r.TagName, kind, date, you)
}

// unzip extracts src into dest, guarding against zip-slip path traversal.
func unzip(src, dest string) error {
	r, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer r.Close()
	prefix := filepath.Clean(dest) + string(os.PathSeparator)
	for _, zf := range r.File {
		target := filepath.Join(dest, filepath.FromSlash(zf.Name))
		if !strings.HasPrefix(target, prefix) {
			return fmt.Errorf("unsafe zip entry: %s", zf.Name)
		}
		if zf.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		rc, err := zf.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(target)
		if err != nil {
			rc.Close()
			return err
		}
		_, copyErr := io.Copy(out, rc)
		out.Close()
		rc.Close()
		if copyErr != nil {
			return copyErr
		}
	}
	return nil
}

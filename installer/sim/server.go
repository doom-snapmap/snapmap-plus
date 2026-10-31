package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// The fake Releases API serves every subfolder of --root as a release whose tag is the folder name
// and whose assets are the files in it, newest first, with the size and "sha256:" digest GitHub
// publishes. The folder is read on every request, so the harness publishes a newer release by adding
// a folder. A tag with a hyphen is a prerelease.

const repoSlug = "doom-snapmap/snapmap-plus"

type ghAsset struct {
	Name               string `json:"name"`
	URL                string `json:"url"`
	BrowserDownloadURL string `json:"browser_download_url"`
	Digest             string `json:"digest"`
	Size               int64  `json:"size"`
}

type ghRelease struct {
	TagName     string    `json:"tag_name"`
	Name        string    `json:"name"`
	Prerelease  bool      `json:"prerelease"`
	PublishedAt string    `json:"published_at"`
	Body        string    `json:"body"`
	HTMLURL     string    `json:"html_url"`
	Assets      []ghAsset `json:"assets"`
}

type releaseServer struct {
	root, base string
}

func fileSHA256(path string) (string, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil)), n, err
}

// releases lists the releases under root, newest first.
func (s *releaseServer) releases() []ghRelease {
	entries, _ := os.ReadDir(s.root)
	var tags []string
	for _, e := range entries {
		if e.IsDir() {
			tags = append(tags, e.Name())
		}
	}
	sort.Slice(tags, func(i, j int) bool { return compareVersions(tags[i], tags[j]) > 0 })
	var out []ghRelease
	for _, tag := range tags {
		r := ghRelease{TagName: tag, Name: tag, Prerelease: strings.Contains(tag, "-"),
			PublishedAt: "2026-10-31T00:00:00Z", HTMLURL: s.base + "/sim/" + tag,
			Body: "## " + tag + "\n\nA simulated release built from dist/ by installer/sim/run.ps1.\n"}
		files, _ := os.ReadDir(filepath.Join(s.root, tag))
		for _, f := range files {
			if f.IsDir() {
				continue
			}
			sum, size, err := fileSHA256(filepath.Join(s.root, tag, f.Name()))
			if err != nil {
				continue
			}
			u := s.base + "/dl/" + tag + "/" + f.Name()
			r.Assets = append(r.Assets, ghAsset{Name: f.Name(), URL: u, BrowserDownloadURL: u, Digest: "sha256:" + sum, Size: size})
		}
		out = append(out, r)
	}
	return out
}

func (s *releaseServer) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	base := "/repos/" + repoSlug + "/releases"
	path := req.URL.Path
	fmt.Fprintf(os.Stderr, "sim-server: %s %s\n", req.Method, path)
	list := s.releases()
	send := func(v any) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(v)
	}
	switch {
	case path == base:
		if list == nil {
			list = []ghRelease{}
		}
		send(list)
	case path == base+"/latest":
		for _, r := range list {
			if !r.Prerelease {
				send(r)
				return
			}
		}
		http.NotFound(w, req)
	case strings.HasPrefix(path, base+"/tags/"):
		for _, r := range list {
			if r.TagName == strings.TrimPrefix(path, base+"/tags/") {
				send(r)
				return
			}
		}
		http.NotFound(w, req)
	case strings.HasPrefix(path, "/dl/"):
		parts := strings.SplitN(strings.TrimPrefix(path, "/dl/"), "/", 2)
		if len(parts) != 2 || strings.ContainsAny(parts[0]+parts[1], `/\`) || strings.Contains(parts[0]+parts[1], "..") {
			http.NotFound(w, req)
			return
		}
		http.ServeFile(w, req, filepath.Join(s.root, parts[0], parts[1]))
	default:
		http.NotFound(w, req)
	}
}

// cmdServe listens on a free loopback port, writes its base URL to --url-file and serves until killed.
func cmdServe(args []string) error {
	o, _ := parseOpts(args)
	if err := o.need("root", "url-file"); err != nil {
		return err
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	s := &releaseServer{root: o.one("root"), base: "http://" + ln.Addr().String()}
	if err := os.WriteFile(o.one("url-file"), []byte(s.base), 0o644); err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "sim-server: serving %s at %s\n", s.root, s.base)
	return http.Serve(ln, s)
}

// bundleFiles are what release.yml zips from dist/.
var bundleFiles = []string{"XINPUT1_4.dll", "snapmap-plus", "MANIFEST.sha256", "snapmap-plus-store.exe", "TOOLS.sha256"}

// cmdRelease packs dist/ into <out>\snapmap-plus-bundle.zip as release.yml does and copies the
// installer beside it as snapmap-plus.exe.
func cmdRelease(args []string) error {
	o, _ := parseOpts(args)
	if err := o.need("dist", "exe", "out"); err != nil {
		return err
	}
	dist, out := o.one("dist"), o.one("out")
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	zf, err := os.Create(filepath.Join(out, "snapmap-plus-bundle.zip"))
	if err != nil {
		return err
	}
	zw := zip.NewWriter(zf)
	for _, top := range bundleFiles {
		err := filepath.Walk(filepath.Join(dist, top), func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if info.IsDir() {
				return nil
			}
			rel, _ := filepath.Rel(dist, path)
			w, err := zw.Create(filepath.ToSlash(rel))
			if err != nil {
				return err
			}
			f, err := os.Open(path)
			if err != nil {
				return err
			}
			defer f.Close()
			_, err = io.Copy(w, f)
			return err
		})
		if err != nil {
			zw.Close()
			zf.Close()
			return fmt.Errorf("packing %s: %v", top, err)
		}
	}
	if err := zw.Close(); err != nil {
		return err
	}
	if err := zf.Close(); err != nil {
		return err
	}
	return copyFile(o.one("exe"), filepath.Join(out, "snapmap-plus.exe"))
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

var releaseVersionRE = regexp.MustCompile(`^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$`)

// compareVersions orders release tags by semantic-version precedence; anything else sorts by name.
func compareVersions(a, b string) int {
	ma, mb := releaseVersionRE.FindStringSubmatch(a), releaseVersionRE.FindStringSubmatch(b)
	if ma == nil || mb == nil {
		return strings.Compare(a, b)
	}
	for i := 1; i <= 3; i++ {
		x, _ := strconv.Atoi(ma[i])
		y, _ := strconv.Atoi(mb[i])
		if x != y {
			return x - y
		}
	}
	switch {
	case ma[4] == mb[4]:
		return 0
	case ma[4] == "":
		return 1
	case mb[4] == "":
		return -1
	}
	ia, ib := strings.Split(ma[4], "."), strings.Split(mb[4], ".")
	for i := 0; i < len(ia) && i < len(ib); i++ {
		x, ex := strconv.Atoi(ia[i])
		y, ey := strconv.Atoi(ib[i])
		if ex == nil && ey == nil {
			if x != y {
				return x - y
			}
			continue
		}
		if c := strings.Compare(ia[i], ib[i]); c != 0 {
			return c
		}
	}
	return len(ia) - len(ib)
}

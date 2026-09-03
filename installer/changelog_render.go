package main

// changelog_render.go -- renders a release's notes for a terminal.
//
// A release body is the reviewed CHANGELOG.md entry that release.yml publishes (see
// docs/contributing.md). It is markdown because the website and GitHub render it; a console does
// not, so this file parses that entry back into its parts and lays them out as plain text: wrapped
// to the terminal, bullets with a hanging indent, one release per block.
//
// DELIBERATELY COLOURLESS. No ANSI escapes: the tool targets Windows, where a plain conhost window
// without virtual-terminal processing would print raw escape codes instead of colour, and the
// structure below reads well without it. A test asserts no escape is emitted.
//
// THE GRAMMAR LIVES IN TWO PLACES. tools/changelog.py parses the same entry shape in Python. That
// duplication is guarded by a shared golden fixture, installer/testdata/entry.md, which the Python
// renderer produces and both test suites read -- change the format and both fail together. Do not
// "simplify" either side away from the fixture.

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// changelogEntry is one release's notes, parsed out of its markdown body.
type changelogEntry struct {
	Headline  string           // the "**one line**" a release is about
	Summary   string           // the paragraph under it, already unwrapped
	Groups    []changelogGroup // New / Improved / Fixed, in the order they appear
	Collapsed string           // the "_Plus N smaller fixes_" line, without its underscores
	Loose     []string         // bullets that appeared under no group (pre-CHANGELOG.md releases)
}

type changelogGroup struct {
	Title string
	Items []string
}

// releaseHeader is what the CLI knows about a release from the GitHub API, as opposed to from its
// notes: the tag, the channel, when it was published, and whether it is the installed one.
type releaseHeader struct {
	Tag       string
	Beta      bool
	Date      string
	Installed bool
	URL       string
}

// indexRow is one line of the "earlier releases" list.
type indexRow struct {
	Tag       string
	Date      string
	Headline  string
	Installed bool
}

// parseEntry turns a release body back into its parts. It is tolerant on purpose: releases
// published before CHANGELOG.md existed are a plain bullet list with no headline and no groups,
// and their text must survive rather than disappear.
func parseEntry(body string) changelogEntry {
	var e changelogEntry
	var summary []string
	group := -1

	for _, raw := range strings.Split(body, "\n") {
		line := strings.TrimRight(raw, " \t\r")
		trimmed := strings.TrimSpace(line)
		if trimmed == "" {
			continue
		}

		switch {
		case strings.HasPrefix(trimmed, "### "):
			e.Groups = append(e.Groups, changelogGroup{
				Title: strings.TrimSpace(strings.TrimPrefix(trimmed, "###")),
			})
			group = len(e.Groups) - 1

		case strings.HasPrefix(trimmed, "- "):
			item := strings.TrimSpace(strings.TrimPrefix(trimmed, "- "))
			if group >= 0 {
				e.Groups[group].Items = append(e.Groups[group].Items, item)
			} else {
				e.Loose = append(e.Loose, item)
			}

		case e.Headline == "" && len(trimmed) > 4 &&
			strings.HasPrefix(trimmed, "**") && strings.HasSuffix(trimmed, "**"):
			e.Headline = strings.TrimSpace(trimmed[2 : len(trimmed)-2])

		case len(trimmed) > 2 && strings.HasPrefix(trimmed, "_") && strings.HasSuffix(trimmed, "_"):
			e.Collapsed = strings.TrimSpace(trimmed[1 : len(trimmed)-1])

		// An indented line continues the bullet above it: CHANGELOG.md is hand-editable and a
		// maintainer may wrap a long one. Dropping the tail of a sentence is worse than any
		// formatting rule.
		case (raw != "" && (raw[0] == ' ' || raw[0] == '\t')) && group >= 0 &&
			len(e.Groups[group].Items) > 0:
			last := len(e.Groups[group].Items) - 1
			e.Groups[group].Items[last] += " " + trimmed

		case group < 0 && len(e.Loose) == 0:
			summary = append(summary, trimmed)
		}
	}

	e.Summary = strings.Join(summary, " ")
	return e
}

// wrap lays text out inside width columns, prefixing the first line with indent and every
// continuation line with hanging. A word longer than the available width (a URL, say) is left
// whole and allowed to overhang rather than being cut in half.
func wrap(text string, width int, indent, hanging string) string {
	words := strings.Fields(text)
	if len(words) == 0 {
		return ""
	}
	var b strings.Builder
	line := indent + words[0]
	for _, w := range words[1:] {
		if len(line)+1+len(w) > width {
			b.WriteString(line)
			b.WriteString("\n")
			line = hanging + w
			continue
		}
		line += " " + w
	}
	b.WriteString(line)
	return b.String()
}

const (
	margin     = "  "
	bulletPad  = "    "
	bulletCont = "      "
)

// renderEntry writes one release as a block: a header line, a rule, the headline, the summary,
// each group, and the collapsed count.
func renderEntry(b *strings.Builder, h releaseHeader, e changelogEntry, width int) {
	writeReleaseHeading(b, h, width)

	if e.Headline != "" {
		fmt.Fprintf(b, "%s\n\n", wrap(e.Headline, width, margin, margin))
	}
	if e.Summary != "" {
		fmt.Fprintf(b, "%s\n\n", wrap(e.Summary, width, margin, margin))
	}
	for _, g := range e.Groups {
		fmt.Fprintf(b, "%s%s\n", margin, g.Title)
		for _, item := range g.Items {
			fmt.Fprintf(b, "%s\n", wrap("- "+item, width, bulletPad, bulletCont))
		}
		b.WriteString("\n")
	}
	for _, item := range e.Loose {
		fmt.Fprintf(b, "%s\n", wrap("- "+item, width, bulletPad, bulletCont))
	}
	if len(e.Loose) > 0 {
		b.WriteString("\n")
	}
	if e.Collapsed != "" {
		fmt.Fprintf(b, "%s\n\n", wrap(e.Collapsed, width, margin, margin))
	}
	if e.Headline == "" && e.Summary == "" && len(e.Groups) == 0 && len(e.Loose) == 0 {
		fmt.Fprintf(b, "%s(no notes were published for this release)\n\n", margin)
	}
}

// writeReleaseHeading writes "  v0.2.1-beta.6  beta  <- installed        3 September 2026" and the
// rule beneath it, right-aligning the date when there is room for it.
func writeReleaseHeading(b *strings.Builder, h releaseHeader, width int) {
	left := margin + h.Tag
	if h.Beta {
		left += "  beta"
	}
	if h.Installed {
		left += "  <- installed"
	}
	date := prettyDate(h.Date)

	if date != "" && len(left)+2+len(date) <= width {
		fmt.Fprintf(b, "%s%s%s\n", left, strings.Repeat(" ", width-len(left)-len(date)), date)
	} else if date != "" {
		fmt.Fprintf(b, "%s\n%s%s\n", left, margin, date)
	} else {
		fmt.Fprintf(b, "%s\n", left)
	}
	fmt.Fprintf(b, "%s%s\n\n", margin, strings.Repeat("-", width-len(margin)))
}

// renderIndex writes the compact "earlier releases" list: one line per release, ending with the
// hint for reading any of them in full. The headline column is what makes a one-line index
// possible at all -- it only exists because entries carry one.
func renderIndex(b *strings.Builder, rows []indexRow, width int) {
	if len(rows) == 0 {
		return
	}
	fmt.Fprintf(b, "%sEarlier releases\n\n", margin)

	tagW := 0
	for _, r := range rows {
		if len(r.Tag) > tagW {
			tagW = len(r.Tag)
		}
	}
	for _, r := range rows {
		date := shortDate(r.Date)
		prefix := fmt.Sprintf("%s%-*s   %-11s   ", bulletPad, tagW, r.Tag, date)
		headline := r.Headline
		// The installed release can be an older one -- someone on the stable channel is
		// behind every beta -- so the marker has to be available here, not only on the
		// expanded entry above.
		suffix := ""
		if r.Installed {
			suffix = "   <- installed"
		}
		if room := width - len(prefix) - len(suffix); room > 3 && len(headline) > room {
			headline = headline[:room-3] + "..."
		}
		fmt.Fprintf(b, "%s%s%s\n", prefix, strings.TrimRight(headline, " "), suffix)
	}
	fmt.Fprintf(b, "\n%ssnapmap-plus changelog <version>   for any of these in full\n", margin)
}

// selectRelease resolves the user's version argument against the published releases. An empty
// argument means the newest release of any kind; "latest" means the newest STABLE one, matching
// what `snapmap-plus update` installs. Matching is exact on the tag (with an optional leading v and
// case-insensitively), never a prefix -- "v0.2.1" must not resolve to "v0.2.1-beta.6".
func selectRelease(list []ghRelease, arg string) (ghRelease, error) {
	if len(list) == 0 {
		return ghRelease{}, fmt.Errorf("no releases have been published yet")
	}
	switch strings.ToLower(strings.TrimSpace(arg)) {
	case "":
		return list[0], nil
	case "latest":
		for _, r := range list {
			if !r.Prerelease {
				return r, nil
			}
		}
		return list[0], nil // no stable release yet
	}

	want := strings.ToLower(strings.TrimSpace(arg))
	if !strings.HasPrefix(want, "v") {
		want = "v" + want
	}
	for _, r := range list {
		if strings.ToLower(r.TagName) == want {
			return r, nil
		}
	}

	var tags []string
	for i, r := range list {
		if i == 8 {
			tags = append(tags, "...")
			break
		}
		tags = append(tags, r.TagName)
	}
	return ghRelease{}, fmt.Errorf("no release %q. Published versions: %s",
		arg, strings.Join(tags, ", "))
}

// prettyDate turns an API timestamp into "3 September 2026". Anything unparseable is returned as
// given: a date is decoration here, never worth failing a command over.
func prettyDate(s string) string {
	t, ok := parseReleaseDate(s)
	if !ok {
		return s
	}
	return fmt.Sprintf("%d %s %d", t.Day(), t.Month(), t.Year())
}

// shortDate turns an API timestamp into "26 Aug 2026" for the index column.
func shortDate(s string) string {
	t, ok := parseReleaseDate(s)
	if !ok {
		return s
	}
	return fmt.Sprintf("%d %s %d", t.Day(), t.Month().String()[:3], t.Year())
}

func parseReleaseDate(s string) (time.Time, bool) {
	if len(s) < 10 {
		return time.Time{}, false
	}
	t, err := time.Parse("2006-01-02", s[:10])
	if err != nil {
		return time.Time{}, false
	}
	return t, true
}

// terminalWidth is the layout width. There is no syscall here on purpose: honouring COLUMNS keeps
// this portable stdlib Go with no OS-specific files, and 80 is a safe default everywhere. The
// clamp keeps prose readable -- a 300-column window should not produce 300-column paragraphs.
func terminalWidth() int {
	const fallback, min, max = 80, 60, 100
	w := fallback
	if s := envInt("COLUMNS"); s > 0 {
		w = s
	}
	if w < min {
		return min
	}
	if w > max {
		return max
	}
	return w
}

// envInt reads an environment variable as a positive integer, returning 0 when it is unset,
// empty, or not a number.
func envInt(name string) int {
	n, err := strconv.Atoi(strings.TrimSpace(os.Getenv(name)))
	if err != nil || n < 0 {
		return 0
	}
	return n
}

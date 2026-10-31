package main

import (
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
)

// Everything the installer prints goes through ui, so every command and the interactive prompt share one
// shape: a titled section per phase with a rule under it, two-column key and value lines, indented file
// lists, one marker-prefixed result line per step, and prompts set apart by a blank line. Colour is used only
// on a console stream that accepted virtual-terminal processing; --plain and NO_COLOR turn it off, and any
// other writer (a pipe, a file, a test buffer) never receives an escape code.

// Result markers, two ASCII characters each so the text after them lines up.
const (
	markOK   = "ok" // done
	markSkip = "--" // skipped or unchanged
	markWarn = "!!" // warning; the command carries on
	markFail = "xx" // error
)

const (
	indent       = "  "     // section text, fields and markers
	markerIndent = "      " // the column after a marker: continuation lines and list items
	labelWidth   = 18       // "Installed files:" plus two spaces
	listCountMin = 6        // a list this long ends with a count line
)

// The palette: headers bold, ok green, warnings yellow, errors red, paths, rules and skips muted grey.
const (
	ansiReset  = "\x1b[0m"
	ansiBold   = "\x1b[1m"
	ansiGreen  = "\x1b[32m"
	ansiYellow = "\x1b[33m"
	ansiRed    = "\x1b[31m"
	ansiGrey   = "\x1b[90m"
)

// console is the output layer. stdout and stderr are looked up on every write, so a test that swaps
// os.Stdout captures the output, and colour follows the writer actually written to.
type console struct {
	mu      sync.Mutex
	stdout  func() io.Writer
	stderr  func() io.Writer
	colorOn func(io.Writer) bool

	plain   bool   // --plain or NO_COLOR: never colour
	started bool   // anything has been written
	blank   bool   // the last line written was empty
	pending string // a section title printed only once something is written under it
}

// colorFiles are the console streams whose virtual-terminal processing setupOutput enabled.
var colorFiles struct {
	sync.Mutex
	files []*os.File
}

var ui = newConsole()

func newConsole() *console {
	return &console{
		stdout:  func() io.Writer { return os.Stdout },
		stderr:  func() io.Writer { return os.Stderr },
		colorOn: isColorFile,
	}
}

func isColorFile(w io.Writer) bool {
	f, ok := w.(*os.File)
	if !ok {
		return false
	}
	colorFiles.Lock()
	defer colorFiles.Unlock()
	for _, cf := range colorFiles.files {
		if cf == f {
			return true
		}
	}
	return false
}

// decideColor is the colour rule: never with --plain or a non-empty NO_COLOR, never on a stream that is not
// a console, and otherwise only when the console accepts virtual-terminal processing.
func decideColor(plain bool, noColor string, isConsole bool, enableVT func() bool) bool {
	if plain || noColor != "" || !isConsole {
		return false
	}
	return enableVT()
}

// setupOutput applies the colour rule to stdout and stderr, once at start-up.
func setupOutput(plain bool) {
	ui.setPlain(plain)
	colorFiles.Lock()
	defer colorFiles.Unlock()
	colorFiles.files = nil
	for _, f := range []*os.File{os.Stdout, os.Stderr} {
		f := f
		if decideColor(plain, os.Getenv("NO_COLOR"), isConsoleFile(f), func() bool { return enableVirtualTerminal(f) }) {
			colorFiles.files = append(colorFiles.files, f)
		}
	}
}

// stripPlain removes every --plain from args and reports whether one was there.
func stripPlain(args []string) ([]string, bool) {
	out := make([]string, 0, len(args))
	found := false
	for _, a := range args {
		if a == "--plain" {
			found = true
			continue
		}
		out = append(out, a)
	}
	return out, found
}

// setPlain turns colour off for the rest of the run; it never turns it back on.
func (c *console) setPlain(p bool) {
	c.mu.Lock()
	c.plain = c.plain || p
	c.mu.Unlock()
}

func (c *console) paint(w io.Writer, code, s string) string {
	if c.plain || s == "" || !c.colorOn(w) {
		return s
	}
	return code + s + ansiReset
}

// writeLines writes complete lines; the caller holds c.mu.
func (c *console) writeLines(w io.Writer, lines ...string) {
	for _, l := range lines {
		io.WriteString(w, l+"\n")
		c.started = true
		c.blank = l == ""
	}
}

// gap writes the one blank line that separates blocks, unless nothing is written yet or one already ends
// the output.
func (c *console) gap(w io.Writer) {
	if c.started && !c.blank {
		c.writeLines(w, "")
	}
}

// flushPending prints a deferred section title ahead of the first line under it.
func (c *console) flushPending(w io.Writer) {
	if c.pending == "" {
		return
	}
	title := c.pending
	c.pending = ""
	c.gap(w)
	c.writeLines(w, c.paint(w, ansiBold, title), c.paint(w, ansiGrey, strings.Repeat("-", len(title))))
}

func (c *console) writer(toErr bool) io.Writer {
	if toErr {
		return c.stderr()
	}
	return c.stdout()
}

// emitLocked writes lines under any pending section title; the caller holds c.mu.
func (c *console) emitLocked(toErr bool, lines ...string) {
	w := c.writer(toErr)
	c.flushPending(w)
	c.writeLines(w, lines...)
}

// Section starts a phase: one blank line, the title, and a rule under it.
func (c *console) Section(title string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.pending = title
	c.flushPending(c.stdout())
}

// LazySection starts a phase whose title appears only once something is printed under it, for phases that
// usually have nothing to report. A later Section replaces it.
func (c *console) LazySection(title string) {
	c.mu.Lock()
	c.pending = title
	c.mu.Unlock()
}

// Blank separates two blocks inside a section; consecutive calls still leave a single blank line.
func (c *console) Blank() {
	c.mu.Lock()
	defer c.mu.Unlock()
	w := c.stdout()
	c.flushPending(w)
	c.gap(w)
}

// fieldLabel pads "label:" to the value column.
func fieldLabel(label string) string {
	l := label + ":"
	if len(l) < labelWidth {
		return indent + l + strings.Repeat(" ", labelWidth-len(l))
	}
	return indent + l + " "
}

// Field prints a key and value line with the value in a fixed column.
func (c *console) Field(label, value string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.emitLocked(false, fieldLabel(label)+value)
}

// PathField is Field for a value that is a path, muted when coloured.
func (c *console) PathField(label, path string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.emitLocked(false, fieldLabel(label)+c.paint(c.stdout(), ansiGrey, path))
}

// Note prints section text; each embedded line keeps the section indent.
func (c *console) Note(format string, a ...any) {
	var lines []string
	for _, l := range strings.Split(fmt.Sprintf(format, a...), "\n") {
		lines = append(lines, strings.TrimRight(indent+l, " "))
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.emitLocked(false, lines...)
}

// result prints one marker line; embedded lines continue in the column after the marker.
func (c *console) result(toErr bool, mark, code, format string, a ...any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	w := c.writer(toErr)
	parts := strings.Split(fmt.Sprintf(format, a...), "\n")
	lines := []string{strings.TrimRight(indent+c.paint(w, code, mark)+"  "+parts[0], " ")}
	for _, p := range parts[1:] {
		lines = append(lines, strings.TrimRight(markerIndent+p, " "))
	}
	c.emitLocked(toErr, lines...)
}

// OK reports a completed step.
func (c *console) OK(format string, a ...any) { c.result(false, markOK, ansiGreen, format, a...) }

// Skip reports a step that was skipped or left something unchanged.
func (c *console) Skip(format string, a ...any) { c.result(false, markSkip, ansiGrey, format, a...) }

// Warn reports a problem the command carries on past. Warnings go to stderr.
func (c *console) Warn(format string, a ...any) { c.result(true, markWarn, ansiYellow, format, a...) }

// Fail reports an error. Errors go to stderr.
func (c *console) Fail(format string, a ...any) { c.result(true, markFail, ansiRed, format, a...) }

// fileList prints its heading with the first item, one path per line under it, and a count line on
// Close when the list is long. An empty list prints nothing.
type fileList struct {
	c       *console
	heading string
	n       int
}

// List begins a file list under heading.
func (c *console) List(heading string) *fileList { return &fileList{c: c, heading: heading} }

// Add prints one path of the list.
func (l *fileList) Add(path string) {
	c := l.c
	c.mu.Lock()
	defer c.mu.Unlock()
	if l.n == 0 {
		c.emitLocked(false, indent+l.heading+":")
	}
	l.n++
	c.emitLocked(false, markerIndent+c.paint(c.stdout(), ansiGrey, path))
}

// Close ends the list, with a count line when it is long.
func (l *fileList) Close() {
	if l.n < listCountMin {
		return
	}
	c := l.c
	c.mu.Lock()
	defer c.mu.Unlock()
	c.emitLocked(false, markerIndent+c.paint(c.stdout(), ansiGrey, fmt.Sprintf("%d files", l.n)))
}

// Ask prints a question set apart by a blank line and leaves the cursor after it. Every line is indented
// like section text; the last one is the question itself, followed by the answer suffix such as "[Y/n]".
func (c *console) Ask(question, suffix string) { c.ask(question, suffix, indent) }

// CommandPrompt prints the interactive mode's command prompt at the left margin.
func (c *console) CommandPrompt(p string) { c.ask(p, "", "") }

func (c *console) ask(question, suffix, pad string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	w := c.stdout()
	c.flushPending(w)
	c.gap(w)
	lines := strings.Split(question, "\n")
	for _, l := range lines[:len(lines)-1] {
		c.writeLines(w, strings.TrimRight(pad+l, " "))
	}
	s := pad + c.paint(w, ansiBold, strings.TrimRight(lines[len(lines)-1], " "))
	if suffix != "" {
		s += " " + suffix
	}
	io.WriteString(w, s+" ")
	c.started, c.blank = true, false
}

// Answered records that the prompt line has ended. closed means input ended without an answer, so a newline
// moves the cursor off the prompt.
func (c *console) Answered(closed bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if closed {
		io.WriteString(c.stdout(), "\n")
	}
	c.started, c.blank = true, false
}

// Line writes one line another program printed, as given, under any pending section title.
func (c *console) Line(s string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.emitLocked(false, strings.TrimRight(s, " "))
}

// Raw writes preformatted text, such as the changelog, unchanged and uncoloured.
func (c *console) Raw(text string) {
	if text == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	w := c.stdout()
	c.flushPending(w)
	io.WriteString(w, text)
	c.started = true
	c.blank = text == "\n" || strings.HasSuffix(text, "\n\n")
}

// Help writes the usage text with its first line and the named heading lines in bold.
func (c *console) Help(text string, headings ...string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	w := c.stdout()
	c.flushPending(w)
	for i, l := range strings.Split(strings.TrimRight(text, "\n"), "\n") {
		bold := i == 0
		for _, h := range headings {
			bold = bold || l == h
		}
		if bold {
			l = c.paint(w, ansiBold, l)
		}
		c.writeLines(w, l)
	}
}

// External records that another program wrote to the console, so the next block is set apart from it.
func (c *console) External() {
	c.mu.Lock()
	c.started, c.blank, c.pending = true, false, ""
	c.mu.Unlock()
}

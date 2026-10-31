package main

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// testConsole writes stdout and stderr into one buffer, in order, with colour forced on or off.
func testConsole(color bool) (*console, *strings.Builder) {
	b := &strings.Builder{}
	c := &console{
		stdout:  func() io.Writer { return b },
		stderr:  func() io.Writer { return b },
		colorOn: func(io.Writer) bool { return color },
	}
	return c, b
}

func TestFieldsShareOneValueColumn(t *testing.T) {
	c, b := testConsole(false)
	for _, label := range []string{"Game", "Build", "Folder", "Version", "Installed files"} {
		c.Field(label, "value")
	}
	c.PathField("Folder", `C:\DOOM`)
	for _, line := range strings.Split(strings.TrimRight(b.String(), "\n"), "\n") {
		if !strings.HasPrefix(line, "  ") || strings.Index(line, ":") < 0 {
			t.Fatalf("field line %q is not an indented label", line)
		}
		if got := len(line) - len(strings.TrimLeft(line[strings.Index(line, ":")+1:], " ")); got != len(indent)+labelWidth {
			t.Errorf("value of %q starts at column %d, want %d", line, got, len(indent)+labelWidth)
		}
	}
}

func TestResultMarkersShareOneColumn(t *testing.T) {
	c, b := testConsole(false)
	c.OK("done")
	c.Skip("skipped")
	c.Warn("careful\nsecond line")
	c.Fail("broken")
	want := "  ok  done\n" +
		"  --  skipped\n" +
		"  !!  careful\n" +
		"      second line\n" +
		"  xx  broken\n"
	if b.String() != want {
		t.Errorf("got:\n%s\nwant:\n%s", b.String(), want)
	}
}

func TestWarningsAndErrorsGoToStderr(t *testing.T) {
	var out, errb strings.Builder
	c := &console{
		stdout:  func() io.Writer { return &out },
		stderr:  func() io.Writer { return &errb },
		colorOn: func(io.Writer) bool { return false },
	}
	c.OK("a")
	c.Warn("b")
	c.Fail("c")
	if out.String() != "  ok  a\n" || errb.String() != "  !!  b\n  xx  c\n" {
		t.Errorf("stdout %q, stderr %q", out.String(), errb.String())
	}
}

// One blank line between sections, never two, none before the first, and a lazy section with nothing
// under it never appears.
func TestSectionRhythm(t *testing.T) {
	c, b := testConsole(false)
	c.Section("Detecting DOOM")
	c.Field("Folder", "x")
	c.Blank()
	c.Blank()
	c.Note("text")
	c.LazySection("Migrating player data")
	c.Section("Installing files")
	c.Blank()
	c.Section("Status")
	c.LazySection("Updating snapmap-plus.exe")
	c.OK("written under the lazy title")
	want := "Detecting DOOM\n--------------\n" +
		"  Folder:           x\n" +
		"\n" +
		"  text\n" +
		"\n" +
		"Installing files\n----------------\n" +
		"\n" +
		"Status\n------\n" +
		"\n" +
		"Updating snapmap-plus.exe\n-------------------------\n" +
		"  ok  written under the lazy title\n"
	if b.String() != want {
		t.Errorf("got:\n%s\nwant:\n%s", b.String(), want)
	}
	if strings.Contains(b.String(), "\n\n\n") {
		t.Error("two blank lines in a row")
	}
}

func TestFileListsIndentAndCount(t *testing.T) {
	c, b := testConsole(false)
	empty := c.List("Removed files")
	empty.Close()
	if b.Len() != 0 {
		t.Fatalf("an empty list printed %q", b.String())
	}
	short := c.List("Installed files")
	short.Add("XINPUT1_4.dll")
	short.Add(`snapmap-plus\snapmap-plus-ui-13954591.dll`)
	short.Close()
	if want := "  Installed files:\n      XINPUT1_4.dll\n      snapmap-plus\\snapmap-plus-ui-13954591.dll\n"; b.String() != want {
		t.Errorf("short list:\n%s", b.String())
	}
	b.Reset()
	long := c.List("Removed files")
	for i := 0; i < listCountMin; i++ {
		long.Add("f.dll")
	}
	long.Close()
	if !strings.HasSuffix(b.String(), "      f.dll\n      6 files\n") {
		t.Errorf("long list has no count line:\n%s", b.String())
	}
}

func TestAskIsSetApartWithASuffix(t *testing.T) {
	c, b := testConsole(false)
	c.Note("before")
	c.Ask("About to install into\n  C:\\DOOM\nContinue?", "[Y/n]")
	c.Answered(false)
	c.Section("Next")
	want := "  before\n\n  About to install into\n    C:\\DOOM\n  Continue? [Y/n] \nNext\n----\n"
	// The answer's newline comes from the user's Enter, so the captured text has the section's gap line
	// right after the prompt.
	if b.String() != want {
		t.Errorf("got:\n%q\nwant:\n%q", b.String(), want)
	}
}

func TestColourOnUsesThePalette(t *testing.T) {
	c, b := testConsole(true)
	c.Section("Status")
	c.PathField("Folder", `C:\DOOM`)
	c.OK("a")
	c.Skip("b")
	c.Warn("c")
	c.Fail("d")
	out := b.String()
	for _, want := range []string{
		ansiBold + "Status" + ansiReset,
		ansiGrey + "------" + ansiReset,
		ansiGrey + `C:\DOOM` + ansiReset,
		"  " + ansiGreen + "ok" + ansiReset + "  a",
		"  " + ansiGrey + "--" + ansiReset + "  b",
		"  " + ansiYellow + "!!" + ansiReset + "  c",
		"  " + ansiRed + "xx" + ansiReset + "  d",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in %q", want, out)
		}
	}
}

func TestColourOffAndPlainEmitNoEscapes(t *testing.T) {
	write := func(c *console) {
		c.Section("Status")
		c.PathField("Folder", `C:\DOOM`)
		l := c.List("Installed files")
		l.Add("a.dll")
		l.Close()
		c.OK("a")
		c.Skip("b")
		c.Warn("c")
		c.Fail("d")
		c.Ask("Continue?", "[Y/n]")
		c.Help("title\nUsage:\n  x", "Usage:")
	}
	off, b := testConsole(false)
	write(off)
	if strings.Contains(b.String(), "\x1b") {
		t.Errorf("colour off emitted an escape: %q", b.String())
	}
	plain, pb := testConsole(true)
	plain.setPlain(true)
	write(plain)
	if strings.Contains(pb.String(), "\x1b") {
		t.Errorf("--plain emitted an escape: %q", pb.String())
	}
}

func TestDecideColor(t *testing.T) {
	for _, c := range []struct {
		name      string
		plain     bool
		noColor   string
		isConsole bool
		vt        bool
		want      bool
		wantAsked bool
	}{
		{"console with VT", false, "", true, true, true, true},
		{"console refusing VT", false, "", true, false, false, true},
		{"not a console", false, "", false, true, false, false},
		{"--plain", true, "", true, true, false, false},
		{"NO_COLOR", false, "1", true, true, false, false},
	} {
		asked := false
		got := decideColor(c.plain, c.noColor, c.isConsole, func() bool { asked = true; return c.vt })
		if got != c.want || asked != c.wantAsked {
			t.Errorf("%s: colour %v (VT asked %v), want %v (%v)", c.name, got, asked, c.want, c.wantAsked)
		}
	}
}

func TestStripPlain(t *testing.T) {
	args, found := stripPlain([]string{"--plain", "install", "--doom", `C:\DOOM`, "--plain"})
	if !found || strings.Join(args, " ") != `install --doom C:\DOOM` {
		t.Errorf("stripPlain = %v, %v", args, found)
	}
	if args, found := stripPlain([]string{"status"}); found || len(args) != 1 {
		t.Errorf("stripPlain without the flag = %v, %v", args, found)
	}
}

// A stream that is not a console (here a pipe, as when output is redirected) never gets colour, and a real
// command written to it carries no escape code.
func TestPipedOutputGetsNoColour(t *testing.T) {
	t.Setenv("NO_COLOR", "")
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	doom := doomWith(t, tmp, fakeExe, fakeGame)
	if err := saveRecord(&installRecord{Version: "v1.2.3", DoomPath: doom, InstalledAt: "2026-10-31T00:00:00Z",
		Files: []string{"XINPUT1_4.dll", "missing.dll"}}); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { colorFiles.files = nil })
	out := captureStdout(t, func() {
		setupOutput(false)
		if isColorFile(os.Stdout) {
			t.Error("a pipe was treated as a console")
		}
		if err := cmdStatus(flags{}); err != nil {
			t.Error(err)
		}
	})
	if strings.Contains(out, "\x1b") {
		t.Errorf("piped status output carries an escape: %q", out)
	}
	for _, want := range []string{"Status\n------\n", "  Version:          v1.2.3\n",
		"  Installed files:  2\n", "  Game:             DOOMx64vk.exe: DOOM 2016 build 13954591"} {
		if !strings.Contains(out, want) {
			t.Errorf("status output lacks %q:\n%s", want, out)
		}
	}
}

// --plain is accepted after any command, including one typed at the interactive prompt.
func TestRunCommandAcceptsPlain(t *testing.T) {
	saved := ui.plain
	t.Cleanup(func() { ui.plain = saved })
	tmp := t.TempDir()
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	out := captureStdout(t, func() {
		if err := runCommand("version", []string{"--plain"}); err != nil {
			t.Error(err)
		}
	})
	if !ui.plain || !strings.Contains(out, "snapmap-plus "+version) {
		t.Errorf("plain %v, output %q", ui.plain, out)
	}
}

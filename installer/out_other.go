//go:build !windows

package main

import (
	"io"
	"os"
)

func openMigrationInput() io.ReadCloser {
	if !isInteractive() {
		return nil
	}
	return io.NopCloser(stdin)
}

// isConsoleFile reports whether f is a terminal.
func isConsoleFile(f *os.File) bool {
	fi, err := f.Stat()
	return err == nil && fi.Mode()&os.ModeCharDevice != 0
}

// enableVirtualTerminal: terminals outside Windows interpret ANSI sequences already.
func enableVirtualTerminal(f *os.File) bool { return isConsoleFile(f) }

func restoreConsole() {}

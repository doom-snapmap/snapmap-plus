package main

import (
	"os"
	"testing"
)

// TestMain keeps every test offline and non-interactive: run from a console, stdin is a terminal, and the
// lifecycle tests must not depend on how an unanswered prompt resolves.
func TestMain(m *testing.M) {
	isInteractive = func() bool { return false }
	// No test reaches GitHub: an unmocked release lookup goes to a closed loopback port.
	os.Setenv(apiBaseEnv, "http://127.0.0.1:1")
	os.Exit(m.Run())
}

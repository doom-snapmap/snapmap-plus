package main

import (
	"os"
	"testing"
)

func TestCommandHelpDoesNotPerformAction(t *testing.T) {
	for _, command := range []string{"install", "update", "uninstall"} {
		for _, option := range []string{"--help", "-h"} {
			t.Run(command+"/"+option, func(t *testing.T) {
				root := t.TempDir()
				if err := runCommand(command, []string{"--doom", root, option}); err != nil {
					t.Fatalf("help attempted the command: %v", err)
				}
				entries, err := os.ReadDir(root)
				if err != nil || len(entries) != 0 {
					t.Fatalf("help changed the target directory: %v, %v", entries, err)
				}
			})
		}
	}
}

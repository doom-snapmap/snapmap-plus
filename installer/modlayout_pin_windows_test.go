//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestMoveModCacheKeepsVerifiedMixedCaseUnit(t *testing.T) {
	data := t.TempDir()
	body := "{\"files\":[]}\n"
	unit := sha256Hex([]byte(body)) + ".json"
	old := filepath.Join(data, "mods", ".content")
	cache := filepath.Join(data, "cache")
	dest := filepath.Join(cache, "content", "units", unit)
	writeF(t, filepath.Join(old, "Units", strings.ToUpper(unit)), body)
	writeF(t, filepath.Join(dest, "prior"), "displaced directory")
	if err := upgradeModLayout(data); err == nil || readF(t, filepath.Join(dest, "prior")) != "displaced directory" || readF(t, filepath.Join(old, "Units", strings.ToUpper(unit))) != body {
		t.Fatal("mixed-case collision displaced a verified unit or folder")
	}
}

func TestCacheMoveRestoresDestinationWhenSourceBusy(t *testing.T) {
	data := t.TempDir()
	payload := "verified object"
	key := sha256Hex([]byte(payload))
	rel := filepath.Join("content", "files", key[:2], key)
	src := filepath.Join(data, "mods", ".content", "files", key[:2], key)
	dst := filepath.Join(data, "cache", rel)
	writeF(t, src, payload)
	writeF(t, dst, "previous contents")
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(src), syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	e := upgradeModLayout(data)
	syscall.CloseHandle(h)
	if e == nil || readF(t, dst) != "previous contents" || readF(t, src) != payload {
		t.Fatal("a sharing violation displaced cache content")
	}
	if err := os.Rename(dst, dst+".mine"); err != nil {
		t.Fatal(err)
	}
	if err := upgradeModLayout(data); err != nil || readF(t, dst) != payload {
		t.Fatal("repair did not resume", err)
	}
}

func TestOnlyGeneratedCacheMovesAvoidAuthorReads(t *testing.T) {
	data := t.TempDir()
	mods, cache := filepath.Join(data, "mods"), filepath.Join(data, "cache")
	// The sharing lock exposes accidental reads of an unconnected author folder.
	author := filepath.Join(mods, "ordinary-author", "mod.json")
	writeF(t, author, "author content")
	writeF(t, filepath.Join(data, "rawmaps", "local.json"), "original map")
	writeF(t, filepath.Join(data, "backups", "existing"), "existing backup")
	writeF(t, filepath.Join(mods, ".builds", "past", "asset"), "original build")
	writeF(t, filepath.Join(mods, ".served", "banks", "a.bnk"), "bank")
	writeF(t, filepath.Join(mods, ".content", "partial", "blob.part"), "pending")
	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(author), syscall.GENERIC_READ, 0, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := upgradeModLayout(data); err != nil && !errors.Is(err, syscall.Errno(32)) {
		t.Fatal(err)
	}
	syscall.CloseHandle(h)
	if err := upgradeModLayout(data); err != nil {
		t.Fatal(err)
	}
	if readF(t, filepath.Join(mods, ".builds", "past", "asset")) != "original build" || readF(t, filepath.Join(cache, "content", "partial", "blob.part")) != "pending" || readF(t, filepath.Join(cache, "served", "banks", "a.bnk")) != "bank" || readF(t, filepath.Join(data, "backups", "existing")) != "existing backup" || readF(t, filepath.Join(data, "rawmaps", "local.json")) != "original map" {
		t.Fatal("unrelated or locked author data changed")
	}
}

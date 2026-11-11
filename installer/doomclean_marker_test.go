package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The marker is found anywhere in a file of any size, as ASCII or UTF-16, even split across the
// windows the search reads it in; a file without it is not ours.
func TestOurMarkerAnywhereInTheFile(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, size int, at int, marker []byte) string {
		data := make([]byte, size)
		copy(data[at:], marker)
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, data, 0o644); err != nil {
			t.Fatal(err)
		}
		return path
	}
	wide := make([]byte, 0, 2*len(ourMarker))
	for _, c := range ourMarker {
		wide = append(wide, c, 0)
	}
	big := 300 << 20 // past the 256 MiB the search used to stop at
	if !carriesOurMarker(write("late.dll", big, big-len(ourMarker), ourMarker)) {
		t.Error("a marker past 256 MiB was not found")
	}
	if !carriesOurMarker(write("split.dll", 3<<20, (1<<20)-5, wide)) {
		t.Error("a UTF-16 marker across a window boundary was not found")
	}
	if carriesOurMarker(write("other.dll", 3<<20, 0, []byte("someone-else"))) {
		t.Error("a file without the marker was taken as ours")
	}
}

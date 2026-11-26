package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"maps"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"sort"
	"strconv"
	"strings"
)

var earlierCarrierName = regexp.MustCompile(`^smpkg\.([a-zA-Z0-9_.-]+)\.([0-9]{1,7})\.([0-9]{1,7})\.([0-9a-f]{16})$`)

type earlierCarrier struct {
	key, digest string
	parts       []string
}

func addEarlierCarrier(all map[string]*earlierCarrier, name, value string) error {
	fields := earlierCarrierName.FindStringSubmatch(name)
	if fields == nil {
		return fmt.Errorf("unrecognized embedded package row %s", name)
	}
	i, _ := strconv.Atoi(fields[2])
	n, _ := strconv.Atoi(fields[3])
	if n == 0 || n > 262144 || i >= n || value == "" {
		return fmt.Errorf("invalid package chunk %s", name)
	}
	key := fields[1]
	c := all[key]
	if c == nil {
		c = &earlierCarrier{key: key, digest: fields[4], parts: make([]string, n)}
		all[key] = c
	}
	if c.digest != fields[4] || len(c.parts) != n || c.parts[i] != "" {
		return fmt.Errorf("conflicting chunks for package %s", key)
	}
	c.parts[i] = value
	return nil
}

func (c *earlierCarrier) unpack(root string) error {
	chunks := make([]io.Reader, len(c.parts))
	for i, value := range c.parts {
		if value == "" {
			return fmt.Errorf("missing chunk %d of %s", i, c.key)
		}
		chunks[i] = strings.NewReader(value)
	}
	f, err := os.CreateTemp(root, "payload-*.zip")
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), base64.NewDecoder(base64.StdEncoding, io.MultiReader(chunks...)))
	if err != nil {
		return err
	}
	if hex.EncodeToString(h.Sum(nil)[:8]) != c.digest {
		return fmt.Errorf("embedded package %s differs from its recorded hash", c.key)
	}
	z, err := zip.NewReader(f, n)
	if err != nil {
		return err
	}
	if len(z.File) > 4096 {
		return fmt.Errorf("embedded package exceeds the earlier file count limit")
	}
	base := filepath.Join(root, "source")
	if err = os.Mkdir(base, 0755); err != nil {
		return err
	}
	var total uint64
	for _, entry := range z.File {
		name := strings.TrimSuffix(entry.Name, "/")
		if !pinRelative(name) || entry.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("invalid package entry %q", name)
		}
		dst := filepath.Join(base, filepath.FromSlash(name))
		if entry.FileInfo().IsDir() {
			continue
		}
		if entry.UncompressedSize64 > 64<<20 || total+entry.UncompressedSize64 > 256<<20 {
			return fmt.Errorf("embedded package exceeds its original file size limits")
		}
		total += entry.UncompressedSize64
		if err = os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return err
		}
		input, err := entry.Open()
		if err != nil {
			return err
		}
		out, err := os.OpenFile(dst, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0644)
		if err != nil {
			input.Close()
			return err
		}
		written, err := io.Copy(out, input)
		closed := out.Close()
		input.Close()
		if err != nil {
			return err
		}
		if closed != nil {
			return closed
		}
		if uint64(written) != entry.UncompressedSize64 {
			return fmt.Errorf("short package entry %q", name)
		}
	}
	return nil
}

func earlierPlacedNames(root earlierObject, id string, added earlierGlobalNames) []string {
	var entities []struct {
		Type      string `json:"~type"`
		EntityDef struct {
			Type    string `json:"~type"`
			Inherit string
		}
	}
	var modules []struct{ ModuleName string }
	_ = json.Unmarshal(root["entities"], &entities)
	_ = json.Unmarshal(root["instances"], &modules)
	own := "mods/" + id + "/"
	seen := map[string]bool{}
	for _, e := range entities {
		if strings.HasPrefix(strings.ToLower(e.EntityDef.Inherit), own) {
			seen[e.EntityDef.Inherit] = true
		} else if e.Type == "idSnapEntity" && e.EntityDef.Type == "idDeclEntityDef" {
			if f, _, ok := added.at(e.EntityDef.Inherit, "entitydef"); ok {
				seen[f.reference] = true
			}
		}
	}
	for _, m := range modules {
		if strings.HasPrefix(strings.ToLower(m.ModuleName), "maps/modules/"+own) {
			seen[m.ModuleName] = true
		}
	}
	names := make([]string, 0, len(seen))
	for name := range seen {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

type earlierCarriedResolver func(root earlierObject, deliveries map[string]*earlierCarrier, ids map[string]string) ([]json.RawMessage, error)

type carriedMod struct {
	source, id, salt, display string
	requests                  []string
}

func (c *earlierCarrier) readSource(work string) (carriedMod, error) {
	if err := c.unpack(work); err != nil {
		return carriedMod{}, err
	}
	src := filepath.Join(work, "source")
	doc, name, err := earlierDescriptor(src)
	if err != nil {
		return carriedMod{}, err
	}
	if doc == nil {
		return carriedMod{}, fmt.Errorf("embedded package %s has no descriptor", c.key)
	}
	id, _ := doc["id"].(string)
	if id == "" {
		if !strings.HasPrefix(c.key, "delivery.") && earlierID.MatchString(c.key) {
			id = c.key
		} else {
			id = "legacy-" + c.digest
		}
		doc["id"] = id
		raw, err := json.Marshal(doc)
		if err != nil {
			return carriedMod{}, err
		}
		if err = writeAtomicFile(filepath.Join(src, name), raw); err != nil {
			return carriedMod{}, err
		}
	}
	_, salt, err := earlierModIdentity(doc)
	if err != nil {
		return carriedMod{}, err
	}
	files, err := earlierSourceFiles(src, name, doc, nil)
	if err != nil {
		return carriedMod{}, err
	}
	display, _ := doc["name"].(string)
	if strings.TrimSpace(display) == "" {
		display = id
	}
	return carriedMod{src, id, salt, display, slices.Sorted(maps.Keys(files))}, nil
}

// Earlier saves could carry several deliveries of the same mod. Build their compatible source files together.
func (p *earlierPins) pinEarlierDeliveries(root earlierObject, deliveries map[string]*earlierCarrier, ids map[string]string, placed *earlierGlobalNames) ([]json.RawMessage, error) {
	work, err := os.MkdirTemp("", "snapmap-plus-carried-*")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(work)
	mods := map[string][]carriedMod{}
	var order []string
	for _, key := range slices.Sorted(maps.Keys(deliveries)) {
		part, err := os.MkdirTemp(work, "delivery-*")
		if err != nil {
			return nil, err
		}
		m, err := deliveries[key].readSource(part)
		if err != nil {
			return nil, err
		}
		prior := mods[m.salt]
		if len(prior) == 0 {
			order = append(order, m.salt)
		} else if prior[0].id != m.id {
			return nil, fmt.Errorf("embedded packages %s and %s have the same namespace", prior[0].id, m.id)
		}
		mods[m.salt] = append(prior, m)
	}
	var rows []json.RawMessage
	for _, salt := range order {
		parts := mods[salt]
		m := parts[0]
		var requests []string
		for _, part := range parts {
			requests = append(requests, part.requests...)
		}
		sort.Strings(requests)
		added, err := p.sourceGlobalNames(m.id, work, slices.Compact(requests))
		if err != nil {
			return nil, err
		}
		for _, f := range added.files {
			if err := placed.add(f); err != nil {
				return nil, err
			}
		}
		joined := filepath.Join(work, salt)
		for _, part := range parts {
			folder := filepath.Join(filepath.Dir(part.source), "author")
			_, err = translateEarlierFiles(part.source, folder, added, nil, nil, nil, p.doom)
			if err != nil {
				return nil, err
			}
			if err = copyEarlierPinTree(folder, joined); err != nil {
				return nil, fmt.Errorf("embedded deliveries of %s: %w", m.id, err)
			}
		}
		names := earlierPlacedNames(root, strings.ToLower(m.id), added)
		row, err := p.pinCarried(joined, m.id, m.display, names)
		if err != nil {
			return nil, err
		}
		if m.id != salt {
			ids[m.id] = salt
		}
		raw, err := json.Marshal(row)
		if err != nil {
			return nil, err
		}
		rows = append(rows, raw)
	}
	return rows, nil
}

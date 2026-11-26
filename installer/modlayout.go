package main

import (
	"bytes"
	"encoding/json"
	"regexp"
)

// The verified backend carries this same manifest path when it supports the split layout.
const modBuildManifest = `.build\manifest.json`

var modSalt = regexp.MustCompile(`^[a-z][a-z0-9]{7}$`)
var modWorkFolder = regexp.MustCompile(`\.(old|staging)-[0-9]+$`)

func upgradeModLayout(data string) error { return convertEarlierData("", data) }

type modSourceIdentity struct {
	Format        int             `json:"format"`
	Salt          string          `json:"salt"`
	CampaignCVars json.RawMessage `json:"campaign_cvars,omitempty"`
}

// Decoder offsets let the installer replace only the builder's value, preserving every author byte.
func inlineModBuild(data []byte) (int, []byte, []byte) {
	text := bytes.TrimPrefix(data, []byte("\xef\xbb\xbf"))
	if !json.Valid(text) {
		return 0, nil, nil
	}
	d := json.NewDecoder(bytes.NewReader(text))
	if tok, _ := d.Token(); tok != json.Delim('{') {
		return 0, nil, nil
	}
	for d.More() {
		key, _ := d.Token()
		var raw json.RawMessage
		if err := d.Decode(&raw); err != nil {
			return 0, nil, nil
		}
		if key != "snapmap-plus" {
			continue
		}
		var block struct {
			modSourceIdentity
			Rules json.RawMessage `json:"rules"`
		}
		if json.Unmarshal(raw, &block) != nil || block.Format != 2 || !modSalt.MatchString(block.Salt) || len(block.Rules) == 0 {
			return 0, nil, nil
		}
		end := len(data) - len(text) + int(d.InputOffset())
		identity, err := json.Marshal(block.modSourceIdentity)
		if err != nil {
			return 0, nil, nil
		}
		return end - len(raw), raw, identity
	}
	return 0, nil, nil
}

package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"unicode/utf8"
)

var editorLocale = regexp.MustCompile(`^[a-z]{2,3}(-([A-Z]{2}|[0-9]{3}|[A-Z][a-z]{3}))*$`)
var editorLanguageCodes = map[string]string{
	"english": "en", "french": "fr", "german": "de", "italian": "it", "spanish": "es", "latin_spanish": "es-419",
	"portuguese": "pt", "polish": "pl", "russian": "ru", "japanese": "ja", "chinese": "zh-Hant",
}

// A converted mod and loose overrides can both contribute to the same staged author file.
func (p *migrationPlan) plannedAuthorFile(path string) (string, error) {
	if !plainPath(path) {
		return "", fmt.Errorf("linked author file %s", path)
	}
	if i, ok := p.destinations[strings.ToLower(path)]; ok {
		if p.Changes[i].After == "" || p.Changes[i].Move != "" {
			return "", fmt.Errorf("author file is being moved or removed: %s", path)
		}
		return filepath.Join(p.work, fmt.Sprintf("new-%d", i)), nil
	}
	return path, nil
}

func (p *migrationPlan) updateAuthorFile(path, source string, body []byte) error {
	if samePath(source, path) {
		return p.putBytes(path, body, "Mods", true)
	}
	if err := writeAtomicFile(source, body); err != nil {
		return err
	}
	hash, err := fileSHA256(source)
	if err == nil {
		p.Changes[p.destinations[strings.ToLower(path)]].After = hash
	}
	return err
}

func editorJSON(raw []byte) ([]byte, error) {
	body := bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf})
	if !utf8.Valid(body) || !json.Valid(body) || !configSurrogatesValid(body) {
		return nil, fmt.Errorf("invalid UTF-8 JSON")
	}
	d := json.NewDecoder(bytes.NewReader(body))
	d.UseNumber()
	return body, checkConfigValue(d, 0)
}

func editorWords(raw []byte) (map[string]string, error) {
	body, err := editorJSON(raw)
	if err != nil {
		return nil, err
	}
	var result map[string]string
	if json.Unmarshal(body, &result) != nil || result == nil {
		return nil, fmt.Errorf("expected an object of translated strings")
	}
	var words map[string]json.RawMessage
	_ = json.Unmarshal(body, &words)
	for key, value := range words {
		if len(value) == 0 || value[0] != '"' {
			return nil, fmt.Errorf("expected text for %q", key)
		}
	}
	return result, nil
}

func (p *migrationPlan) editorCatalogs(dir string) error {
	path := filepath.Join(dir, "mod.json")
	source, err := p.plannedAuthorFile(path)
	if err != nil {
		return err
	}
	raw, err := os.ReadFile(source)
	if err != nil {
		return err
	}
	body := bytes.TrimPrefix(raw, []byte{0xef, 0xbb, 0xbf})
	fields, err := configMembers(body)
	if err != nil {
		return err
	}
	for i, field := range fields {
		if field.name != "strings" {
			continue
		}
		if _, err = editorJSON(raw); err != nil {
			return err
		}
		var catalogs map[string]json.RawMessage
		if json.Unmarshal(body[field.value:field.end], &catalogs) != nil || catalogs == nil {
			return fmt.Errorf("expected language catalogs in mod.json strings")
		}
		locales := make([]string, 0, len(catalogs))
		for lang := range catalogs {
			locales = append(locales, lang)
		}
		slices.Sort(locales)
		for _, lang := range locales {
			words, err := editorWords(catalogs[lang])
			if err != nil {
				return fmt.Errorf("mod.json strings.%s: %w", lang, err)
			}
			code := lang
			if normal, ok := editorLanguageCodes[strings.ToLower(lang)]; ok {
				code = normal
			}
			if !editorLocale.MatchString(code) || !filepath.IsLocal(code+".json") {
				return fmt.Errorf("cannot identify editor language %q in mod.json", lang)
			}
			target := filepath.Join(dir, "editor", "strings", code+".json")
			current, err := p.plannedAuthorFile(target)
			if err != nil {
				return err
			}
			old, err := os.ReadFile(current)
			if err == nil {
				existing, err := editorWords(old)
				if err != nil {
					return fmt.Errorf("%s: %w", target, err)
				}
				added := false
				for key, value := range words {
					previous, ok := existing[key]
					if ok && previous != value {
						return fmt.Errorf("different translations for %q in mod.json and %s", key, target)
					}
					added = added || !ok
					existing[key] = value
				}
				if !added {
					continue
				}
				words = existing
			} else if !errors.Is(err, fs.ErrNotExist) {
				return err
			}
			pretty, err := json.MarshalIndent(words, "", "  ")
			if err != nil {
				return err
			}
			if err = p.updateAuthorFile(target, current, append(pretty, '\n')); err != nil {
				return err
			}
		}
		start, end := field.start, field.end
		if i == 0 && len(fields) > 1 {
			end += bytes.IndexByte(body[end:fields[1].value], ',') + 1
		}
		prefix := len(raw) - len(body)
		next := append(slices.Clone(raw[:prefix+start]), body[end:]...)
		return p.updateAuthorFile(path, source, next)
	}
	return nil
}

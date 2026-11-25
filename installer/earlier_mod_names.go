package main

import (
	"crypto/sha256"
	"fmt"
	"path"
	"regexp"
	"strings"
)

// A release id has one namespace across its installed copies, maps and exports.
func earlierSalt(id string) string {
	sum := sha256.Sum256([]byte("snapmap-plus/earlier-id/" + strings.ToLower(id)))
	const chars = "abcdefghijklmnopqrstuvwxyz0123456789"
	mod := make([]byte, 8)
	mod[0] = chars[int(sum[0])%26]
	for i := 1; i < len(mod); i++ {
		mod[i] = chars[int(sum[i])%len(chars)]
	}
	if strings.Contains(string(mod), "initial") {
		mod[1] = 'z'
	}
	return string(mod)
}

var earlierDecls = map[string]string{"material": "materials", "fx": "effects", "env": "environments", "particle": "particles", "entitydef": "entities", "sound": "sounds", "md6def": "meshes", "renderprog": "programs"}

// Only this mod's namespace is rewritten. The engine folded hyphens in cooked asset paths.
func earlierOwnSpelling(id, name string) string {
	if !strings.Contains(id, "-") {
		return name
	}
	cooked := "mods/" + strings.ReplaceAll(id, "-", "_") + "/"
	raw := "mods/" + id + "/"
	if strings.HasPrefix(name, cooked) {
		return raw + name[len(cooked):]
	}
	if before, rest, found := strings.Cut(name, "/"+cooked); found {
		return before + "/" + raw + rest
	}
	return name
}

func earlierAuthorPath(id, request string) (string, error) {
	r := earlierOwnSpelling(id, strings.ToLower(request))
	own := "mods/" + strings.ToLower(id) + "/"
	for _, prefix := range []string{"generated/maps/modules/", "maps/modules/"} {
		if rest, ok := strings.CutPrefix(r, prefix+own); ok {
			return "map-modules/" + rest, nil
		}
	}
	if rest, ok := strings.CutPrefix(r, "maps/modules/palettes/"); ok {
		pal, tail, ok := strings.Cut(rest, "/")
		if inner, owned := strings.CutPrefix(tail, own); ok && owned {
			return "map-modules/palettes/" + pal + "/" + inner, nil
		}
	}
	if rest, ok := strings.CutPrefix(r, own+"scripts/"); ok {
		return "scripts/" + rest, nil
	}
	if rest, ok := strings.CutPrefix(r, "sound/soundbanks/pc/"+id+"_"); ok && !strings.Contains(rest, "/") && strings.HasSuffix(rest, ".bnk") {
		return "sounds/" + rest, nil
	}
	if rest, ok := strings.CutPrefix(r, "generated/decls/"); ok {
		kind, rest, _ := strings.Cut(rest, "/")
		if inner, ok := strings.CutPrefix(rest, own); ok && strings.HasSuffix(inner, ".decl") {
			folder := earlierDecls[kind]
			if folder == "" {
				folder = "decls/" + kind
			}
			if kind == "sound" {
				dir, leaf := path.Split(inner)
				stem, ok := strings.CutPrefix(leaf, id+"_")
				if !ok {
					return "decls/sound/" + inner, nil
				}
				inner = dir + stem
			}
			return folder + "/" + inner, nil
		}
	}
	for _, kind := range earlierAssetTypes {
		if rest, ok := strings.CutPrefix(r, kind.root+own); ok && path.Ext(r) == kind.extension {
			return kind.folder + "/" + rest, nil
		}
	}
	// Earlier image recipes could put the namespace after the texture directory.
	if image, ok := strings.CutPrefix(r, "generated/image/"); ok && path.Ext(image) == ".bimage" {
		if prefix, rest, found := strings.Cut(image, "/"+own); found {
			return "images/" + prefix + "/" + rest, nil
		}
	}
	if strings.Contains(r, "/mods/") || strings.HasPrefix(r, "mods/") {
		return "", fmt.Errorf("unrecognized mod asset path %s", request)
	}
	return "native/" + r, nil
}

var earlierNameToken = regexp.MustCompile(`(?:#str_|native:)?[A-Za-z0-9_.$/+@~-]+`)
var earlierKey = regexp.MustCompile(`^[a-zA-Z0-9_]+$`)
var earlierAuthorPrefix = regexp.MustCompile(`^(?:materials|effects|environments|particles|entities|images|animations|collision|movies|scripts|map-modules|virtual-textures|programs|meshes|sounds)/.+|^decls/[^/]+/.+`)
var earlierLookKey = regexp.MustCompile(`(?i)^(\s*\{\s*)look(\s+(?:layered|particle)\b)`)

type earlierModNames struct {
	id, salt string
	files    map[string]string
	sounds   map[string]string
	keys     map[string]string
	images   map[string]string
}

func (n earlierModNames) add(request, author string) {
	if image, ok := strings.CutPrefix(earlierOwnSpelling(n.id, strings.ToLower(request)), "generated/image/"); ok && strings.HasPrefix(author, "images/") {
		n.images[strings.TrimSuffix(image, ".bimage")] = strings.TrimSuffix(author, ".bimage")
	}
	top, stem, _ := strings.Cut(author, "/")
	if top == "decls" {
		kind, inner, _ := strings.Cut(stem, "/")
		top += "/" + kind
		stem = inner
	}
	ext := path.Ext(stem)
	if top == "decls/sound" && ext == ".decl" {
		leaf := strings.TrimSuffix(stem, ext)
		n.sounds[leaf] = top + "/" + leaf
		return
	}
	if top == "sounds" && ext == ".decl" {
		stem = strings.TrimSuffix(stem, ext)
		dir, leaf := path.Split(stem)
		n.sounds[dir+n.id+"_"+leaf] = "sounds/" + stem
		return
	}
	if top == "native" || top == "atlases" || top == "scripts" || top == "map-modules" || strings.HasPrefix(author, "programs/permutations/") {
		return
	}
	if ext == ".decl" {
		stem = strings.TrimSuffix(stem, ext)
		priority := func(f string) int {
			for i, p := range []string{"entities", "materials", "effects", "particles", "sounds", "meshes", "programs"} {
				if f == p {
					return i
				}
			}
			return 10
		}
		if prev, exists := n.files[stem]; !exists || priority(top) < priority(prev) {
			n.files[stem] = top
		}
	} else if top == "meshes" || top == "images" {
		n.files[stem] = top
	}
}

func (n earlierModNames) authorText(text string) string {
	own := "mods/" + n.id + "/"
	return earlierNameToken.ReplaceAllStringFunc(text, func(token string) string {
		if strings.HasPrefix(token, "#str_") {
			if key, ok := n.keys[token[5:]]; ok {
				return "#str_" + key
			}
			return token
		}
		low := earlierOwnSpelling(n.id, strings.ToLower(token))
		if image := n.images[strings.TrimSuffix(low, path.Ext(low))]; image != "" {
			return image + path.Ext(low)
		}
		for _, r := range []struct{ prefix, author string }{{"md6/" + own, "animations/"}, {"swf/" + own, "movies/"}, {"maps/modules/" + own, "map-modules/"}, {own + "scripts/", "scripts/"}} {
			if strings.HasPrefix(low, r.prefix) {
				rest := token[len(r.prefix):]
				if r.author == "animations/" && (strings.HasSuffix(low, ".md6skl") || strings.HasSuffix(low, ".md6mesh")) {
					return "meshes/" + rest
				}
				return r.author + rest
			}
		}
		if strings.HasPrefix(low, own) && len(token) > len(own) {
			rest := token[len(own):]
			key := strings.ToLower(rest)
			if sound, ok := n.sounds[key]; ok {
				return sound
			}
			folder := n.files[key]
			if folder == "" {
				switch path.Ext(key) {
				case ".tga", ".png", ".dds", ".jpg", ".jpeg", ".bmp", ".bimage", ".exr", ".tif", ".tiff":
					folder = "images"
				case ".lwo", ".ase", ".obj", ".fbx", ".ma", ".md6", ".bmodel", ".md6mesh", ".md6skl":
					folder = "meshes"
				default:
					folder = "entities"
				}
			}
			return folder + "/" + rest
		}
		if earlierAuthorPrefix.MatchString(low) || strings.HasPrefix(low, "native:") {
			return "native:" + token
		}
		return token
	})
}

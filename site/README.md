# Website

## Purpose

Contains the Jekyll source for the public Snapmap+ website, player guide, release history and Community pages.

## Contents

- `index.html`, `changelog.html` and `community*.html` define the main pages.
- `snapmap-plus-guide.md` and `privacy.md` contain player documentation.
- `_layouts/` and `_includes/` share page framing and navigation.
- `assets/` contains scripts and styles; `images/` contains screenshots.
- `_config.yml` configures Jekyll; `sitemap.xml` lists public pages.

## Working here

The [Pages workflow](../.github/workflows/pages.yml) renders `CHANGELOG.md` into temporary `_data/changelog.json`, then builds this directory with Jekyll. Do not maintain a second release history here. Community pages call the separate [Community service](../community/README.md).

Use a local Jekyll preview to verify Liquid includes and Markdown pages. Check narrow layouts and keyboard navigation when changing presentation. Directory READMEs are contributor documentation and are excluded from the published site.

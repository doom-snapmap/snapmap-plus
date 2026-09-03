#!/usr/bin/env python3
"""Parse CHANGELOG.md -- the single source of truth for user-facing release notes.

Standard library only. This runs in three workflows with no dependency install:
release.yml (windows-2022, `python`), pages.yml and ci.yml (ubuntu-latest,
`python3`). Adding a third-party import breaks the release.

The section-header grammar every consumer parses:

    ## <tag> -- <YYYY-MM-DD> (<channel>)

Read liberally, emit strictly: the separator is accepted as `--` or either
Unicode dash an editor may substitute, and only ever written as ASCII `--`.
"""

import argparse
import datetime
import json
import re
import sys

TAG = r"v\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?"
DASH = r"(?:--|—|–)"

HEADER_RE = re.compile(
    r"^## +(?P<tag>" + TAG + r") +" + DASH + r" +"
    r"(?P<date>\d{4}-\d{2}-\d{2}) +\((?P<channel>beta|stable)\)\s*$"
)

# A section ends at the next "## " -- WITH the trailing space. A bare "^##"
# also matches "### New" and would truncate every body at its first group.
SECTION_PREFIX = "## "

GROUP_RE = re.compile(r"^### +(?P<name>New|Improved|Fixed)\s*$")
BULLET_RE = re.compile(r"^- +(?P<text>.*\S)\s*$")
HEADLINE_RE = re.compile(r"^\*\*(?P<text>.+?)\*\*\s*$")
COLLAPSED_RE = re.compile(r"^_(?P<text>.+?)_\s*$")

GROUP_KEYS = {"New": "added", "Improved": "improved", "Fixed": "fixed"}


def _split_sections(text):
    """Yield (header_line, body_lines) for every '## ' block, in file order."""
    lines = text.splitlines()
    starts = [i for i, ln in enumerate(lines) if ln.startswith(SECTION_PREFIX)]
    for n, start in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        yield lines[start], lines[start + 1:end]


def _parse_body(body_lines):
    """Pull the structured fields out of a section body."""
    out = {"headline": "", "summary": "", "added": [], "improved": [],
           "fixed": [], "collapsed": ""}
    group = None
    summary_parts = []
    for raw in body_lines:
        line = raw.rstrip()
        if not line.strip():
            continue
        m = GROUP_RE.match(line)
        if m:
            group = GROUP_KEYS[m.group("name")]
            continue
        m = BULLET_RE.match(line)
        if m and group:
            out[group].append(m.group("text"))
            continue
        m = HEADLINE_RE.match(line)
        if m and not out["headline"]:
            out["headline"] = m.group("text")
            continue
        m = COLLAPSED_RE.match(line)
        if m:
            out["collapsed"] = m.group("text")
            continue
        if group is None:
            summary_parts.append(line.strip())
    out["summary"] = " ".join(summary_parts)
    return out


def parse(text):
    """Return every well-formed section, in file order (newest first)."""
    sections = []
    for header, body_lines in _split_sections(text):
        m = HEADER_RE.match(header)
        if not m:
            continue
        section = {
            "tag": m.group("tag"),
            "date": m.group("date"),
            "channel": m.group("channel"),
            "body": "\n".join(body_lines).strip("\n"),
        }
        section.update(_parse_body(body_lines))
        sections.append(section)
    return sections


def find(sections, tag):
    """Exact tag match. A substring search would resolve v0.2.1 to
    v0.2.1-beta.6, which is precisely the first stable release."""
    for section in sections:
        if section["tag"] == tag:
            return section
    return None


def lint(text):
    """Return human-readable problems; empty list means the file is valid."""
    problems = []
    seen = set()
    for header, _ in _split_sections(text):
        m = HEADER_RE.match(header)
        if not m:
            problems.append(
                "malformed section header (expected "
                "'## <tag> -- <YYYY-MM-DD> (beta|stable)'): " + header.strip()
            )
            continue
        tag, date = m.group("tag"), m.group("date")
        if tag in seen:
            problems.append("duplicate section for " + tag)
        seen.add(tag)
        try:
            datetime.date.fromisoformat(date)
        except ValueError:
            problems.append("not a real date in " + tag + ": " + date)
        expected = "beta" if "-" in tag else "stable"
        if m.group("channel") != expected:
            problems.append(
                tag + " is marked (" + m.group("channel") + ") but its tag "
                "means it is " + expected
            )
    return problems


def _write(path, content):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(content)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", default="CHANGELOG.md")
    ap.add_argument("--out")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--section", metavar="TAG")
    mode.add_argument("--json", action="store_true")
    mode.add_argument("--has", metavar="TAG")
    mode.add_argument("--lint", action="store_true")
    args = ap.parse_args(argv)

    if (args.section or args.json) and not args.out:
        ap.error("--section and --json both require --out")

    try:
        with open(args.file, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        print("::error::cannot read " + args.file + ": " + str(exc), file=sys.stderr)
        return 1

    if args.lint:
        problems = lint(text)
        for problem in problems:
            print("::error::" + args.file + ": " + problem, file=sys.stderr)
        return 1 if problems else 0

    sections = parse(text)

    if args.has:
        return 0 if find(sections, args.has) else 1

    if args.section:
        found = find(sections, args.section)
        if not found:
            print(
                "::error::no section for " + args.section + " in " + args.file
                + " -- run the prepare-release workflow for this version first",
                file=sys.stderr,
            )
            return 1
        _write(args.out, found["body"] + "\n")
        return 0

    payload = [
        {k: v for k, v in section.items() if k != "body"} for section in sections
    ]
    _write(args.out, json.dumps(payload, indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

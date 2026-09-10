#!/usr/bin/env python3
"""Synchronize published release bodies with reviewed CHANGELOG.md entries.
Requires an authenticated gh CLI. Default is a dry run; --apply writes changes
and --tag selects one release. Save current bodies before writing so changes
can be reversed with gh release edit --notes-file."""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changelog  # noqa: E402


def gh(*args, check=True):
    return subprocess.run(["gh", *args], capture_output=True, text=True, check=check)


def published_tags():
    """Return tags with published releases; tags without a release body are not drift."""
    out = gh("release", "list", "--limit", "200", "--json", "tagName").stdout
    return {r["tagName"] for r in json.loads(out)}


def current_body(tag):
    return gh("release", "view", tag, "--json", "body").stdout and json.loads(
        gh("release", "view", tag, "--json", "body").stdout
    )["body"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default="CHANGELOG.md")
    ap.add_argument("--tag", help="sync only this tag (default: every entry)")
    ap.add_argument("--apply", action="store_true",
                    help="actually rewrite the release bodies")
    ap.add_argument("--backup", default="release-bodies-backup.json")
    args = ap.parse_args(argv)

    with open(args.file, encoding="utf-8") as fh:
        sections = changelog.parse(fh.read())
    if args.tag:
        sections = [s for s in sections if s["tag"] == args.tag]
        if not sections:
            print("no CHANGELOG.md entry for " + args.tag, file=sys.stderr)
            return 1

    try:
        live = published_tags()
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print("cannot reach GitHub via `gh`: %s" % exc, file=sys.stderr)
        return 1

    drifted, backup = [], {}
    for section in sections:
        tag = section["tag"]
        if tag not in live:
            print("  skip   %-18s no published release for this tag" % tag)
            continue
        body = current_body(tag)
        backup[tag] = body
        if (body or "").strip() == section["body"].strip():
            print("  ok     %-18s already matches" % tag)
            continue
        drifted.append((tag, section["body"]))
        print("  DRIFT  %-18s %6d bytes published -> %4d bytes in CHANGELOG.md"
              % (tag, len(body or ""), len(section["body"])))

    if not drifted:
        print("\nEvery published release already matches CHANGELOG.md.")
        return 0

    if not args.apply:
        print("\n%d release(s) would be rewritten. Re-run with --apply to do it."
              % len(drifted))
        return 0

    with open(args.backup, "w", encoding="utf-8") as fh:
        json.dump(backup, fh, indent=2, ensure_ascii=False)
    print("\nSaved the current bodies to %s" % args.backup)

    for tag, body in drifted:
        path = ".release-notes-%s.md" % tag
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body + "\n")
        try:
            gh("release", "edit", tag, "--notes-file", path)
            print("  updated %s" % tag)
        finally:
            os.remove(path)
    print("\nRewrote %d release(s)." % len(drifted))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Draft one CHANGELOG.md entry in the credential-bearing, read-only workflow job.
A separate job opens the pull request. validate() enforces content and length
limits before rendering; contributor-controlled text must not introduce Markdown
structure. Import optional API dependencies inside draft() so offline tests work."""

import argparse
import dataclasses
import datetime
import os
import re
import subprocess
import sys

MODEL = "claude-opus-5"
MAX_TOKENS = 16000
MAX_BODY_CHARS = 800      # one commit body; git permits megabyte messages
MAX_PROMPT_CHARS = 60000  # the assembled prompt
MAX_INPUT_TOKENS = 40000  # counted before spending

# Include changed feature documentation so drafts can describe how users reach a
# feature.
DOC_PATHS = ["docs/capabilities.md", "docs/fidelity.md", "docs/webview-ui.md",
             "README.md"]
MAX_DOC_CHARS = 24000

# Require interface-location words to appear in the inputs. This catches invented
# UI surfaces, but it does not establish that every drafted claim is true.
UI_NOUNS = ("tab", "panel", "button", "menu", "dialog", "checkbox", "slider",
            "toolbar", "sidebar", "window", "dropdown", "wizard", "toggle")

MAX_HEADLINE = 60
MAX_SUMMARY = 320
MAX_ITEMS = 6
MAX_SKELETON_SUBJECTS = 20

# Canonical user-facing commit filter; keep contributing.md aligned.
INTERNAL_PREFIX_RE = re.compile(
    r"^(ci|chore|scrub|docs|tests?|release|refactor|build|style|meta): ", re.I
)
TRAILER_RE = re.compile(
    r"^co-authored-by:|^signed-off-by:|generated with claude"
    r"|claude\.com/claude-code|noreply@anthropic",
    re.I,
)
SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")

# Rejected outright in any model-authored string. "[" and "]" cover markdown
# links; "<" and ">" cover inline HTML; the backtick covers code spans.
FORBIDDEN = ("\n", "\r", "[", "]", "<", ">", "`")

SYSTEM = """\
You write the release notes for Snapmap+, a free open-source mod for DOOM (2016) \
that unlocks its SnapMap editor. You are given the commits in one release. Produce \
the entry a player reads.

Rules:
- At most 6 named bullets in TOTAL across New, Improved and Fixed, one sentence \
each, present tense.
- Name a fix only if a user could have hit it and remembered it. Everything a user \
could never have noticed is counted in collapsed_count and never described.
- Plain language. No symbol names, addresses, file paths, test counts, commit \
subjects copied verbatim, or internal jargon.
- headline: at most 60 characters, the one thing this release is about.
- summary: at most 320 characters, one to three sentences on what changed and what \
it means for someone using Snapmap+.
- sources: the short commit hashes backing your named bullets, lowercase hex only, \
for maintainer review.
- Never use newlines, square brackets, angle brackets, or backticks inside any \
string. Text containing them is rejected and your draft is discarded.

Grounding -- this is what makes the entry TRUE rather than merely plausible:
- You are given the commits AND the diff of the user-facing documentation. A behaviour change is required to update those docs in the same pull request, so the docs diff is the account of what a user actually sees.
- Say only what those sources support. If they tell you a capability changed but not how it is reached, describe the capability and stop.
- NEVER invent where something lives. Do not name a tab, panel, button, menu, dialog, checkbox, window or any other place in the interface unless that exact word appears in the sources. A draft naming a UI surface the sources never mention is rejected and discarded.
- Prefer the docs diff's own wording for what a feature is and where it lives.
"""


@dataclasses.dataclass
class ChangelogDraft:
    """The drafted entry. A plain dataclass on purpose -- the renderer and its
    tests must work without pydantic installed."""

    headline: str
    summary: str
    added: list
    improved: list
    fixed: list
    collapsed_count: int
    sources: list


class DraftRejected(Exception):
    """The model's output is not safe or not short enough to render."""


def _git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True,
                          check=True).stdout.strip()


def _one_line(text, limit=200):
    return " ".join(text.split())[:limit]


def channel_for(tag):
    return "beta" if "-" in tag else "stable"


def is_user_facing(subject):
    return not INTERNAL_PREFIX_RE.match(subject)


def scrub_body(body):
    kept = [ln for ln in body.splitlines() if not TRAILER_RE.search(ln.strip())]
    return "\n".join(kept).strip()[:MAX_BODY_CHARS]


def _reject_forbidden(where, value):
    for ch in FORBIDDEN:
        if ch in value:
            raise DraftRejected(
                "%s contains %r, which could forge markup or a section header"
                % (where, ch)
            )


def validate(draft, min_collapsed=0):
    """Raise DraftRejected unless every field is safe and short enough."""
    groups = {"added": draft.added, "improved": draft.improved, "fixed": draft.fixed}

    for name, value in [("headline", draft.headline), ("summary", draft.summary)]:
        _reject_forbidden(name, value)
    for name, items in groups.items():
        for item in items:
            _reject_forbidden(name + " item", item)

    if not draft.headline.strip():
        raise DraftRejected("headline is empty")
    if not draft.summary.strip():
        raise DraftRejected("summary is empty")
    if len(draft.headline) > MAX_HEADLINE:
        raise DraftRejected("headline is %d characters (max %d)"
                            % (len(draft.headline), MAX_HEADLINE))
    if len(draft.summary) > MAX_SUMMARY:
        raise DraftRejected("summary is %d characters (max %d)"
                            % (len(draft.summary), MAX_SUMMARY))
    # Enforce the item limit across all groups combined.
    total = sum(len(items) for items in groups.values())
    if total > MAX_ITEMS:
        raise DraftRejected("%d named bullets across New, Improved and Fixed "
                            "(max %d in total)" % (total, MAX_ITEMS))
    for name, items in groups.items():
        for item in items:
            if not item.strip():
                raise DraftRejected(name + " contains an empty item")
    if draft.collapsed_count < 0:
        raise DraftRejected("collapsed_count is negative")
    # The collapsed count includes both omitted inputs and commits the draft leaves
    # unnamed.
    if draft.collapsed_count < min_collapsed:
        raise DraftRejected(
            "collapsed_count is %d but at least %d commits are not described"
            % (draft.collapsed_count, min_collapsed))

    # sources reaches the pull-request description, and reaches git as argv.
    for sha in draft.sources:
        _reject_forbidden("sources item", sha)
        if not SHA_RE.match(sha):
            raise DraftRejected("sources contains %r, which is not a commit hash"
                                % sha)


def render(tag, date, draft):
    """Render the full section. Never called before validate()."""
    lines = ["## %s -- %s (%s)" % (tag, date, channel_for(tag)), "",
             "**%s**" % draft.headline, "", draft.summary, ""]
    for title, items in [("New", draft.added), ("Improved", draft.improved),
                         ("Fixed", draft.fixed)]:
        if not items:
            continue
        lines.append("### " + title)
        lines.extend("- " + item for item in items)
        lines.append("")
    if draft.collapsed_count > 0:
        if draft.collapsed_count == 1:
            lines.append("_Plus 1 smaller fix and internal change._")
        else:
            lines.append("_Plus %d smaller fixes and internal changes._"
                         % draft.collapsed_count)
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def render_skeleton(tag, date, subjects, reason):
    """A valid, obviously-unfinished section for when drafting fails.
    A drafting failure must never block a release."""
    safe = []
    for subject in subjects[:MAX_SKELETON_SUBJECTS]:
        safe.append(_one_line(subject.replace("[", "(").replace("]", ")"), 120))
    lines = ["## %s -- %s (%s)" % (tag, date, channel_for(tag)), "",
             "**NEEDS WRITING**", "",
             "Automatic drafting did not run (%s), so this entry is the raw "
             "commit list and must be rewritten before merging." % _one_line(reason),
             ""]
    if safe:
        lines.append("### New")
        lines.extend("- " + s for s in safe)
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def render_sources(draft):
    """The bullet-to-SHA table, for the pull-request description ONLY.
    It never enters CHANGELOG.md. Every sha is hex-validated by validate()."""
    if not draft.sources:
        return "_The drafter cited no commits._\n"
    lines = ["Commits backing the named bullets, for spot-checking:", "",
             "| Commit | Subject |", "| --- | --- |"]
    for sha in draft.sources[:40]:
        try:
            subject = _git("show", "-s", "--format=%s", sha)
        except subprocess.CalledProcessError:
            subject = "(unknown commit)"
        lines.append("| `%s` | %s |" % (sha, subject.replace("|", "\\|")))
    return "\n".join(lines) + "\n"


def collect(base):
    """Return (prompt_block, user_facing_subjects, omitted_count) for base..HEAD.
    The release tag does not exist yet. Count internal commits and prompt-cap
    omissions."""
    rev_range = "HEAD" if not base else base + "..HEAD"
    shas = _git("rev-list", "--no-merges", rev_range).split()

    blocks, subjects, omitted, used = [], [], 0, 0
    for sha in shas:
        subject = _git("show", "-s", "--format=%s", sha)
        if not is_user_facing(subject):
            omitted += 1
            continue
        body = scrub_body(_git("show", "-s", "--format=%b", sha))
        block = "commit %s\n%s\n%s" % (sha[:7], subject, body)
        if used + len(block) > MAX_PROMPT_CHARS:
            omitted += 1          # user-facing, but did not fit
            continue
        used += len(block)
        blocks.append(block)
        subjects.append(subject)
    return "\n\n".join(blocks), subjects, omitted


def collect_docs(base):
    """Return the bounded user-facing documentation diff for this release range.
    Use changes rather than complete files to focus the draft on this release."""
    if not base:
        return ""
    try:
        out = _git("diff", "--unified=1", base + "..HEAD", "--", *DOC_PATHS)
    except subprocess.CalledProcessError:
        return ""
    keep = [ln for ln in out.splitlines()
            if ln.startswith(("diff --git", "+", "-"))]
    return "\n".join(keep)[:MAX_DOC_CHARS]


def _corpus(commits, docs):
    """Everything the model was actually shown, lowercased, for grounding."""
    return (commits + "\n" + docs).lower()


def check_grounded(draft, corpus):
    """Reject interface-location words absent from the supplied sources.
    This vocabulary check cannot verify the truth of a sentence."""
    for name, items in (("added", draft.added), ("improved", draft.improved),
                        ("fixed", draft.fixed)):
        for item in items:
            low = item.lower()
            for noun in UI_NOUNS:
                if re.search(r"\b%s\b" % noun, low) and noun not in corpus:
                    raise DraftRejected(
                        "%s item names a %r that appears nowhere in the commits or "
                        "the docs diff -- the drafter invented where the feature "
                        "lives: %s" % (name, noun, _one_line(item, 120)))


def previous_section(path):
    """The most recent entry, as a style anchor."""
    try:
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return ""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import changelog
    sections = changelog.parse(text)
    return sections[0]["body"] if sections else ""


def draft(commits, style, omitted_count, docs=""):
    """The one API call. pydantic and anthropic are imported here, not above."""
    import anthropic
    from pydantic import BaseModel

    class ChangelogDraftSchema(BaseModel):
        headline: str
        summary: str
        added: list[str]
        improved: list[str]
        fixed: list[str]
        collapsed_count: int
        sources: list[str]

    client = anthropic.Anthropic()
    prompt = (
        "Commits in this release:\n\n" + commits
        + "\n\nCommits not shown -- internal changes, or trimmed for length. "
        "Count these in collapsed_count and do not describe them: %d\n"
        % omitted_count
    )
    if docs:
        prompt += (
            "\nThe diff of the user-facing documentation over the same range. "
            "This is what a user can now do and where; prefer its wording, and do "
            "not name any part of the interface it does not name:\n\n"
            + docs + "\n")
    if style:
        prompt += ("\nThe previous release's entry, for voice and length only "
                   "-- do not repeat its content:\n\n" + style + "\n")

    counted = client.messages.count_tokens(
        model=MODEL, system=SYSTEM, messages=[{"role": "user", "content": prompt}]
    )
    if counted.input_tokens > MAX_INPUT_TOKENS:
        raise DraftRejected("input is %d tokens (max %d)"
                            % (counted.input_tokens, MAX_INPUT_TOKENS))

    response = client.messages.parse(
        model=MODEL,
        max_tokens=MAX_TOKENS,
        thinking={"type": "adaptive"},
        output_config={"effort": "medium"},
        system=SYSTEM,
        messages=[{"role": "user", "content": prompt}],
        output_format=ChangelogDraftSchema,
    )
    if response.stop_reason == "refusal":
        raise DraftRejected("the model declined this request")
    if response.stop_reason == "max_tokens":
        raise DraftRejected("the response hit the token cap and is incomplete")
    if response.parsed_output is None:
        raise DraftRejected("no structured output in the response")
    return ChangelogDraft(**response.parsed_output.model_dump())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--base", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sources-out", required=True)
    ap.add_argument("--changelog", default="CHANGELOG.md")
    args = ap.parse_args(argv)

    date = datetime.date.today().isoformat()
    subjects = []
    try:
        # collect() is inside the try on purpose: a git failure must degrade to
        # a skeleton like every other failure, not kill the workflow.
        commits, subjects, omitted = collect(args.base)
        docs = collect_docs(args.base)
        parsed = draft(commits, previous_section(args.changelog), omitted, docs)
        validate(parsed, omitted)
        check_grounded(parsed, _corpus(commits, docs))
        section, sources = render(args.tag, date, parsed), render_sources(parsed)
    except Exception as exc:            # noqa: BLE001 -- every failure degrades
        reason = _one_line("%s: %s" % (type(exc).__name__, exc))
        print("::warning::drafting failed, falling back to a skeleton -- " + reason,
              file=sys.stderr)
        section = render_skeleton(args.tag, date, subjects, reason)
        sources = "_Drafting failed; there are no cited commits._\n"

    for path, content in [(args.out, section), (args.sources_out, sources)]:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())

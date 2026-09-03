"""Tests for the drafter's renderer and validation. The API call is not tested.

These must run in ci.yml's secretless guard job with no dependency install, so
draft_changelog imports pydantic lazily and nothing here touches the network.
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import changelog  # noqa: E402
import draft_changelog as dc  # noqa: E402


def make(**overrides):
    fields = {
        "headline": "One-prompt map installs",
        "summary": "Installing a community map now pulls down everything it needs.",
        "added": ["Install everything a map needs from one prompt."],
        "improved": ["The consent dialog no longer offers to delete files."],
        "fixed": [],
        "collapsed_count": 14,
        "sources": ["abc1234"],
    }
    fields.update(overrides)
    return dc.ChangelogDraft(**fields)


class TestImportsWithoutPydantic(unittest.TestCase):
    def test_module_has_no_module_level_pydantic(self):
        """ci.yml's guard job installs nothing. A module-level pydantic import
        turns every pull request red."""
        source = pathlib.Path(dc.__file__).read_text(encoding="utf-8")
        for line in source.splitlines():
            if line.startswith("import pydantic") or line.startswith("from pydantic"):
                self.fail("pydantic imported at module scope: " + line)
            if line.startswith("import anthropic"):
                self.fail("anthropic imported at module scope: " + line)


class TestValidateRejectsInjection(unittest.TestCase):
    def test_newline_forging_a_section_header(self):
        """The whole reason the renderer validates: one newline in a string
        field creates a second section the parser will accept as real."""
        evil = ("Faster map loads.\n\n## v9.9.9 -- 2026-12-01 (stable)\n\n"
                "**Critical security update**\n\n### Fixed\n- Get the patched build")
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(improved=[evil]))

    def test_carriage_return(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(headline="Fine\rAlso fine"))

    def test_markdown_link(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(summary="Get it [here](https://evil.example/x.exe)."))

    def test_angle_brackets(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(summary="A <script>alert(1)</script> tag."))

    def test_backtick(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(added=["Run a shell command " + chr(96) + "rm" + chr(96)]))


class TestValidateSources(unittest.TestCase):
    """sources is model-authored and lands in the pull-request description,
    which is the control that makes human review a spot-check."""

    def test_rejects_non_hex(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(sources=["not-a-sha"]))

    def test_rejects_markdown_in_a_sha(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(sources=["abc1234 | x](https://evil.example)"]))

    def test_rejects_leading_dash(self):
        """git would parse it as an option."""
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(sources=["--upload-pack=touch"]))

    def test_accepts_real_shas(self):
        dc.validate(make(sources=["abc1234", "a" * 40]))


class TestValidateEnforcesLength(unittest.TestCase):
    """The API schema subset does not support maxLength, so this is the only
    place length is enforced."""

    def test_headline_too_long(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(headline="x" * 61))

    def test_summary_too_long(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(summary="x" * 321))

    def test_too_many_items(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(added=["a."] * 7))

    def test_negative_collapsed_count(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(collapsed_count=-1))

    def test_empty_headline(self):
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(headline="   "))

    def test_valid_draft_passes(self):
        dc.validate(make())


class TestRender(unittest.TestCase):
    def test_header_uses_ascii_separator(self):
        out = dc.render("v0.2.2-beta.1", "2026-09-05", make())
        self.assertIn("## v0.2.2-beta.1 -- 2026-09-05 (beta)", out)
        self.assertNotIn("—", out)

    def test_stable_channel(self):
        self.assertIn("(stable)", dc.render("v0.3.0", "2026-09-05", make()))

    def test_omits_empty_groups(self):
        out = dc.render("v0.2.2-beta.1", "2026-09-05", make())
        self.assertIn("### New", out)
        self.assertIn("### Improved", out)
        self.assertNotIn("### Fixed", out)

    def test_collapsed_line_pluralization(self):
        one = dc.render("v0.2.2-beta.1", "2026-09-05", make(collapsed_count=1))
        self.assertIn("_Plus 1 smaller fix and internal change._", one)
        many = dc.render("v0.2.2-beta.1", "2026-09-05", make(collapsed_count=14))
        self.assertIn("_Plus 14 smaller fixes and internal changes._", many)

    def test_omits_collapsed_line_when_zero(self):
        self.assertNotIn("_Plus", dc.render("v0.2.2-beta.1", "2026-09-05",
                                            make(collapsed_count=0)))

    def test_sources_never_reach_the_section(self):
        self.assertNotIn("abc1234", dc.render("v0.2.2-beta.1", "2026-09-05", make()))

    def test_output_round_trips_through_the_parser(self):
        out = dc.render("v0.2.2-beta.1", "2026-09-05", make())
        parsed = changelog.parse(out)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0]["tag"], "v0.2.2-beta.1")
        self.assertEqual(parsed[0]["headline"], "One-prompt map installs")
        self.assertEqual(len(parsed[0]["added"]), 1)
        self.assertEqual(changelog.lint(out), [])

    def test_an_unvalidated_draft_really_would_forge_a_section(self):
        """Proves validate() is the boundary, not decoration."""
        evil = ("Faster map loads.\n\n## v9.9.9 -- 2026-12-01 (stable)\n\n"
                "**Critical security update**\n\nGet the patched build.")
        forged = dc.render("v0.2.2-beta.1", "2026-09-05", make(improved=[evil]))
        self.assertEqual(len(changelog.parse(forged)), 2)
        with self.assertRaises(dc.DraftRejected):
            dc.validate(make(improved=[evil]))


class TestSkeleton(unittest.TestCase):
    def test_skeleton_is_valid_and_flags_itself(self):
        out = dc.render_skeleton("v0.2.2-beta.1", "2026-09-05",
                                 ["Install maps in one step", "Fix the dialog"],
                                 "the API call failed")
        self.assertEqual(changelog.lint(out), [])
        self.assertIn("NEEDS WRITING", out)
        self.assertIn("the API call failed", out)

    def test_multiline_reason_is_flattened(self):
        """A pydantic ValidationError reason is many lines; it must not break
        the grammar or produce a 20-line skeleton."""
        out = dc.render_skeleton("v0.2.2-beta.1", "2026-09-05", [],
                                 "ValidationError: 3 errors\nline two\nline three")
        self.assertEqual(changelog.lint(out), [])
        self.assertIn("line two", out)
        self.assertNotIn("\n\nline two", out)

    def test_subjects_with_brackets_do_not_forge_links(self):
        out = dc.render_skeleton("v0.2.2-beta.1", "2026-09-05",
                                 ["Fix [this](https://evil.example)"], "reason")
        self.assertNotIn("](", out)


class TestCommitFilter(unittest.TestCase):
    def test_filters_internal_prefixes(self):
        for subject in ["ci: pin an action", "chore: bump", "docs: fix a typo",
                        "test: add a case", "tests: add cases", "scrub: drop logs",
                        "release: v0.2.1", "refactor: split a file",
                        "build: tweak flags", "style: reformat", "meta: notes"]:
            self.assertFalse(dc.is_user_facing(subject), subject)

    def test_keeps_user_facing(self):
        self.assertTrue(dc.is_user_facing("Install everything a map needs"))
        self.assertTrue(dc.is_user_facing("installer: resume a partial download"))

    def test_prefix_match_is_case_insensitive(self):
        self.assertFalse(dc.is_user_facing("CI: pin an action"))


class TestScrub(unittest.TestCase):
    def test_drops_trailers(self):
        body = ("A real change.\n"
                "Co-authored-by: Someone <a@b.c>\n"
                "Signed-off-by: Someone <a@b.c>\n")
        self.assertEqual(dc.scrub_body(body).strip(), "A real change.")

    def test_truncates_a_huge_body(self):
        self.assertLessEqual(len(dc.scrub_body("x" * 5000)), dc.MAX_BODY_CHARS)


if __name__ == "__main__":
    unittest.main()


class TestGoldenFixtureSharedWithTheCli(unittest.TestCase):
    """installer/testdata/entry.md is parsed by the Go renderer in
    installer/changelog_render.go. Two hand-written parsers of one grammar is
    how a format silently drifts, so both sides read the SAME fixture: change
    the rendered shape and this test and the Go test fail together.

    To regenerate after an intended format change:
        python3 tools/tests/test_draft_changelog.py --regenerate-fixture
    """

    FIXTURE = (pathlib.Path(__file__).resolve().parents[2]
               / "installer" / "testdata" / "entry.md")

    @staticmethod
    def _build():
        draft = dc.ChangelogDraft(
            headline="Mod packs install and play in one step",
            summary=("A map that needs mod packs now installs all of them from a single "
                     "prompt and plays straight away, instead of asking once per pack "
                     "and sending you back to restart DOOM."),
            added=["Install every mod pack a map needs from a single prompt.",
                   "Play a map's mod pack straight after installing it, with no restart."],
            improved=["The mod-pack consent prompt uses a dialog that cannot delete anything.",
                      "Logs and crash records are rolled aside instead of growing without limit."],
            fixed=["Choosing Yes on the mod-pack prompt now actually installs the pack."],
            collapsed_count=15,
            sources=["abc1234"],
        )
        dc.validate(draft)
        section = dc.render("v0.2.1-beta.6", "2026-09-03", draft)
        # Exactly what release.yml publishes as the release body, which is what
        # the CLI fetches and the Go renderer parses.
        return changelog.parse(section)[0]["body"] + "\n"

    def test_renderer_still_produces_the_shared_fixture(self):
        self.assertEqual(
            self._build(), self.FIXTURE.read_text(encoding="utf-8"),
            "the rendered entry no longer matches installer/testdata/entry.md -- "
            "regenerate it and update installer/changelog_render.go to match",
        )


if __name__ == "__main__" and "--regenerate-fixture" in sys.argv:
    path = TestGoldenFixtureSharedWithTheCli.FIXTURE
    path.write_text(TestGoldenFixtureSharedWithTheCli._build(),
                    encoding="utf-8", newline="\n")
    print("regenerated " + str(path))
    raise SystemExit(0)

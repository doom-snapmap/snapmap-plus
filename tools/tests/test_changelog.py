"""Tests for tools/changelog.py. Stdlib unittest -- no pytest dependency."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import changelog  # noqa: E402

FIXTURES = pathlib.Path(__file__).parent / "fixtures"
VALID = (FIXTURES / "valid.md").read_text(encoding="utf-8")
MALFORMED = (FIXTURES / "malformed.md").read_text(encoding="utf-8")
SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "changelog.py"


class TestParse(unittest.TestCase):
    def test_finds_every_section_in_order(self):
        got = [s["tag"] for s in changelog.parse(VALID)]
        self.assertEqual(got, ["v0.2.1", "v0.2.1-beta.6", "v0.1.0-beta.1"])

    def test_parses_header_fields(self):
        first = changelog.parse(VALID)[0]
        self.assertEqual(first["date"], "2026-09-10")
        self.assertEqual(first["channel"], "stable")

    def test_accepts_en_dash_separator(self):
        self.assertIn("v0.1.0-beta.1", [s["tag"] for s in changelog.parse(VALID)])

    def test_body_keeps_group_headings(self):
        """A '^##' terminator would truncate the body at '### New'."""
        body = changelog.parse(VALID)[1]["body"]
        self.assertIn("### New", body)
        self.assertIn("### Improved", body)
        self.assertIn("_Plus 14 smaller fixes", body)

    def test_body_excludes_the_next_section(self):
        body = changelog.parse(VALID)[1]["body"]
        self.assertNotIn("v0.1.0-beta.1", body)
        self.assertNotIn("First public beta", body)

    def test_body_excludes_its_own_header(self):
        self.assertNotIn("## v0.2.1", changelog.parse(VALID)[0]["body"])


class TestStructuredFields(unittest.TestCase):
    def test_headline_and_summary(self):
        s = changelog.parse(VALID)[1]
        self.assertEqual(s["headline"], "One-prompt map installs")
        self.assertTrue(s["summary"].startswith("Installing a community map"))

    def test_groups(self):
        s = changelog.parse(VALID)[1]
        self.assertEqual(len(s["added"]), 2)
        self.assertEqual(s["added"][0], "Install everything a map needs from one prompt.")
        self.assertEqual(len(s["improved"]), 1)
        self.assertEqual(s["fixed"], [])

    def test_hand_wrapped_bullet_keeps_its_continuation(self):
        """CHANGELOG.md is edited by hand; a wrapped bullet must not lose its tail."""
        s = changelog.parse(VALID)[1]
        self.assertEqual(
            s["added"][1],
            "Use a map's mod pack straight after installing it, even when the "
            "pack arrived halfway through the session.",
        )

    def test_collapsed_line(self):
        s = changelog.parse(VALID)[1]
        self.assertEqual(s["collapsed"], "Plus 14 smaller fixes and internal changes.")

    def test_section_without_groups(self):
        s = changelog.parse(VALID)[2]
        self.assertEqual(s["added"], [])
        self.assertEqual(s["collapsed"], "")


class TestFind(unittest.TestCase):
    def test_exact_match_does_not_match_a_prefix(self):
        """v0.2.1 must not resolve to the v0.2.1-beta.6 section."""
        sections = changelog.parse(VALID)
        self.assertEqual(changelog.find(sections, "v0.2.1")["date"], "2026-09-10")
        self.assertEqual(changelog.find(sections, "v0.2.1-beta.6")["date"], "2026-09-02")

    def test_absent_tag(self):
        self.assertIsNone(changelog.find(changelog.parse(VALID), "v9.9.9"))


class TestLint(unittest.TestCase):
    def test_valid_file_is_clean(self):
        self.assertEqual(changelog.lint(VALID), [])

    def test_reports_every_problem(self):
        problems = changelog.lint(MALFORMED)
        self.assertEqual(len(problems), 4, problems)
        joined = " ".join(problems)
        self.assertIn("v0.3.1-beta.1", joined)   # channel contradicts the tag
        self.assertIn("not-a-version", joined)
        self.assertIn("10-04-2026", joined)
        self.assertIn("canary", joined)

    def test_reports_duplicate_tags(self):
        dupe = VALID + "\n## v0.2.1 -- 2026-09-11 (stable)\n\n**Again**\n"
        self.assertTrue(any("duplicate" in p.lower() for p in changelog.lint(dupe)))


class TestCli(unittest.TestCase):
    def _run(self, *args, fixture="valid.md"):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--file", str(FIXTURES / fixture), *args],
            capture_output=True, text=True,
        )

    def test_section_writes_utf8_file(self):
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "notes.md"
            r = self._run("--section", "v0.2.1-beta.6", "--out", str(out))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("One-prompt map installs", out.read_text(encoding="utf-8"))

    def test_section_missing_tag_exits_1(self):
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "notes.md"
            r = self._run("--section", "v9.9.9", "--out", str(out))
            self.assertEqual(r.returncode, 1)
            self.assertIn("v9.9.9", r.stderr)

    def test_json_shape(self):
        with tempfile.TemporaryDirectory() as d:
            out = pathlib.Path(d) / "changelog.json"
            self.assertEqual(self._run("--json", "--out", str(out)).returncode, 0)
            data = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(data[0]["tag"], "v0.2.1")
            self.assertEqual(data[1]["added"][0],
                             "Install everything a map needs from one prompt.")
            self.assertNotIn("body", data[0])

    def test_has_exit_codes(self):
        self.assertEqual(self._run("--has", "v0.2.1").returncode, 0)
        self.assertEqual(self._run("--has", "v9.9.9").returncode, 1)

    def test_lint_exit_codes(self):
        self.assertEqual(self._run("--lint").returncode, 0)
        self.assertEqual(self._run("--lint", fixture="malformed.md").returncode, 1)

    def test_missing_file_is_a_clean_error_not_a_traceback(self):
        r = subprocess.run(
            [sys.executable, str(SCRIPT), "--file", "no/such/file.md", "--lint"],
            capture_output=True, text=True,
        )
        self.assertEqual(r.returncode, 1)
        self.assertNotIn("Traceback", r.stderr)


if __name__ == "__main__":
    unittest.main()

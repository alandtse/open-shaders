import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
SCRIPT_PATH = ROOT / ".github" / "scripts" / "nexus_changelog.py"
SPEC = importlib.util.spec_from_file_location("nexus_changelog", SCRIPT_PATH)
nexus_changelog = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(nexus_changelog)


class MarkdownToPlainTextTests(unittest.TestCase):
    def test_headings_are_unwrapped(self):
        self.assertEqual(nexus_changelog.markdown_to_plain_text("## What's New"), "What's New")

    def test_bullet_list_markers_are_normalized(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("* first\n+ second\n- third"),
            "- first\n- second\n- third",
        )

    def test_link_becomes_label_and_url(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("See [the PR](https://example.com/pr/1) for details."),
            "See the PR (https://example.com/pr/1) for details.",
        )

    def test_bare_link_keeps_only_url(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("[](https://example.com)"),
            "https://example.com",
        )

    def test_link_already_wrapped_in_parens_does_not_double_up(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("procedural sun ([#678](https://example.com/issues/678))"),
            "procedural sun https://example.com/issues/678",
        )

    def test_adjacent_wrapped_links_do_not_double_up(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text(
                "fix ([#691](https://example.com/issues/691)) ([f24fa5a8](https://example.com/commit/f24fa5a8))"
            ),
            "fix https://example.com/issues/691 https://example.com/commit/f24fa5a8",
        )

    def test_issue_number_label_matching_url_is_dropped(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("[#678](https://example.com/issues/678)"),
            "https://example.com/issues/678",
        )

    def test_issue_number_label_not_matching_url_is_kept(self):
        # e.g. a link whose visible text is an unrelated issue number.
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("[#1](https://example.com/issues/678)"),
            "#1 (https://example.com/issues/678)",
        )

    def test_commit_hash_label_matching_url_is_dropped(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text(
                "[470f8472](https://example.com/commit/470f847247223a7a2db99aff2253c261f0bd7f4c)"
            ),
            "https://example.com/commit/470f847247223a7a2db99aff2253c261f0bd7f4c",
        )

    def test_short_numeric_label_is_not_mistaken_for_hex(self):
        # A bare (non-"#") numeric label isn't an issue-number match, and
        # "123" isn't hex-plausible at under 6 chars, so it must be kept.
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("[123](https://example.com/other/456)"),
            "123 (https://example.com/other/456)",
        )

    def test_image_keeps_only_alt_text(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("![screenshot](https://example.com/x.png)"),
            "screenshot",
        )

    def test_bold_italic_and_strikethrough_markers_are_stripped(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("**bold** and *italic* and ~~old~~ text"),
            "bold and italic and old text",
        )

    def test_nested_emphasis_is_fully_unwrapped(self):
        self.assertEqual(nexus_changelog.markdown_to_plain_text("**_bold italic_**"), "bold italic")

    def test_inline_code_keeps_only_content(self):
        self.assertEqual(nexus_changelog.markdown_to_plain_text("Fixed `shadows` flicker"), "Fixed shadows flicker")

    def test_code_fence_keeps_only_body(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("```cpp\nint x = 1;\n```"),
            "int x = 1;",
        )

    def test_code_fence_body_is_not_mangled_by_other_passes(self):
        # A fenced "# ..." line must not lose its "#" to the heading pass, and
        # a fenced "---" line must not be removed by the horizontal-rule pass.
        body = "```cpp\n# define FEATURE 1\n---\nint x = 1;\n```"
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text(body),
            "# define FEATURE 1\n---\nint x = 1;",
        )

    def test_inline_code_identifier_is_not_mangled_by_emphasis_pass(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("call `snake_case_name` here"),
            "call snake_case_name here",
        )

    def test_intraword_underscores_are_not_treated_as_emphasis(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("foo_bar_baz stays literal"),
            "foo_bar_baz stays literal",
        )

    def test_underscore_emphasis_at_word_boundary_still_converts(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("__init__ is special"),
            "init is special",
        )

    def test_blockquote_marker_is_dropped(self):
        self.assertEqual(nexus_changelog.markdown_to_plain_text("> A quoted note"), "A quoted note")

    def test_horizontal_rule_is_removed(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("Above\n\n---\n\nBelow"),
            "Above\n\nBelow",
        )

    def test_blank_line_runs_are_collapsed(self):
        self.assertEqual(
            nexus_changelog.markdown_to_plain_text("First\n\n\n\nSecond"),
            "First\n\nSecond",
        )

    def test_empty_body_returns_empty_string(self):
        self.assertEqual(nexus_changelog.markdown_to_plain_text(""), "")
        self.assertEqual(nexus_changelog.markdown_to_plain_text(None), "")

    def test_full_release_body(self):
        body = (
            "## What's New\n\n"
            "- **Bold fix** for `shadows`\n"
            "- Added [procedural sun](https://example.com/pr/1) support\n"
            "- *Italic* note and ~~old~~ text\n\n"
            "> A quote line\n\n"
            "---\n"
        )
        expected = (
            "What's New\n\n"
            "- Bold fix for shadows\n"
            "- Added procedural sun (https://example.com/pr/1) support\n"
            "- Italic note and old text\n\n"
            "A quote line"
        )
        self.assertEqual(nexus_changelog.markdown_to_plain_text(body), expected)


if __name__ == "__main__":
    unittest.main()

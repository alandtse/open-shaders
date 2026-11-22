import importlib.util
import unittest
from pathlib import Path

TOOL = Path(__file__).parents[1] / "tools" / "shader_cost_report.py"
SPEC = importlib.util.spec_from_file_location("shader_cost_report", TOOL)
REPORT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(REPORT)

CONFIG = """
shaders:
  - file: Lit.hlsl
    configs:
      PSHADER:
        common_defines: [PSHADER]
        entries:
          - {entry: "Lit:Pixel:1", defines: [A]}
          - {entry: "Lit:Pixel:2", defines: [B]}
  - file: Sky.hlsl
    configs:
      PSHADER:
        entries:
          - {entry: "Sky:Pixel:1"}
"""


def build(files):
    """(tree, read) for an in-memory shader tree: blob sha is the content itself."""
    tree = {path: text for path, text in files.items()}
    return tree, lambda sha: sha.encode()


def variants(files):
    tree, read = build(files)
    return REPORT.variant_map(REPORT.parse_variants(CONFIG), tree, read)


class ShaderCostReportTest(unittest.TestCase):
    def test_parse_variants_expands_entries_with_common_defines(self):
        rows = REPORT.parse_variants(CONFIG)
        self.assertEqual(
            [(r[0], r[2]) for r in rows],
            [
                ("Lit.hlsl", "Lit:Pixel:1"),
                ("Lit.hlsl", "Lit:Pixel:2"),
                ("Sky.hlsl", "Sky:Pixel:1"),
            ],
        )
        self.assertIn("PSHADER", rows[0][3])

    def test_include_change_recompiles_only_dependents(self):
        base = {
            "Lit.hlsl": '#include "Common/A.hlsli"\n',
            "Sky.hlsl": "x",
            "Common/A.hlsli": "a",
        }
        head = dict(base, **{"Common/A.hlsli": "a plus more"})
        result = REPORT.compare(variants(base), variants(head), REPORT.Weights())
        self.assertEqual(
            (result["changed"], result["added"], result["removed"]), (2, 0, 0)
        )
        self.assertGreater(result["growth_pct"], 0)

    def test_nested_and_case_insensitive_includes_resolve(self):
        files = {
            "Lit.hlsl": '#include "common/b.hlsli"\n',
            "Sky.hlsl": "x",
            "Common/B.hlsli": '#include "C.hlsli"\n',
            "Common/C.hlsli": "c",
        }
        tree, read = build(files)
        digest, _, _ = REPORT.closure_index(tree, read)
        before = digest("Lit.hlsl")
        tree["Common/C.hlsli"] = "c2"
        digest, _, _ = REPORT.closure_index(tree, read)
        self.assertNotEqual(before, digest("Lit.hlsl"))
        self.assertEqual(
            digest("Sky.hlsl"),
            REPORT.closure_index({"Sky.hlsl": "x"}, read)[0]("Sky.hlsl"),
        )

    def test_added_and_removed_variants_are_weighted(self):
        base = variants({"Lit.hlsl": "x", "Sky.hlsl": "y"})
        head = dict(base)
        gone = ("Sky.hlsl", "Sky:Pixel:1", "PSHADER")
        del head[gone]
        head[("Lit.hlsl", "Lit:Pixel:3", "PSHADER")] = ("new", 1)
        result = REPORT.compare(base, head, REPORT.Weights())
        self.assertEqual((result["added"], result["removed"]), (1, 1))
        self.assertAlmostEqual(result["net_pct"], 0.0)

    def test_verdict_uses_net_cost_in_both_directions(self):
        up = {"net_pct": 6.0, "added": 1, "removed": 0}
        down = {"net_pct": -6.0, "added": 0, "removed": 4}
        flat = {"net_pct": 0.0, "added": 0, "removed": 0}
        self.assertEqual(REPORT.verdict(up, False), "up")
        self.assertEqual(REPORT.verdict(down, False), "down")
        self.assertEqual(REPORT.verdict(flat, False), "flat")
        self.assertEqual(REPORT.verdict(flat, True), "flags")

    def test_render_states_direction_and_marker(self):
        result = {
            "base_total": 10,
            "total": 12,
            "changed": 3,
            "added": 2,
            "removed": 0,
            "recompile_s": 600.0,
            "total_s": 1000.0,
            "added_pct": 8.0,
            "removed_pct": 0.0,
            "growth_pct": 0.0,
            "net_s": 80.0,
            "net_pct": 8.0,
            "top_files": [("Lit.hlsl", 5)],
        }
        text = REPORT.render({"VR": result, "SE": None}, False, True)
        self.assertTrue(text.startswith(REPORT.MARKER))
        self.assertIn("goes up", text)
        self.assertIn("n/a", text)
        self.assertIn("`Lit.hlsl` (5)", text)

    def test_untimed_report_shows_counts_only(self):
        result = {
            "base_total": 5,
            "total": 5,
            "changed": 0,
            "added": 0,
            "removed": 0,
            "recompile_s": 0.0,
            "total_s": 5.0,
            "added_pct": 0.0,
            "removed_pct": 0.0,
            "growth_pct": 0.0,
            "net_s": 0.0,
            "net_pct": 0.0,
            "top_files": [],
        }
        text = REPORT.render({"VR": result}, False, False)
        self.assertIn("unchanged", text)
        self.assertNotIn("Net (est.)", text)
        self.assertIn("counts only", text)

    def test_timing_diff_flags_large_slowdowns_only(self):
        import json
        import os
        import tempfile

        def write(rows):
            handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
            json.dump(
                [
                    {
                        "file": "Lit.hlsl",
                        "entry": e,
                        "type": "PSHADER",
                        "duration_seconds": d,
                    }
                    for e, d in rows
                ],
                handle,
            )
            handle.close()
            self.addCleanup(os.unlink, handle.name)
            return handle.name

        base = write([("a", 100.0), ("b", 100.0), ("c", 100.0), ("d", 5.0)])
        head = write(
            [("a", 160.0), ("b", 105.0), ("c", 50.0), ("d", 9.0), ("new", 30.0)]
        )
        diff = REPORT.timing_diff(base, head)
        self.assertEqual((diff["compared"], diff["slower"], diff["faster"]), (4, 1, 1))
        self.assertEqual(diff["worst"][0][0], "a")
        self.assertIsNone(REPORT.timing_diff(base, ""))

    def _digest(self, files, defines=()):
        tree, read = build(files)
        digest, _, _ = REPORT.closure_index(tree, read)
        return digest("Lit.hlsl", frozenset(defines))

    def test_edit_in_inactive_block_keeps_digest(self):
        def files(body):
            return {
                "Lit.hlsl": '#include "Data.hlsli"\n',
                "Data.hlsli": f"float a;\n#if defined(SNOW) && (defined(PSHADER) || defined(CSHADER))\n{body}\n#endif\n",
            }

        old, new = files("float snow;"), files("float snow; float more;")
        self.assertEqual(self._digest(old, ["PSHADER"]), self._digest(new, ["PSHADER"]))
        self.assertNotEqual(
            self._digest(old, ["SNOW", "PSHADER"]),
            self._digest(new, ["SNOW", "PSHADER"]),
        )

    def test_includes_in_inactive_blocks_are_dropped(self):
        def files(extra):
            return {
                "Lit.hlsl": '#ifdef SNOW\n#include "Snow.hlsli"\n#endif\n#ifndef SNOW\nfloat plain;\n#else\nfloat snowy;\n#endif\n',
                "Snow.hlsli": extra,
            }

        self.assertEqual(self._digest(files("a")), self._digest(files("b")))
        self.assertNotEqual(
            self._digest(files("a"), ["SNOW"]), self._digest(files("b"), ["SNOW"])
        )

    def test_undecidable_conditions_keep_every_branch(self):
        def files(body):
            return {
                "Lit.hlsl": f"#define FEATURE 1\n#if FEATURE > 0 || defined(OTHER)\n{body}\n#endif\n"
            }

        self.assertNotEqual(self._digest(files("x")), self._digest(files("y")))

        def implicit(body):
            return {"Lit.hlsl": f"#ifdef VR\n{body}\n#endif\n"}

        self.assertNotEqual(self._digest(implicit("x")), self._digest(implicit("y")))

    def test_macros_defined_outside_the_include_closure_do_not_blur_conditions(self):
        def files(body, unrelated):
            return {
                "Lit.hlsl": f"#if MODE\n{body}\n#endif\n",
                "Unrelated.hlsl": unrelated,
            }

        self.assertEqual(
            self._digest(files("x", "float a;")),
            self._digest(files("x", "#define MODE 1\nfloat a;")),
        )

    def test_comment_and_whitespace_only_edits_keep_digest(self):
        plain = {"Lit.hlsl": "float a;\nfloat   b;\n"}
        noisy = {"Lit.hlsl": "float a; // note\n/* block */\n  float b;\n\n"}
        self.assertEqual(self._digest(plain), self._digest(noisy))

    def test_variants_use_their_own_defines(self):
        def files(body):
            return {"Lit.hlsl": f"#if defined(A) && !defined(B)\n{body}\n#endif\n"}

        text = CONFIG.replace("[A]", "[A]").replace("Lit:Pixel:2", "Lit:Pixel:2")
        old = REPORT.variant_map(REPORT.parse_variants(text), *build(files("x")))
        new = REPORT.variant_map(REPORT.parse_variants(text), *build(files("y")))
        changed = [k for k in old if old[k][0] != new[k][0]]
        self.assertEqual([k[1] for k in changed], ["Lit:Pixel:1"])

    def test_measured_slowdown_sets_headline_when_structure_is_flat(self):
        flat = {
            "base_total": 5,
            "total": 5,
            "changed": 0,
            "added": 0,
            "removed": 0,
            "recompile_s": 0.0,
            "total_s": 5.0,
            "added_pct": 0.0,
            "removed_pct": 0.0,
            "growth_pct": 0.0,
            "net_s": 0.0,
            "net_pct": 0.0,
            "top_files": [],
        }
        diff = {
            "compared": 30,
            "base_s": 100.0,
            "pr_s": 120.0,
            "delta_pct": 20.0,
            "slower": 4,
            "faster": 0,
            "worst": [("Lit:Pixel:1", 50.0, 70.0)],
        }
        text = REPORT.render({"VR": flat}, False, True, {"VR": diff})
        self.assertIn("compile slower (measured)", text)
        self.assertIn("`Lit:Pixel:1` 50 → 70 s", text)
        few = dict(diff, compared=3)
        self.assertIn(
            "unchanged", REPORT.render({"VR": flat}, False, True, {"VR": few})
        )


if __name__ == "__main__":
    unittest.main()

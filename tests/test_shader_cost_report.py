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


if __name__ == "__main__":
    unittest.main()

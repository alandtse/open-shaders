import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("feature_flip_impact", ROOT / "tools/feature-flip-impact.py")
tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(tool)


class FeatureFlipImpactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, document):
        path = self.root / name
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_merge_joins_runtimes(self):
        se = self.write("se.json", {"schemaVersion": 1, "runtimes": {"SE": {"SSS": {"share": 0.04, "groups": 3}}}})
        vr = self.write("vr.json", {"schemaVersion": 1, "runtimes": {"VR": {"SSS": {"share": 0.05, "groups": 4}}}})
        merged = tool.merge_tables([se, vr])
        self.assertEqual(sorted(merged["runtimes"]), ["SE", "VR"])
        self.assertEqual(merged["runtimes"]["VR"]["SSS"]["share"], 0.05)

    def test_merge_skips_unknown_schema(self):
        se = self.write("se.json", {"schemaVersion": 1, "runtimes": {"SE": {}}})
        future = self.write("future.json", {"schemaVersion": 2, "runtimes": {"VR": {}}})
        self.assertEqual(sorted(tool.merge_tables([se, future])["runtimes"]), ["SE"])

    def test_merge_with_no_usable_table_is_empty(self):
        self.assertEqual(tool.merge_tables([])["runtimes"], {})

    def test_write_table_creates_parent_and_ends_with_newline(self):
        output = self.root / "nested" / "table.json"
        tool.write_table({"schemaVersion": 1, "runtimes": {}}, output)
        self.assertTrue(output.read_text(encoding="utf-8").endswith("\n"))

    def test_type_means_average_per_file_and_stage(self):
        report = [
            {"file": "Lighting.hlsl", "type": "PSHADER", "duration_seconds": 10.0},
            {"file": "Lighting.hlsl", "type": "PSHADER", "duration_seconds": 20.0},
            {"file": "Lighting.hlsl", "type": "VSHADER", "duration_seconds": 1.0},
        ]
        means = tool.mean_by_file_and_type(report)
        self.assertEqual(means[("Lighting.hlsl", "PSHADER")], 15.0)
        self.assertEqual(means[("Lighting.hlsl", "VSHADER")], 1.0)

    def test_deduped_zero_durations_do_not_zero_a_group_or_skew_the_mean(self):
        report = [
            {"file": "L.hlsl", "entry": "a", "type": "PSHADER", "duration_seconds": 30.0},
            {"file": "L.hlsl", "entry": "b", "type": "PSHADER", "duration_seconds": 0.0},
            {"file": "L.hlsl", "entry": "c", "type": "PSHADER", "duration_seconds": 2.0},
        ]
        means = tool.mean_by_file_and_type(report)
        self.assertEqual(means[("L.hlsl", "PSHADER")], 16.0)
        variants = [("L.hlsl", "a", "PSHADER"), ("L.hlsl", "b", "PSHADER"), ("L.hlsl", "c", "PSHADER")]
        timings = {("L.hlsl", t["entry"]): t["duration_seconds"] for t in report}
        share, changed = tool.group_share(variants, ["x", "x", "y"], ["z", "z", "y"], timings, means)
        self.assertEqual((share, changed), (30.0 / 32.0, 1))


    def test_group_is_changed_when_any_member_changes(self):
        variants = [("L.hlsl", "a", "PSHADER"), ("L.hlsl", "b", "PSHADER")]
        timings = {("L.hlsl", "a"): 10.0, ("L.hlsl", "b"): 12.0}
        self.assertEqual(tool.group_share(variants, ["x", "x"], ["x", "x"], timings, {}), (0.0, 0))
        share, changed = tool.group_share(variants, ["x", "x"], ["x", "y"], timings, {})
        self.assertEqual((share, changed), (1.0, 1))

    def test_unchanged_group_costs_nothing_and_missing_timing_is_skipped(self):
        variants = [("L.hlsl", "a", "PSHADER"), ("L.hlsl", "b", "VSHADER")]
        share, changed = tool.group_share(variants, ["x", "y"], ["x", "z"], {("L.hlsl", "a"): 3.0, ("L.hlsl", "b"): 1.0}, {})
        self.assertEqual((round(share, 2), changed), (0.25, 1))
        self.assertEqual(tool.group_share(variants, ["x", "y"], ["x", "z"], {}, {}), (None, 0))

    def test_flip_impact_only_rehashes_variants_that_have_the_define(self):
        variants = [
            ("A.hlsl", "a", "PSHADER", Path("A.hlsl"), ["LIGHT", "NOOP", "SSS"]),
            ("A.hlsl", "b", "PSHADER", Path("A.hlsl"), ["LIGHT", "SSS"]),
            ("B.hlsl", "c", "PSHADER", Path("B.hlsl"), ["LIGHT"]),
            ("B.hlsl", "d", "VSHADER", Path("B.hlsl"), []),
        ]
        timings = {("A.hlsl", "a"): 4.0, ("A.hlsl", "b"): 6.0, ("B.hlsl", "c"): 8.0, ("B.hlsl", "d"): 2.0}
        calls = []

        def fake_hash(d3d, stage, relative, defines):
            calls.append(tuple(defines))
            return relative.as_posix() + "|" + ",".join(d for d in defines if d != "NOOP")

        with mock.patch.object(tool, "code_hash", fake_hash):
            impact = tool.flip_impact(None, None, variants, timings, {}, ["LIGHT", "NOOP", "SSS"], 2)

        def brute_force(define):
            with_define = [fake_hash(None, None, v[3], v[4]) for v in variants]
            without = [fake_hash(None, None, v[3], [d for d in v[4] if d != define]) for v in variants]
            share, changed = tool.group_share(variants, with_define, without, timings, {})
            return {"share": round(share, 3), "groups": changed}

        self.assertEqual(impact, {define: brute_force(define) for define in ["LIGHT", "NOOP", "SSS"]})
        calls.clear()
        with mock.patch.object(tool, "code_hash", fake_hash):
            tool.flip_impact(None, None, variants, timings, {}, ["LIGHT", "NOOP", "SSS"], 2)
        self.assertEqual(len(calls), len(variants) + 3 + 1 + 2)


if __name__ == "__main__":
    unittest.main()

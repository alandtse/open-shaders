import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("release_plan", ROOT / "tools" / "release_plan.py")
release_plan = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(release_plan)


class TempRepo:
    def __init__(self, directory):
        self.path = directory
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "commit.gpgsign", "false")

    def git(self, *args):
        result = subprocess.run(["git", *args], cwd=self.path, capture_output=True, text=True, check=True)
        return result.stdout.strip()

    def commit(self, relative_path, content):
        target = Path(self.path) / relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", f"change {relative_path}")
        return self.git("rev-parse", "HEAD")

    def set_origin_main(self, sha):
        self.git("update-ref", "refs/remotes/origin/main", sha)


class PlanTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = TempRepo(self._tmp.name)
        self.main = self.repo.commit(".github/workflows/a.yaml", "one")
        self.repo.set_origin_main(self.main)

    def decide(self, ff_target, **overrides):
        options = dict(
            ref_name="main", ff_target=ff_target, single_phase=False, base_sha="", head_sha=self.main
        )
        options.update(overrides)
        return release_plan.plan(self.repo.path, **options)

    def test_workflow_diff_promotes(self):
        target = self.repo.commit(".github/workflows/a.yaml", "two")
        mode, base, _ = self.decide(target)
        self.assertEqual((mode, base), ("promote", self.main))

    def test_action_diff_promotes(self):
        target = self.repo.commit(".github/actions/x/action.yaml", "new")
        self.assertEqual(self.decide(target)[0], "promote")

    def test_tools_only_diff_releases(self):
        target = self.repo.commit("tools/build-shader-cache.py", "x")
        self.assertEqual(self.decide(target)[:2], ("release", ""))

    def test_identical_definitions_release(self):
        target = self.repo.commit("src/a.cpp", "x")
        self.assertEqual(self.decide(target)[0], "release")

    def test_empty_ff_target_releases(self):
        self.assertEqual(self.decide("")[0], "release")

    def test_non_main_ref_releases(self):
        target = self.repo.commit(".github/workflows/a.yaml", "two")
        self.assertEqual(self.decide(target, ref_name="dev")[0], "release")

    def test_single_phase_bypasses_detection(self):
        target = self.repo.commit(".github/workflows/a.yaml", "two")
        self.assertEqual(self.decide(target, single_phase=True)[0], "release")

    def test_unresolvable_ff_target_is_refused(self):
        with self.assertRaises(release_plan.PlanError):
            self.decide("f" * 40)

    def test_git_diff_failure_fails_toward_promote(self):
        target = self.repo.commit("src/a.cpp", "x")
        original = release_plan._git

        def broken(args, cwd):
            if args[:2] == ["diff", "--quiet"]:
                return subprocess.CompletedProcess(args, 128, "", "fatal")
            return original(args, cwd)

        release_plan._git = broken
        self.addCleanup(setattr, release_plan, "_git", original)
        self.assertEqual(self.decide(target)[0], "promote")

    def test_main_equal_target_without_base_sha_is_refused(self):
        with self.assertRaises(release_plan.PlanError):
            self.decide(self.main)


class PhaseTwoTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = TempRepo(self._tmp.name)
        self.old_main = self.repo.commit(".github/workflows/a.yaml", "one")
        self.target = self.repo.commit(".github/workflows/a.yaml", "two")
        self.repo.set_origin_main(self.target)

    def phase_two(self, **overrides):
        options = dict(
            ref_name="main", ff_target=self.target, single_phase=False, base_sha=self.old_main, head_sha=self.target
        )
        options.update(overrides)
        return release_plan.plan(self.repo.path, **options)

    def test_phase_two_releases_and_keeps_base_sha(self):
        self.assertEqual(self.phase_two()[:2], ("release", self.old_main))

    def test_never_loops_back_to_promote(self):
        self.assertNotEqual(self.phase_two()[0], "promote")

    def test_base_sha_not_an_ancestor_is_refused(self):
        self.repo.git("checkout", "-q", "--orphan", "other")
        stray = self.repo.commit("x.txt", "x")
        with self.assertRaises(release_plan.PlanError):
            self.phase_two(base_sha=stray)

    def test_main_not_at_ff_target_is_refused(self):
        self.repo.set_origin_main(self.old_main)
        with self.assertRaises(release_plan.PlanError):
            self.phase_two()

    def test_definitions_differing_from_dispatched_commit_are_refused(self):
        with self.assertRaises(release_plan.PlanError):
            self.phase_two(head_sha=self.old_main)


class DispatchArgsTests(unittest.TestCase):
    def test_skip_feature_bumps_forwarded_only_when_true(self):
        self.assertNotIn("skip_feature_bumps=true", release_plan.gh_dispatch_args("a", "b", False))
        self.assertIn("skip_feature_bumps=true", release_plan.gh_dispatch_args("a", "b", True))

    def test_forwarded_inputs_match_workflow_declaration(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        workflow = yaml.safe_load((ROOT / ".github" / "workflows" / "release-semantic.yaml").read_text(encoding="utf-8"))
        declared = set(workflow[True]["workflow_dispatch"]["inputs"])
        self.assertEqual(declared - {"single_phase"}, set(release_plan.FORWARDED_INPUTS))


if __name__ == "__main__":
    unittest.main()

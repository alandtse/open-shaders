import importlib.util
import io
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_fork_lines", ROOT / "tools" / "verify-fork-lines.py")
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)

BASE = "int baseLineOne = computeBase(1);\nint baseLineTwo = computeBase(2);\n"
FORK_HOOK = "FlipImpactDisplay::Tooltip(feature, impactTier);\n"
FORK_OTHER = "state.glossinessSpecular = wetnessGlossiness;\n"


class VerifyForkLinesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        self.git("init", "-q", "-b", "dev")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "commit.gpgsign", "false")

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", self.repo, *args], check=True, capture_output=True, text=True).stdout.strip()

    def write(self, name, text):
        path = Path(self.repo, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def losses(self, head):
        return verify.find_losses(self.repo, "dev", head, "10 years ago", verify.re.compile("Translations/"))

    def make_fork(self):
        self.write("src/a.cpp", BASE)
        self.commit("base")
        self.write("src/a.cpp", BASE + FORK_HOOK)
        hook = self.commit("feat: hook")
        self.write("src/b.cpp", FORK_OTHER)
        self.commit("feat: other")
        return hook

    def test_intact_head_has_no_losses(self):
        self.make_fork()
        self.assertEqual(self.losses("dev"), [])

    def test_dropped_line_is_reported_against_the_adding_commit(self):
        hook = self.make_fork()
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.write("src/a.cpp", BASE)
        self.commit("sync drops hook")
        lost = self.losses("sync")
        self.assertEqual([(i["sha"], i["line"]) for i in lost], [(hook, FORK_HOOK.strip())])
        self.assertTrue(lost[0]["at_fork"])

    def test_line_relocated_to_another_file_is_not_lost(self):
        self.make_fork()
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.write("src/a.cpp", BASE)
        self.write("src/c.cpp", FORK_HOOK)
        self.commit("sync moves hook")
        self.assertEqual(self.losses("sync"), [])

    def test_line_removed_by_a_later_fork_commit_is_not_expected(self):
        self.make_fork()
        self.write("src/a.cpp", BASE)
        self.commit("refactor: fork removes hook itself")
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.assertEqual(self.losses("sync"), [])

    def test_loss_baked_into_the_fork_tip_is_marked_already_lost(self):
        hook = self.make_fork()
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.write("src/a.cpp", BASE)
        self.commit("sync drops hook")
        self.git("checkout", "-q", "dev")
        self.git("merge", "-q", "--no-ff", "sync", "-m", "merge sync")
        lost ={i["line"]: i for i in self.losses("dev")}
        self.assertIn(FORK_HOOK.strip(), lost)
        self.assertFalse(lost[FORK_HOOK.strip()]["at_fork"])
        self.assertEqual(lost[FORK_HOOK.strip()]["sha"], hook)

    def test_rename_carries_tracked_lines(self):
        self.make_fork()
        self.git("mv", "src/b.cpp", "src/renamed.cpp")
        self.commit("refactor: rename")
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.assertEqual(self.losses("sync"), [])

    def test_trivial_lines_are_ignored(self):
        self.write("src/a.cpp", BASE)
        self.commit("base")
        self.write("src/a.cpp", BASE + "}\n// a comment line that is long enough\n")
        self.commit("feat: trivial")
        self.git("checkout", "-q", "-b", "sync", "dev~1")
        self.assertEqual(self.losses("sync"), [])

    def test_main_exit_codes_and_allow(self):
        hook = self.make_fork()
        self.git("checkout", "-q", "-b", "sync", "dev")
        self.write("src/a.cpp", BASE)
        self.commit("sync drops hook")
        base = ["--repo", self.repo, "--fork-ref", "dev", "--head", "sync", "--since", "10 years ago"]
        out = io.StringIO()
        original = verify.sys.stdout
        verify.sys.stdout = out
        try:
            self.assertEqual(verify.main(base), 1)
            self.assertEqual(verify.main(base + ["--allow", hook[:10]]), 0)
        finally:
            verify.sys.stdout = original
        self.assertIn("feat: hook", out.getvalue())
        self.assertEqual(verify.main(["--repo", self.repo, "--fork-ref", "nope", "--head", "sync"]), 2)


if __name__ == "__main__":
    unittest.main()

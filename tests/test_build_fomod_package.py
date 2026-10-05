import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from test_verify_shader_cache import dxbc

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / ".github/scripts/build-fomod-package.py"
SPEC = importlib.util.spec_from_file_location("build_fomod_package", SCRIPT)
fomod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fomod)

DIGEST = "0123456789abcdef0123456789abcdef"


def write_cache(path, blobs=("Sky/a.pso", "Sky/b.pso"), manifest=None, info=True):
    path.mkdir(parents=True)
    if info:
        (path / "Info.ini").write_text("[Cache]\nPluginVersion = 1-0-0\n", encoding="utf-8")
    for name in blobs:
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_bytes(dxbc())
    entries = dict.fromkeys(blobs, DIGEST) if manifest is None else manifest
    (path / "Manifest.json").write_text(json.dumps({"schemaVersion": 1, "entries": entries}), encoding="utf-8")


class BuildFomodPackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.core = self.root / "core"
        (self.core / "SKSE/Plugins").mkdir(parents=True)
        (self.core / "SKSE/Plugins/CommunityShaders.dll").write_bytes(b"MZ")
        self.out = self.root / "staged"

    def run_builder(self, se=None, vr=None, core=None, clang=None):
        command = [sys.executable, str(SCRIPT), "--core", str(core or self.core), "--output", str(self.out), "--version", "v1.0.0"]
        if se is not None:
            command += ["--se-cache", str(se)]
        if vr is not None:
            command += ["--vr-cache", str(vr)]
        if clang is not None:
            command += ["--clang-dll", str(clang)]
        return subprocess.run(command, capture_output=True, text=True)

    def module_config(self):
        return (self.out / "fomod" / "ModuleConfig.xml").read_text(encoding="utf-8-sig")

    def test_valid_caches_become_options(self):
        write_cache(self.root / "se")
        write_cache(self.root / "vr")
        result = self.run_builder(self.root / "se", self.root / "vr")
        self.assertEqual(result.returncode, 0, result.stderr)
        config = self.module_config()
        self.assertIn("SE/AE", config)
        self.assertIn("VR", config)
        self.assertTrue((self.out / "ShaderCache-SE-AE/ShaderCache/Info.ini").is_file())
        self.assertTrue((self.out / "ShaderCache-VR/ShaderCache/Info.ini").is_file())

    def test_module_config_has_the_exact_filename(self):
        self.assertEqual(self.run_builder().returncode, 0)
        self.assertIn("ModuleConfig.xml", [entry.name for entry in (self.out / "fomod").iterdir()])

    def write_clang(self, size=2_000_000, header=b"MZ"):
        clang = self.root / "clang"
        (clang / "SKSE/Plugins").mkdir(parents=True)
        (clang / "SKSE/Plugins/CommunityShaders.dll").write_bytes(header + b"\x00" * (size - len(header)))
        return clang

    def option_type(self, name):
        plugin = ET.fromstring(self.module_config()).find(f".//plugin[@name='{name}']")
        return plugin.find("typeDescriptor/type").get("name")

    def test_clang_dll_becomes_a_radio_choice_with_the_default(self):
        result = self.run_builder(clang=self.write_clang())
        self.assertEqual(result.returncode, 0, result.stderr)
        config = self.module_config()
        self.assertIn('type="SelectExactlyOne"', config)
        self.assertIn("Default build (recommended)", config)
        self.assertIn("clang-cl build (experimental, may be faster)", config)
        self.assertIn('source="ClangCL/SKSE" destination="SKSE"', config)
        self.assertIn('source="DefaultBuild/SKSE" destination="SKSE"', config)

    def test_each_choice_installs_exactly_one_dll(self):
        self.assertEqual(self.run_builder(clang=self.write_clang()).returncode, 0)
        self.assertFalse((self.out / "Core/SKSE/Plugins/CommunityShaders.dll").exists())
        self.assertTrue((self.out / "DefaultBuild/SKSE/Plugins/CommunityShaders.dll").is_file())
        self.assertTrue((self.out / "ClangCL/SKSE/Plugins/CommunityShaders.dll").is_file())
        self.assertNotIn("priority", self.module_config())

    def test_default_build_is_recommended_and_clang_is_optional(self):
        self.assertEqual(self.run_builder(clang=self.write_clang()).returncode, 0)
        self.assertEqual(self.option_type("Default build (recommended)"), "Recommended")
        self.assertEqual(self.option_type("clang-cl build (experimental, may be faster)"), "Optional")

    def test_default_pdb_travels_with_the_default_dll(self):
        (self.core / "SKSE/Plugins/CommunityShaders.pdb").write_bytes(b"pdb")
        self.assertEqual(self.run_builder(clang=self.write_clang()).returncode, 0)
        self.assertTrue((self.out / "DefaultBuild/SKSE/Plugins/CommunityShaders.pdb").is_file())
        self.assertFalse((self.out / "Core/SKSE/Plugins/CommunityShaders.pdb").exists())

    def test_clang_option_comes_with_the_cache_options(self):
        write_cache(self.root / "se")
        self.assertEqual(self.run_builder(self.root / "se", clang=self.write_clang()).returncode, 0)
        config = self.module_config()
        self.assertIn("SE/AE", config)
        self.assertIn("clang-cl build (experimental, may be faster)", config)

    def assert_clang_dropped(self, clang):
        result = self.run_builder(clang=clang)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::", result.stderr)
        self.assertNotIn("clang-cl", self.module_config())
        self.assertFalse((self.out / "ClangCL").exists())
        self.assertFalse((self.out / "DefaultBuild").exists())
        self.assertTrue((self.out / "Core/SKSE/Plugins/CommunityShaders.dll").is_file())

    def test_absent_clang_dll_is_dropped(self):
        self.assert_clang_dropped(self.root / "no-such-clang")

    def test_tiny_clang_dll_is_dropped(self):
        self.assert_clang_dropped(self.write_clang(size=1000))

    def test_non_pe_clang_dll_is_dropped(self):
        self.assert_clang_dropped(self.write_clang(header=b"XX"))

    def test_no_clang_argument_offers_no_clang_option(self):
        self.assertEqual(self.run_builder().returncode, 0)
        self.assertNotIn("clang-cl", self.module_config())

    def test_missing_cache_is_dropped_not_fatal(self):
        write_cache(self.root / "vr")
        result = self.run_builder(self.root / "no-such-se", self.root / "vr")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::", result.stderr)
        self.assertNotIn("ShaderCache-SE-AE", self.module_config())
        self.assertFalse((self.out / "ShaderCache-SE-AE").exists())
        self.assertTrue((self.out / "ShaderCache-VR/ShaderCache").is_dir())

    def assert_dropped(self, broken):
        write_cache(self.root / "vr")
        result = self.run_builder(broken, self.root / "vr")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::", result.stderr)
        self.assertFalse((self.out / "ShaderCache-SE-AE").exists())
        self.assertTrue((self.out / "ShaderCache-VR/ShaderCache").is_dir())

    def test_empty_cache_is_dropped(self):
        (self.root / "se").mkdir()
        self.assert_dropped(self.root / "se")

    def test_cache_without_info_ini_is_dropped(self):
        write_cache(self.root / "se", info=False)
        self.assert_dropped(self.root / "se")

    def test_manifest_entry_without_a_blob_is_dropped(self):
        write_cache(self.root / "se", manifest={"Sky/a.pso": DIGEST, "Sky/b.pso": DIGEST, "Sky/gone.pso": DIGEST})
        self.assert_dropped(self.root / "se")

    def test_blob_missing_from_manifest_is_dropped(self):
        write_cache(self.root / "se", manifest={"Sky/a.pso": DIGEST})
        self.assert_dropped(self.root / "se")

    def test_truncated_blob_is_dropped(self):
        write_cache(self.root / "se")
        blob = self.root / "se/Sky/a.pso"
        blob.write_bytes(blob.read_bytes()[:20])
        self.assert_dropped(self.root / "se")

    def test_garbled_manifest_is_dropped(self):
        write_cache(self.root / "se")
        (self.root / "se/Manifest.json").write_text("{not json", encoding="utf-8")
        self.assert_dropped(self.root / "se")

    def test_every_cache_dropped_still_builds_the_aio_package(self):
        (self.root / "se").mkdir()
        result = self.run_builder(self.root / "se", self.root / "nope")
        self.assertEqual(result.returncode, 0, result.stderr)
        config = self.module_config()
        self.assertNotIn("ShaderCache-", config)
        self.assertTrue((self.out / "Core/SKSE/Plugins/CommunityShaders.dll").is_file())

    def test_core_without_the_plugin_dll_is_an_error(self):
        (self.core / "SKSE/Plugins/CommunityShaders.dll").unlink()
        result = self.run_builder()
        self.assertEqual(result.returncode, 1)
        self.assertIn("CommunityShaders.dll", result.stderr)

    def test_staged_check_flags_a_referenced_folder_that_vanished(self):
        write_cache(self.root / "se")
        self.assertEqual(self.run_builder(self.root / "se").returncode, 0)
        self.assertEqual(fomod.missing_staged_files(self.out), [])
        shutil.rmtree(self.out / "ShaderCache-SE-AE")
        self.assertTrue(fomod.missing_staged_files(self.out))


if __name__ == "__main__":
    unittest.main()

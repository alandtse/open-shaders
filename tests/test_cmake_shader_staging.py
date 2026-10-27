"""Configure-time tests for the shader staging CMake modules.

The modules are included into a stub project so the generated Ninja build can be
inspected without a compiler, vcpkg or the game SDKs.
"""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")
NINJA = shutil.which("ninja")

MODULES = ("FeatureDiscovery", "CopyIfDifferent", "ShaderStaging")

HARNESS = """\
cmake_minimum_required(VERSION 3.28)
project(shader_staging_harness NONE)
set(ENABLE_EFFECTS11 {effects11})
set(SKIP_RUNTIME_DOWNLOADS {skip_downloads})
set(AIO_DIR "${{CMAKE_CURRENT_BINARY_DIR}}/aio")
set(FFX_RUNTIME_SHADER_ROOT "${{CMAKE_CURRENT_BINARY_DIR}}/ffx-runtime/Shaders")
set(FFX_RUNTIME_RELATIVE_DIRECTORY "Shaders/Upscaling/FidelityFX")
set(FFX_RUNTIME_FILES "${{FFX_RUNTIME_SHADER_ROOT}}/Upscaling/FidelityFX/ffx.dll")
set(STREAMLINE_RUNTIME_DIRECTORY "${{CMAKE_CURRENT_BINARY_DIR}}/sl/Shaders/Upscaling/Streamline")
set(STREAMLINE_RUNTIME_RELATIVE_DIRECTORY "Shaders/Upscaling/Streamline")
set(STREAMLINE_RUNTIME_FILES "${{STREAMLINE_RUNTIME_DIRECTORY}}/sl.dll")
set(STREAMLINE_RUNTIME_DX12_DIRECTORY "${{CMAKE_CURRENT_BINARY_DIR}}/sl12/Shaders/Upscaling/StreamlineDX12")
set(STREAMLINE_RUNTIME_DX12_RELATIVE_DIRECTORY "Shaders/Upscaling/StreamlineDX12")
set(STREAMLINE_RUNTIME_DX12_FILES "${{STREAMLINE_RUNTIME_DX12_DIRECTORY}}/sl12.dll")
file(GLOB_RECURSE HLSL_FILES "${{CMAKE_SOURCE_DIR}}/package/Shaders/*.hlsl")
{includes}
"""

FIXTURE = {
    "package/Shaders/Common/Color.hlsli": "",
    "package/Shaders/Tests/PackageOnly.hlsl": "",
    "features/Alpha/Shaders/Alpha.hlsl": "",
    "features/Alpha/Shaders/Sub/Helper.hlsli": "",
    "features/Alpha/Shaders/Features/Alpha.ini": "[Info]\n",
    "features/Alpha/Shaders/Tests/AlphaTest.hlsl": "",
    "features/Alpha/readme.txt": "",
    "features/Effects11/Shaders/Effects.hlsl": "",
    "features/Effects11/Shaders/Features/Effects11.ini": "[Info]\n",
    "features/NoShaders/plugin.esp": "",
}


class ShaderStagingConfigure(unittest.TestCase):
    def setUp(self):
        if not (CMAKE and NINJA):
            if os.environ.get("CI"):
                self.fail("cmake and ninja are required in CI")
            self.skipTest("cmake and ninja are required")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.work = Path(self._tmp.name)
        self.src = self.work / "src"
        self.build = self.work / "build"
        for rel, text in FIXTURE.items():
            path = self.src / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        (self.src / "features" / "Empty").mkdir()
        cmake_dir = self.src / "cmake"
        cmake_dir.mkdir()
        for name in ("CleanupStaleEntries.cmake", "PreservedRuntimePaths.cmake"):
            shutil.copy(ROOT / "cmake" / name, cmake_dir / name)

    def configure(self, effects11="OFF", skip_downloads="OFF", modules=MODULES):
        includes = "\n".join(
            f'include("{(ROOT / "cmake" / name).as_posix()}.cmake")' for name in modules
        )
        (self.src / "CMakeLists.txt").write_text(
            HARNESS.format(effects11=effects11, skip_downloads=skip_downloads, includes=includes),
            encoding="utf-8",
        )
        result = subprocess.run(
            [CMAKE, "-S", str(self.src), "-B", str(self.build), "-G", "Ninja"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def copies(self):
        """Stages the shaders for real and lists the files that land under AIO."""
        for runtime_file in ("ffx-runtime/Shaders/Upscaling/FidelityFX/ffx.dll",
                             "sl/Shaders/Upscaling/Streamline/sl.dll",
                             "sl12/Shaders/Upscaling/StreamlineDX12/sl12.dll"):
            path = self.build / runtime_file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        built = subprocess.run(
            [CMAKE, "--build", str(self.build), "--target", "prepare_shaders"],
            capture_output=True, text=True,
        )
        self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        staged = {
            path.relative_to(self.build / "aio").as_posix()
            for path in (self.build / "aio").rglob("*") if path.is_file()
        }
        self.assertTrue(staged)
        return staged

    def test_package_and_feature_shaders_are_staged_without_tests(self):
        self.configure()
        copies = self.copies()
        self.assertIn("Shaders/Common/Color.hlsli", copies)
        self.assertIn("Shaders/Alpha.hlsl", copies)
        self.assertIn("Shaders/Sub/Helper.hlsli", copies)
        self.assertIn("Shaders/Features/Alpha.ini", copies)
        self.assertFalse([c for c in copies if "/Tests/" in c])

    def test_non_shader_feature_files_are_left_to_aio_preparation(self):
        self.configure()
        self.assertNotIn("readme.txt", self.copies())

    def test_effects11_shaders_follow_the_enable_flag(self):
        self.configure(effects11="OFF")
        self.assertNotIn("Shaders/Effects.hlsl", self.copies())
        shutil.rmtree(self.build)
        self.configure(effects11="ON")
        self.assertIn("Shaders/Effects.hlsl", self.copies())

    def test_empty_feature_directories_are_dropped(self):
        result = self.configure()
        self.assertIn("Skipping empty feature directory: Empty", result.stdout)

    def test_feature_shader_path_list_names_each_staged_root(self):
        self.configure()
        listed = (self.build / "feature_shader_paths.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(listed[0], (self.build / "ffx-runtime" / "Shaders").as_posix())
        self.assertIn((self.src / "features" / "Alpha" / "Shaders").as_posix(), listed)
        self.assertNotIn((self.src / "features" / "Effects11" / "Shaders").as_posix(), listed)
        self.assertNotIn((self.src / "features" / "NoShaders").as_posix(), "".join(listed))

    def test_validation_targets_depend_on_staging(self):
        self.configure()
        targets = subprocess.run(
            [NINJA, "-C", str(self.build), "-t", "targets", "all"],
            capture_output=True, text=True, check=True,
        ).stdout
        self.assertIn("prepare_shaders:", targets)
        self.assertIn("validate_changed:", targets)
        self.assertIn("copy_shaders.stamp:", targets)
        ninja = (self.build / "build.ninja").read_text(encoding="utf-8")
        self.assertRegex(ninja, r"build validate_changed: phony .*\bprepare_shaders\b")

    def test_runtime_payload_prefixes_are_preserved_only_when_downloads_skipped(self):
        prefixes = self.build / "preserve_runtime_prefixes.txt"
        self.configure(skip_downloads="OFF")
        self.assertFalse(prefixes.exists())
        shutil.rmtree(self.build)
        self.configure(skip_downloads="ON")
        self.assertEqual(
            prefixes.read_text(encoding="utf-8").splitlines(),
            ["Upscaling/FidelityFX", "Upscaling/Streamline", "Upscaling/StreamlineDX12"],
        )


if __name__ == "__main__":
    unittest.main()

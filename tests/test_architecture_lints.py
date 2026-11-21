import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ArchitectureLintTests(unittest.TestCase):
    def test_dynamic_gpu_passes_are_limited_to_names_built_at_run_time(self):
        # CS_GPU_PASS_DYNAMIC allocates per call: fixed names use CS_GPU_PASS, _SELECT or _INDEXED.
        allowed = {
            "src/Features/Effects11.cpp": 2,
            "src/Features/Effects11/EffectManager.cpp": 1,
            "src/Features/Effects11/Effects/Effect.cpp": 1,
        }
        found = {}
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".cpp", ".h"} or path.name == "GpuPass.h":
                continue
            count = path.read_text(encoding="utf-8", errors="replace").count("CS_GPU_PASS_DYNAMIC(")
            if count:
                found[path.relative_to(ROOT).as_posix()] = count
        self.assertEqual(found, allowed)

    def test_eye_count_and_width_use_the_shared_helpers(self):
        # Hand-rolled eye math drifts per feature: use Util::EyeCount()/EyeWidth() (Utils/Game.h).
        eyes = r"(?:2u?|2\.0f|0\.5f|k\w*EyeCount)"
        flat = r"(?:1u?|1\.0f)"
        idiom = re.compile(rf"(?<!!)isVR\s*\?\s*{eyes}\s*:\s*{flat}|!\s*[\w:]*isVR\s*\?\s*{flat}\s*:\s*{eyes}")
        offenders = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".cpp", ".h"} or path.name == "Game.h":
                continue
            if idiom.search(path.read_text(encoding="utf-8", errors="replace")):
                offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_shaders_use_the_shared_eye_count(self):
        # A per-shader `#ifdef VR eyeCount = 2` duplicates kEyeCount in Common/StereoEye.hlsli.
        idiom = re.compile(r"(?:eyeCount|EyeCount)\s*=\s*[12]\s*;")
        offenders = []
        for base in (ROOT / "features", ROOT / "package/Shaders"):
            for path in sorted(base.rglob("*.hl*")):
                if path.name == "StereoEye.hlsli":
                    continue
                if idiom.search(path.read_text(encoding="utf-8", errors="replace")):
                    offenders.append(path.relative_to(ROOT).as_posix())
        self.assertEqual(offenders, [])

    def test_skylighting_warmup_covers_shader_confidence_fade(self):
        # A warmup shorter than the shader's fadeInThreshold shows half-faded probes after a reset.
        shader = (ROOT / "features/Skylighting/Shaders/Skylighting/UpdateProbesCS.hlsl").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/Skylighting.h").read_text(encoding="utf-8")
        samples = int(re.search(r"fadeInThreshold = (\d+)", shader).group(1))
        frames = int(re.search(r"probeHistoryWarmupFrames = (\d+)", header).group(1))
        self.assertGreaterEqual(frames, 4 * samples)
        self.assertEqual(frames % 4, 0)

    # How each Post Processing shader folder handles the side-by-side VR target. A new folder fails
    # until classified here and in docs/development/vr-eye-handling.md.
    POST_PROCESSING_STEREO = {
        "Border": "stereo",
        "Camera": "stereo",
        "Composite": "stereo",
        "DoF": "stereo",
        "LensFlare": "stereo",
        "LocalExposure": "stereo",
        "MotionBlur": "stereo",
        "Vignette": "stereo",
        "PhysicalLensFlare": "eye-local",
        "PhysicalLensImaging": "eye-local",
        "ColorGrading": "per-pixel",
        "LUT": "per-pixel",
        "HistogramAutoExposure": "global",
        "CODBloom": "stereo",
        "PhysicalGlare": "eye-local",
    }

    def test_post_processing_shaders_declare_their_vr_stereo_handling(self):
        shaders = ROOT / "features/Post Processing/Shaders/PostProcessing"
        folders = {p.name for p in shaders.iterdir() if p.is_dir()}
        self.assertEqual(folders, set(self.POST_PROCESSING_STEREO), "classify new folders in POST_PROCESSING_STEREO")
        for folder, kind in self.POST_PROCESSING_STEREO.items():
            text = "".join(
                p.read_text(encoding="utf-8", errors="replace")
                for p in (shaders / folder).rglob("*")
                if p.suffix in {".hlsl", ".hlsli"}
            )
            uses_stereo = "Stereo::" in text or "StereoEye.hlsli" in text
            if kind == "stereo":
                self.assertTrue(uses_stereo, f"{folder} is declared stereo but never converts eye UVs")
            elif kind in {"per-pixel", "global"}:
                self.assertFalse(uses_stereo, f"{folder} is declared {kind} but uses stereo helpers: reclassify it")
        for source in ("PhysicalLens", "PhysicalGlare"):
            text = (ROOT / f"src/Features/PostProcessing/{source}.cpp").read_text(encoding="utf-8")
            self.assertIn("eyeCount", text, f"{source} eye-local shaders need the per-eye dispatch loop")


if __name__ == "__main__":
    unittest.main()

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

    def test_skylighting_warmup_covers_shader_confidence_fade(self):
        # A warmup shorter than the shader's fadeInThreshold shows half-faded probes after a reset.
        shader = (ROOT / "features/Skylighting/Shaders/Skylighting/UpdateProbesCS.hlsl").read_text(encoding="utf-8")
        header = (ROOT / "src/Features/Skylighting.h").read_text(encoding="utf-8")
        samples = int(re.search(r"fadeInThreshold = (\d+)", shader).group(1))
        frames = int(re.search(r"probeHistoryWarmupFrames = (\d+)", header).group(1))
        self.assertGreaterEqual(frames, 4 * samples)
        self.assertEqual(frames % 4, 0)


if __name__ == "__main__":
    unittest.main()

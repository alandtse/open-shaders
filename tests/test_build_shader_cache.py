import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("build_shader_cache", ROOT / "tools/build-shader-cache.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class ShaderCacheManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / "ShaderCache"
        self.cache.mkdir()
        self.shaders = self.root / "Shaders"
        self.shaders.mkdir()
        self.config = self.root / "config.yaml"

    def write_config(self, source="Lighting.hlsl", stage="PSHADER", entries=None, common=None):
        self.config.write_text(json.dumps({"shaders": [{"file": source, "configs": {stage: {
            "common_defines": common or [],
            "entries": entries or [{"entry": "Lighting:Pixel:1", "defines": ["ALPHA"]}],
        }}}]}), encoding="utf-8")

    def test_key_matches_runtime_sort_values_and_trailing_space(self):
        self.write_config(common=["ZETA", "SHADOWSPLITCOUNT=3", "EMPTY=", "PSHADER", "VR",
                                  "D3DCOMPILE_DEBUG", "D3DCOMPILE_SKIP_OPTIMIZATION"])
        self.assertEqual(builder.cache_shader_permutations(self.config, "SE"), {
            "Lighting/1.pso": ("Lighting.hlsl", "Lighting:Pixel:ALPHA EMPTY SHADOWSPLITCOUNT=3 ZETA ")})

    def test_imagespace_uses_runtime_technique_and_keeps_source(self):
        for runtime, descriptor in (("SE", "72"), ("VR", "75")):
            with self.subTest(runtime=runtime):
                self.write_config(source="ISCompositeLensFlareVolumetricLighting.hlsl", stage="VSHADER",
                                  entries=[{"entry": f"ImageSpace:Vertex:{descriptor}", "defines": ["VL"]}])
                self.assertEqual(builder.cache_shader_permutations(self.config, runtime), {
                    f"ISCompositeVolumetricLighting/{descriptor}.vso": (
                        "ISCompositeLensFlareVolumetricLighting.hlsl", "ISCompositeVolumetricLighting:Vertex:VL ")})

    def test_conflicting_blob_defines_are_rejected(self):
        self.write_config(entries=[{"entry": "Lighting:Pixel:1", "defines": [define]}
                                   for define in ("ALPHA", "BETA")])
        with self.assertRaisesRegex(ValueError, "Conflicting shader permutations"):
            builder.cache_shader_permutations(self.config, "SE")

    def test_unmapped_vr_imagespace_keeps_its_source_name(self):
        self.write_config(source="ISFullScreenVR.hlsl", stage="VSHADER",
                          entries=[{"entry": "ImageSpace:Vertex:81", "defines": []}])
        self.assertEqual(builder.cache_shader_permutations(self.config, "VR"), {
            "ISFullScreenVR/81.vso": ("ISFullScreenVR.hlsl", "ISFullScreenVR:Vertex:")})

    def test_profile_adds_only_declared_feature_to_its_shader_type(self):
        import yaml

        headers = self.root / "src/Features"
        headers.mkdir(parents=True)
        (headers / "Sun.h").write_text(
            'std::string GetShortName() override { return "Sun"; }\n'
            'std::string_view GetShaderDefineName() override { return "TEST_SUN"; }\n'
            'bool HasShaderDefine(RE::BSShader::Type shaderType) override '
            '{ return shaderType == RE::BSShader::Type::Sky; }\n', encoding="utf-8")
        self.write_config(source="Sky.hlsl", entries=[
            {"entry": "Sky:Pixel:1", "defines": []},
            {"entry": "Lighting:Pixel:1", "defines": []}])
        original = self.config.read_bytes()
        output = self.root / "profile.yaml"
        builder.filter_profile_defines(self.config, output, set(), self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        self.assertEqual(entries[0]["defines"], ["TEST_SUN"])
        self.assertEqual(entries[1]["defines"], [])
        self.assertEqual(self.config.read_bytes(), original)
        first = output.read_bytes()
        builder.filter_profile_defines(output, output, set(), self.root)
        self.assertEqual(output.read_bytes(), first)
        builder.filter_profile_defines(output, output, {"TEST_SUN"}, self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        self.assertEqual(entries[0]["defines"], [])

    def test_profiles_follow_current_shader_declarations_and_boot_defaults(self):
        import yaml

        declared = builder.single_shader_feature_defines(ROOT)
        strip, _, _ = builder.profile_strip_defines(ROOT, "aio")
        for runtime, config in builder.CONFIGS.items():
            with self.subTest(runtime=runtime):
                output = self.root / f"profile-{runtime}.yaml"
                builder.filter_profile_defines(config, output, strip)
                for shader in yaml.safe_load(output.read_text())["shaders"]:
                    for stage in shader["configs"].values():
                        for entry in stage["entries"]:
                            family = entry["entry"].split(":", 1)[0]
                            present = {define.split("=", 1)[0] for define in
                                       stage.get("common_defines", []) + entry.get("defines", [])}
                            self.assertFalse(strip & present, entry["entry"])
                            self.assertLessEqual(declared.get(family, set()) - strip, present, entry["entry"])

    def test_finalize_requires_the_original_compile_config(self):
        with self.assertRaisesRegex(ValueError, "missing CompileConfig.yaml"):
            builder.finalize_existing(self.cache, self.shaders, "2-13-0-0", "SE", "aio")
        self.assertFalse((self.cache / "Manifest.json").exists())

    def test_captured_runtime_digest(self):
        from hlslkit.shader_digest import combine_hashes, hash_string, to_hex

        defines = ("ANISO_LIGHTING CLOUD_SHADOWS CS_HAIR CS_UTILITY DEFERRED DO_ALPHA_TEST "
                   "DYNAMIC_CUBEMAPS EFFECTS11 EXP_HEIGHT_FOG EXTENDED_MATERIALS EXTENDED_TRANSLUCENCY "
                   "GLINT IBL ISL LIGHT_LIMIT_FIX LOD_BLENDING SCREEN_SPACE_SHADOWS SKINNED SKYLIGHTING "
                   "SSS TERRAIN_BLENDING TERRAIN_SHADOWS TERRAIN_VARIATION TRUE_PBR VANILLA_FRESNEL VC "
                   "VOLUMETRIC_SHADOWS WATER_EFFECTS WETNESS_EFFECTS")
        self.write_config(entries=[{"entry": "Lighting:Pixel:11001B", "defines": defines.split()}],
                          common=["PSHADER", "D3DCOMPILE_DEBUG", "D3DCOMPILE_SKIP_OPTIMIZATION"])
        _, key = builder.cache_shader_permutations(self.config, "SE")["Lighting/11001B.pso"]
        source_and_globals = int("35d5c398b0f0357c1362c8fd70e9af19", 16)
        self.assertEqual(to_hex(combine_hashes(source_and_globals, hash_string(key))),
                         "c9257f5becbcd8d0b5c0c79218a834be")

    @unittest.skipUnless(sys.platform == "win32", "Runtime include ordering requires Windows")
    def test_manifest_changes_with_permutation_runtime_and_includes(self):
        self.write_config(entries=[{"entry": f"Lighting:Pixel:{i}", "defines": [define]}
                                   for i, define in enumerate(("ALPHA", "BETA"))])
        (self.shaders / "Lighting.hlsl").write_text('#include "Shared.hlsli"\n', encoding="utf-8")
        include = self.shaders / "Shared.hlsli"
        include.write_text("float value = 1;\n", encoding="utf-8")
        (self.cache / "Lighting").mkdir()
        for i in range(2):
            (self.cache / f"Lighting/{i}.pso").write_bytes(b"DXBC")

        def manifest(runtime):
            builder.write_shader_cache_manifest(self.cache, self.shaders, runtime, self.config)
            return json.loads((self.cache / "Manifest.json").read_text())["entries"]

        original = manifest("SE")
        self.assertEqual(len(original), 2)
        self.assertNotEqual(original["Lighting/0.pso"], original["Lighting/1.pso"])
        self.assertEqual(original, manifest("SE"))
        for path, digest in manifest("VR").items():
            self.assertNotEqual(digest, original[path])
        include.write_text("float value = 2;\n", encoding="utf-8")
        for path, digest in manifest("SE").items():
            self.assertNotEqual(digest, original[path])

    @unittest.skipUnless(sys.platform == "win32", "Runtime include ordering requires Windows")
    def test_unknown_blob_does_not_replace_manifest(self):
        self.write_config()
        (self.cache / "Lighting").mkdir()
        (self.cache / "Lighting/FFFF.pso").write_bytes(b"DXBC")
        manifest = self.cache / "Manifest.json"
        manifest.write_text("original", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "No compile configuration"):
            builder.write_shader_cache_manifest(self.cache, self.shaders, "SE", self.config)
        self.assertEqual(manifest.read_text(), "original")


if __name__ == "__main__":
    unittest.main()

import importlib.util
import json
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
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

    def test_info_ini_records_default_compile_flags(self):
        features = self.root / "stage" / "Features"
        features.mkdir(parents=True)
        (features / "Sample.ini").write_text("[Info]\nVersion = 1-0-0\n", encoding="utf-8")
        builder.write_info_ini(self.cache, self.root / "stage", "1-7-1-0", "SE")
        text = (self.cache / "Info.ini").read_bytes().decode("utf-8-sig")
        self.assertIn("PluginVersion = 1-7-1-0\r\nCompileFlags = pp=0;afc=0\r\n", text)
        self.assertIn("[Sample]\r\nEnabled = true\r\nVersion = 1-0-0", text)

    def test_companion_disable_retains_the_loaded_version(self):
        features = self.shaders / "Features"
        features.mkdir()
        (features / "HorizonFix.ini").write_text("[Info]\nVersion = 1-0-0\n", encoding="utf-8")
        builder.write_info_ini(self.cache, self.shaders, "2-19-0-0", "SE", disabled_stems={"HorizonFix"})
        self.assertIn("[HorizonFix]\r\nEnabled = false\r\nVersion = 1-0-0",
                      (self.cache / "Info.ini").read_bytes().decode("utf-8-sig"))

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

    def test_profile_adds_default_enabled_reverse_z_to_all_shader_types(self):
        import yaml

        headers = self.root / "src/Features"
        headers.mkdir(parents=True)
        (headers / "ReverseZ.h").write_text(
            'std::string GetShortName() override { return "ReverseZ"; }\n'
            'std::string_view GetShaderDefineName() override { return "REVERSE_Z"; }\n'
            'bool EnableReverseZBuffer = true;\n', encoding="utf-8")
        self.write_config(entries=[
            {"entry": "Lighting:Pixel:1", "defines": []},
            {"entry": "ImageSpace:Pixel:2", "defines": ["REVERSE_Z"]},
            {"entry": "Utility:Pixel:3", "defines": []}])
        original = self.config.read_bytes()
        output = self.root / "profile.yaml"
        builder.filter_profile_defines(self.config, output, set(), self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        for entry in entries:
            self.assertEqual(entry["defines"], ["REVERSE_Z"], entry["entry"])
        self.assertEqual(self.config.read_bytes(), original)
        first = output.read_bytes()
        builder.filter_profile_defines(output, output, set(), self.root)
        self.assertEqual(output.read_bytes(), first)
        builder.filter_profile_defines(output, output, {"REVERSE_Z"}, self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        for entry in entries:
            self.assertEqual(entry["defines"], [], entry["entry"])

    def test_profile_keeps_reverse_z_disabled_for_older_defaults(self):
        import yaml

        headers = self.root / "src/Features"
        headers.mkdir(parents=True)
        for setting in ("EnableReverseZ", "EnableReverseZBuffer"):
            with self.subTest(setting=setting):
                (headers / "ReverseZ.h").write_text(
                    'std::string GetShortName() override { return "ReverseZ"; }\n'
                    'std::string_view GetShaderDefineName() override { return "REVERSE_Z"; }\n'
                    f'bool {setting} = false;\n', encoding="utf-8")
                self.write_config(entries=[{"entry": "Lighting:Pixel:1", "defines": ["REVERSE_Z"]}])
                output = self.root / "profile.yaml"
                builder.filter_profile_defines(self.config, output, set(), self.root)
                entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
                self.assertEqual(entries[0]["defines"], [])

    def test_profile_adds_global_and_multiple_type_declarations(self):
        import yaml

        headers = self.root / "src/Features"
        headers.mkdir(parents=True)
        declarations = (("Global", "true"), ("Snow", "type == RE::BSShader::Type::Lighting || "
                                           "type == RE::BSShader::Type::Grass || "
                                           "type == RE::BSShader::Type::DistantTree"))
        for short, expression in declarations:
            (headers / f"{short}.h").write_text(
                f'std::string GetShortName() override {{ return "{short}"; }}\n'
                f'std::string_view GetShaderDefineName() override {{ return "{short.upper()}"; }}\n'
                f'bool HasShaderDefine(RE::BSShader::Type type) override {{ return {expression}; }}\n',
                encoding="utf-8")
        families = ("Lighting", "Grass", "DistantTree", "BloodSplatter", "Utility")
        self.write_config(entries=[{"entry": f"{family}:Pixel:1", "defines": []} for family in families])
        output = self.root / "profile.yaml"
        builder.filter_profile_defines(self.config, output, set(), self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        self.assertEqual([entry["defines"] for entry in entries],
                         [["GLOBAL", "SNOW"], ["GLOBAL", "SNOW"], ["GLOBAL", "SNOW"], ["GLOBAL"], []])
        builder.filter_profile_defines(output, output, {"GLOBAL"}, self.root)
        entries = yaml.safe_load(output.read_text())["shaders"][0]["configs"]["PSHADER"]["entries"]
        self.assertEqual([entry["defines"] for entry in entries], [["SNOW"], ["SNOW"], ["SNOW"], [], []])

    def test_profiles_follow_current_shader_declarations_and_boot_defaults(self):
        import yaml

        declared = builder.static_feature_defines(ROOT)
        reverse_z_default = "REVERSE_Z" in builder.default_reverse_z_defines(ROOT)
        for runtime, config in builder.CONFIGS.items():
            with self.subTest(runtime=runtime):
                strip, _, _ = builder.profile_strip_defines(ROOT, "aio", runtime)
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
                            if family != "Utility":
                                self.assertLessEqual(declared.get("*", set()) - strip, present, entry["entry"])
                            self.assertEqual("REVERSE_Z" in present, reverse_z_default, entry["entry"])
                permutations = builder.cache_shader_permutations(output, runtime)
                self.assertTrue(permutations)
                for path, (_, key) in permutations.items():
                    self.assertEqual("REVERSE_Z " in key, reverse_z_default, path)

    def write_extra_capture(self, requests):
        (self.root / "features").mkdir(exist_ok=True)
        headers = self.root / "src/Features"
        headers.mkdir(parents=True, exist_ok=True)
        (headers / "ReverseZ.h").write_text(
            'std::string GetShortName() override { return "ReverseZ"; }\n'
            'std::string_view GetShaderDefineName() override { return "REVERSE_Z"; }\n'
            'bool EnableReverseZBuffer = true;\n', encoding="utf-8")
        path = self.root / ".github/configs/shader-cache-inputs.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"schemaVersion": 1, "runtime": "SE", "requests": requests}))

    def test_portable_standalone_key_matches_production_cpp_vector(self):
        defines = [["VR", ""], ["REVERSE_Z", ""], ["VR", ""]]
        self.assertEqual(builder.standalone_compile_key("Effects11/QuadVS.hlsl", "main", "vs_5_0", 34818, defines),
                         "95db091fa85c9a6fb12d3f348b3fdd5e")

    def test_captured_standalone_macros_target_each_runtime(self):
        self.write_extra_capture([{"kind": "standalone", "source": "Feature/Main.hlsl", "runtime": "SE",
                                   "entryPoint": "main", "profile": "cs_5_0", "flags": 34818,
                                   "defines": [["COMPUTESHADER", ""], ["WINPC", ""], ["DX11", ""], ["REVERSE_Z", ""]]}])
        se = builder.extra_shader_requests(self.root, "SE")
        vr = builder.extra_shader_requests(self.root, "VR")
        self.assertEqual(len(se), 1)
        self.assertEqual(len(vr), 1)
        se_request, vr_request = next(iter(se.values())), next(iter(vr.values()))
        self.assertNotIn("VR", dict(se_request["defines"]))
        self.assertEqual(dict(vr_request["defines"])["VR"], "")
        self.assertNotEqual(se_request["key"], vr_request["key"])
        self.assertTrue(next(iter(se)).startswith("Feature/Main.hlsl.main."))
        self.assertTrue(next(iter(se)).endswith(".cso"))

    def test_default_shadow_variants_cover_native_resolutions_and_both_vr_eyes(self):
        self.write_extra_capture([{"kind": "standalone", "source": "ScreenSpaceShadows/RaymarchCS.hlsl",
                                   "runtime": "SE", "entryPoint": "main", "profile": "cs_5_0", "flags": 34818,
                                   "defines": [["COMPUTESHADER", ""], ["SAMPLE_COUNT", "80"]]}])
        se = builder.extra_shader_requests(self.root, "SE")
        self.assertLessEqual({"64", "80", "120"}, {dict(r["defines"])["SAMPLE_COUNT"] for r in se.values()})
        vr = list(builder.extra_shader_requests(self.root, "VR").values())
        self.assertEqual(len(vr), 2)
        self.assertEqual({dict(r["defines"])["SAMPLE_COUNT"] for r in vr}, {"64"})
        self.assertEqual(sum("RIGHT" in dict(r["defines"]) for r in vr), 1)

    def test_captured_engine_alias_uses_runtime_descriptor(self):
        self.write_extra_capture([{"kind": "engine", "runtime": "SE", "source": "ISLensFlareVisibility.hlsl",
                                   "shader": "LensFlareVisibility", "type": "ImageSpace", "descriptor": 101,
                                   "stage": "Pixel", "key": "LensFlareVisibility:Pixel:REVERSE_Z ",
                                   "path": "LensFlareVisibility/65.pso"}])
        request = builder.extra_shader_requests(self.root, "VR")["LensFlareVisibility/68.pso"]
        self.assertEqual(request["key"], "LensFlareVisibility:Pixel:REVERSE_Z ")
        self.assertIn(("VR", None), request["defines"])

    def test_captured_reverse_z_variants_follow_disabled_boot_default(self):
        self.write_extra_capture([{"kind": "engine", "runtime": "SE", "source": "Lighting.hlsl",
                                   "shader": "Lighting", "type": "Lighting", "descriptor": 1,
                                   "stage": "Pixel", "key": "Lighting:Pixel:REVERSE_Z ", "path": "Lighting/1.pso"},
                                  {"kind": "standalone", "source": "Main.hlsl", "runtime": "SE",
                                   "entryPoint": "main", "profile": "cs_5_0", "flags": 34818,
                                   "defines": [["COMPUTESHADER", ""], ["REVERSE_Z", ""]]}])
        header = self.root / "src/Features/ReverseZ.h"
        header.write_text(header.read_text().replace("= true;", "= false;"))
        requests = builder.extra_shader_requests(self.root, "SE")
        self.assertEqual(requests["Lighting/1.pso"]["key"], "Lighting:Pixel:")
        self.assertTrue(all("REVERSE_Z" not in dict(request["defines"]) for request in requests.values()))

    def test_captured_descriptors_can_share_identical_disk_bytecode(self):
        request = {"kind": "engine", "runtime": "SE", "source": "RunGrass.hlsl",
                   "shader": "RunGrass", "type": "Grass", "descriptor": 8, "stage": "Pixel",
                   "key": "RunGrass:Pixel:RENDER_DEPTH REVERSE_Z ", "path": "RunGrass/8.pso"}
        self.write_extra_capture([request, {**request, "descriptor": 9}])
        self.assertEqual(len(builder.extra_shader_requests(self.root, "SE")), 1)

    def test_captured_cache_rejects_unsafe_sources_and_nonrelease_flags(self):
        request = {"kind": "standalone", "runtime": "SE", "source": "../outside.hlsl", "entryPoint": "main",
                   "profile": "cs_5_0", "flags": 34818, "defines": []}
        self.write_extra_capture([request])
        with self.assertRaisesRegex(ValueError, "Unsafe captured shader source"):
            builder.extra_shader_requests(self.root, "SE")
        self.write_extra_capture([{**request, "source": "Main.hlsl", "flags": 1}])
        with self.assertRaisesRegex(ValueError, "default release"):
            builder.extra_shader_requests(self.root, "SE")

    def test_live_capture_normalizes_debug_flags_for_release(self):
        self.write_config()
        record = {"kind": "standalone", "runtime": "SE", "source": "Main.hlsl", "entryPoint": "main",
                  "profile": "cs_5_0", "flags": 3,
                  "defines": [["COMPUTESHADER", ""], ["D3DCOMPILE_DEBUG", ""], ["D3DCOMPILE_SKIP_OPTIMIZATION", ""]]}
        log = self.root / "capture.log"
        log.write_text("[D] ShaderCacheInput " + json.dumps(record))
        output = self.root / "capture.json"
        with patch.object(builder, "CONFIGS", {"SE": self.config}):
            builder.capture_shader_inputs(log, output)
        captured = json.loads(output.read_text())["requests"][0]
        self.assertEqual(captured["flags"], builder.DEFAULT_STANDALONE_FLAGS)
        self.assertEqual(captured["defines"], [["COMPUTESHADER", ""]])

    def test_supplementary_compiler_preserves_empty_macro_values(self):
        self.write_extra_capture([{"kind": "standalone", "runtime": "SE", "source": "Main.hlsl",
                                   "entryPoint": "main", "profile": "cs_5_0", "flags": 34818,
                                   "defines": [["EMPTY", ""]]}])
        with patch.object(builder, "REPO", self.root), patch.object(builder.subprocess, "run",
                return_value=Mock(returncode=0, stdout="", stderr="")) as compile_shader:
            builder.compile_extra_shaders(self.cache, self.shaders, "SE", "fxc.exe", 1)
        self.assertIn("/DEMPTY= ", compile_shader.call_args.args[0])

    def test_supplementary_compiler_rejects_unsuppressed_warnings(self):
        for source, warning, returncode, accepted in (
                ("LightLimitFix/ClusterCullingCS.hlsl", "X4714", 0, False),
                ("Other/ClusterCullingCS.hlsl", "X4714", 0, False),
                ("LightLimitFix/ClusterCullingCS.hlsl", "X3206", 0, False),
                ("Main.hlsl", "X1519", 0, True),
                ("LightLimitFix/ClusterCullingCS.hlsl", "X1519", 1, False)):
            with self.subTest(source=source, warning=warning, returncode=returncode):
                self.write_extra_capture([{"kind": "standalone", "runtime": "SE", "source": source,
                                           "entryPoint": "main", "profile": "cs_5_0", "flags": 34818,
                                           "defines": []}])
                diagnostics = f"warning {warning}: compiler diagnostic\n"
                with patch.object(builder, "REPO", self.root), patch.object(builder.subprocess, "run",
                        return_value=Mock(returncode=returncode, stdout="", stderr=diagnostics)):
                    if accepted:
                        builder.compile_extra_shaders(self.cache, self.shaders, "SE", "fxc.exe", 1)
                    else:
                        with self.assertRaisesRegex(ValueError, warning):
                            builder.compile_extra_shaders(self.cache, self.shaders, "SE", "fxc.exe", 1)

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

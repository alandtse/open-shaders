import importlib.util
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_shader_cache", ROOT / "tools/verify_shader_cache.py")
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)
builder = verifier.builder


def dxbc(stage=0):
    return b"DXBC" + bytes(16) + struct.pack("<IIII4sIII", 1, 52, 1, 36, b"SHEX", 8, stage << 16 | 0x50, 2)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.shaders = self.root / "Shaders"
        self.cache = self.root / "ShaderCache"
        (self.cache / "Sky").mkdir(parents=True)
        (self.shaders / "Features").mkdir(parents=True)
        (self.root / "src/Features").mkdir(parents=True)
        (self.root / ".github/configs").mkdir(parents=True)
        (self.root / "CMakeLists.txt").write_text("project(CommunityShaders VERSION 2.13.0)")
        (self.shaders / "Sky.hlsl").write_text("float4 main() : SV_Target { return 1; }\n")
        self.add_feature("FutureFeature", "FUTURE_FEATURE", beta=True)
        self.add_feature("ActiveFeature", "ACTIVE_FEATURE")
        config = {"shaders": [{"file": "Sky.hlsl", "configs": {"PSHADER": {
            "entries": [{"entry": "Sky:Pixel:1", "defines": ["FUTURE_FEATURE"]}]}}}]}
        self.config = self.root / ".github/configs/shader-validation.yaml"
        self.config.write_text(json.dumps(config))
        self.profile = self.root / "profile.yaml"
        strip, include, disabled = builder.profile_strip_defines(self.root, "aio")
        builder.filter_profile_defines(self.config, self.profile, strip, self.root)
        (self.cache / "Sky/1.pso").write_bytes(dxbc())
        builder.write_info_ini(self.cache, self.shaders, "2-13-0-0", "SE", include, disabled)
        if sys.platform == "win32":
            builder.write_shader_cache_manifest(self.cache, self.shaders, "SE", self.profile)

    def add_feature(self, name, define, beta=False, has_shader=True):
        ini = f"[Info]\nVersion = 1-0-0\nBeta = {str(beta)}\n[Nexus]\naio = true\n"
        feature = self.root / f"features/{name}/Shaders/Features"
        feature.mkdir(parents=True, exist_ok=True)
        (feature / f"{name}.ini").write_text(ini)
        (self.shaders / f"Features/{name}.ini").write_text(ini)
        predicate = ("bool HasShaderDefine(RE::BSShader::Type type) override "
                     "{ return type == RE::BSShader::Type::Sky; }\n") if has_shader else ""
        (self.root / f"src/Features/{name}.h").write_text(
            f'std::string GetShortName() override {{ return "{name}"; }}\n'
            f'std::string_view GetShaderDefineName() override {{ return "{define}"; }}\n' + predicate)

    def verify(self):
        return verifier.verify_artifact(self.cache, self.shaders, self.root, "SE")

    def test_future_default_disabled_feature_is_discovered_without_a_name_list(self):
        self.assertIn("FutureFeature", builder.default_disabled_features(self.root))
        self.assertEqual(verifier.read_info(self.cache / "Info.ini")["FutureFeature"]["enabled"], "false")
        key = builder.cache_shader_permutations(self.profile, "SE")["Sky/1.pso"][1]
        self.assertNotIn("FUTURE_FEATURE", key)
        self.assertIn("ACTIVE_FEATURE", key)

    def test_feature_without_shader_predicate_does_not_inject_a_macro(self):
        self.add_feature("RuntimeOnlyFeature", "RUNTIME_ONLY_FEATURE", has_shader=False)
        self.assertNotIn("RUNTIME_ONLY_FEATURE", builder.single_shader_feature_defines(self.root).get("Sky", set()))

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_valid_artifact_and_dynamic_disabled_profile(self):
        self.assertEqual(self.verify()["entries"], 1)
        self.assertIn("FutureFeature", self.verify()["defaultDisabledFeatures"])

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_future_default_disabled_feature_flip_is_rejected(self):
        info = self.cache / "Info.ini"
        info.write_text(info.read_text(encoding="utf-8-sig").replace("Enabled = false", "Enabled = true"))
        with self.assertRaisesRegex(ValueError, "FutureFeature"):
            self.verify()

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_legacy_manifest_without_permutation_digest_is_rejected(self):
        from hlslkit.shader_digest import combine_hashes, compute_shader_content_digest, hash_string, to_hex

        digest = combine_hashes(compute_shader_content_digest(self.shaders / "Sky.hlsl", self.shaders), hash_string(""))
        (self.cache / "Manifest.json").write_text(json.dumps({"schemaVersion": 1, "entries": {"Sky/1.pso": to_hex(digest)}}))
        with self.assertRaisesRegex(ValueError, "permutation digests"):
            self.verify()

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_stale_compile_profile_is_rejected_even_with_self_consistent_manifest(self):
        import yaml

        stale = yaml.safe_load(self.profile.read_text())
        stale["shaders"][0]["configs"]["PSHADER"]["entries"][0]["defines"].remove("ACTIVE_FEATURE")
        self.profile.write_text(yaml.safe_dump(stale))
        builder.write_shader_cache_manifest(self.cache, self.shaders, "SE", self.profile)
        with self.assertRaisesRegex(ValueError, "Sky/1.pso"):
            self.verify()

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_new_captured_permutation_missing_from_cache_is_rejected(self):
        config = json.loads(self.config.read_text())
        config["shaders"][0]["configs"]["PSHADER"]["entries"].append({"entry": "Sky:Pixel:2", "defines": []})
        self.config.write_text(json.dumps(config))
        with self.assertRaisesRegex(ValueError, "coverage.*Sky/2.pso"):
            self.verify()

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_source_changed_after_compilation_is_rejected(self):
        (self.shaders / "Sky.hlsl").write_text("changed source")
        with self.assertRaisesRegex(ValueError, "permutation digests"):
            self.verify()

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_added_feature_and_changed_boot_default_invalidate_old_artifacts(self):
        for name in ("NewFeature", "FutureFeature"):
            with self.subTest(feature=name):
                self.assertEqual(self.verify()["entries"], 1)
                self.add_feature(name, name.upper(), beta=False)
                with self.assertRaisesRegex(ValueError, "Feature profile"):
                    self.verify()
                _, include, disabled = builder.profile_strip_defines(self.root, "aio")
                builder.write_info_ini(self.cache, self.shaders, "2-13-0-0", "SE", include, disabled)
                with self.assertRaisesRegex(ValueError, "permutation digests"):
                    self.verify()
                strip, _, _ = builder.profile_strip_defines(self.root, "aio")
                builder.filter_profile_defines(self.config, self.profile, strip, self.root)
                builder.write_shader_cache_manifest(self.cache, self.shaders, "SE", self.profile)
                self.assertEqual(self.verify()["entries"], 1)

    def test_wrong_stage_and_truncated_bytecode_are_rejected(self):
        path = self.cache / "Sky/1.pso"
        for content, message in ((dxbc(1), "Wrong shader stage"), (dxbc()[:-1], "header")):
            with self.subTest(message=message):
                path.write_bytes(content)
                with self.assertRaisesRegex(ValueError, message):
                    verifier.check_dxbc(path)

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_snapshot_requires_a_separate_fresh_artifact(self):
        dll = self.root / "SKSE/Plugins/CommunityShaders.dll"
        dll.parent.mkdir(parents=True)
        dll.write_bytes(b"test plugin")
        log = self.root / "CommunityShaders.log"
        log.write_text("previous session")
        reference = self.root / "reference"
        shutil.copytree(self.cache, reference)
        snapshot = verifier.take_snapshot(self.root, reference, log, "SE")
        self.assertEqual(snapshot["previousLogStart"], "previous session")
        self.assertIn("SKSE/Plugins/CommunityShaders.dll", snapshot["installation"])
        with self.assertRaisesRegex(ValueError, "separate fresh build"):
            verifier.take_snapshot(self.root, self.cache, log, "SE")
        (self.cache / "Sky/1.pso").write_bytes(dxbc() + b"rewritten")
        with self.assertRaisesRegex(ValueError, "Installed vs shipped bytecode"):
            verifier.take_snapshot(self.root, reference, log, "SE")

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_finalize_requires_compile_time_record(self):
        shutil.copyfile(self.profile, self.cache / "CompileConfig.yaml")
        with patch.object(builder, "REPO", self.root), self.assertRaisesRegex(ValueError, "CompileInputs.json"):
            builder.finalize_existing(self.cache, self.shaders, "2-13-0-0", "SE", "aio")

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_recorded_compile_survives_version_only_finalization(self):
        shutil.copyfile(self.profile, self.cache / "CompileConfig.yaml")
        builder.record_compile_inputs(self.cache, self.shaders, "SE")
        with patch.object(builder, "REPO", self.root):
            builder.finalize_existing(self.cache, self.shaders, "2-14-0-0", "SE", "aio")
        result = verifier.verify_artifact(self.cache, self.shaders, self.root, "SE", "2-14-0-0")
        self.assertEqual(result["entries"], 1)
        self.assertFalse((self.cache / "CompileConfig.yaml").exists())
        self.assertFalse((self.cache / "CompileInputs.json").exists())

    @unittest.skipUnless(sys.platform == "win32", "Windows digest ordering")
    def test_finalize_cannot_relabel_stale_bytecode_with_current_inputs(self):
        shutil.copyfile(self.profile, self.cache / "CompileConfig.yaml")
        builder.record_compile_inputs(self.cache, self.shaders, "SE")
        manifest = (self.cache / "Manifest.json").read_bytes()
        changes = ((self.shaders / "Sky.hlsl", b"changed shader source"),
                   (self.cache / "Sky/1.pso", dxbc() + b"changed bytecode"),
                   (self.cache / "CompileConfig.yaml", self.profile.read_bytes().replace(b"ACTIVE_FEATURE", b"NEW_DEFINE")))
        for path, content in changes:
            with self.subTest(path=path.name):
                original = path.read_bytes()
                try:
                    path.write_bytes(content)
                    with patch.object(builder, "REPO", self.root), self.assertRaisesRegex(ValueError, "changed since compilation"):
                        builder.finalize_existing(self.cache, self.shaders, "2-13-0-0", "SE", "aio")
                    self.assertEqual((self.cache / "Manifest.json").read_bytes(), manifest)
                finally:
                    path.write_bytes(original)
        with patch.object(builder, "REPO", self.root), self.assertRaisesRegex(ValueError, "changed since compilation"):
            builder.finalize_existing(self.cache, self.shaders, "2-13-0-0", "VR", "aio")


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.snapshot = {"created_ns": 100, "previousLogStart": "old session",
                         "cache": {"info": {"Cache": {"pluginversion": "2-13-0-0"}}}}
        self.log = "[19:00:00] [I] CommunityShaders v2-13-0-0\n[I] Using disk cache\n"
        self.status = {"compiling": False, "failedTasks": 0, "currentFailedCount": 0,
                       "digestMissTasks": 0, "totalTasks": 10, "completedTasks": 10,
                       "diskHitTasks": 10, "digestHitTasks": 10, "cacheMismatches": [],
                       "diskCacheHeld": False, "featureSetChanged": False, "featureSetRevertPending": False}

    def test_clean_session_and_unrelated_effect_error(self):
        verifier.verify_log(self.log + "[EFFECTS11] Required effect file not found: 'enbeffect.fx'\n", self.snapshot, 101)
        verifier.check_runtime_status(self.status)

    def test_no_fresh_session_cannot_pass(self):
        with self.assertRaisesRegex(ValueError, "predates"):
            verifier.verify_log(self.log, self.snapshot, 99)
        self.snapshot["previousLogStart"] = self.log.splitlines()[0]
        with self.assertRaisesRegex(ValueError, "fresh game session"):
            verifier.verify_log(self.log, self.snapshot, 101)

    def test_log_version_must_match_exactly(self):
        for version in ("2-13-0-01", "2-14-0-0"):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "plugin version"):
                verifier.verify_log(self.log.replace("2-13-0-0", version), self.snapshot, 101)

    def test_mismatch_and_compile_are_rejected_even_with_zero_failures(self):
        for message in ("Disk cache mismatch: Future Feature - installed/enabled now",
                        "Shader compilation started (10 tasks queued)",
                        "Shader compilation completed: 10/10 tasks (0 failed) in 00:00:42"):
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, "compiled or invalidated"):
                verifier.verify_log(self.log + message, self.snapshot, 101)

    def test_live_cache_counters_must_prove_reuse(self):
        for key, value in {"compiling": True, "failedTasks": 1, "currentFailedCount": 1,
                           "digestMissTasks": 1, "totalTasks": 0, "completedTasks": 9,
                           "diskHitTasks": 9, "digestHitTasks": 0, "diskCacheHeld": True,
                           "cacheMismatches": [{"shortName": "FutureFeature"}]}.items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                verifier.check_runtime_status({**self.status, key: value})
        for value in (None, True, "10", -1):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "Invalid live counter"):
                verifier.check_runtime_status({**self.status, "totalTasks": value})

    def test_identical_bytecode_rewrite_is_detected_by_timestamp(self):
        with self.assertRaisesRegex(ValueError, "Sky/1.pso"):
            verifier.compare("Cache blobs", {"Sky/1.pso": {"sha256": "same", "mtime_ns": 1}},
                             {"Sky/1.pso": {"sha256": "same", "mtime_ns": 2}})

    def test_live_probe_rejects_wrong_runtime_before_other_queries(self):
        from io import BytesIO

        with patch.object(verifier.urllib.request, "urlopen", return_value=BytesIO(b'{"vr":true,"playerLoaded":true}')) as request:
            with self.assertRaisesRegex(ValueError, "Wrong live Skyrim runtime"):
                verifier.live_status("http://127.0.0.1:8920", "SE")
            self.assertEqual(request.call_count, 1)

    def test_live_probe_requires_loaded_player_and_advancing_frames(self):
        from io import BytesIO

        for player_loaded, frame in ((False, 2), (True, 1), (True, 2)):
            with self.subTest(player_loaded=player_loaded, frame=frame):
                responses = [{"vr": False, "playerLoaded": player_loaded},
                             {"plugin": "CommunityShaders", "vr": False, "frame_count": 1},
                             {"plugin": "CommunityShaders", "vr": False, "frame_count": frame}, self.status]
                streams = [BytesIO(json.dumps(response).encode()) for response in responses]
                with patch.object(verifier.urllib.request, "urlopen", side_effect=streams) as request, \
                        patch.object(verifier.time, "sleep"):
                    if player_loaded and frame > 1:
                        self.assertEqual(verifier.live_status("http://127.0.0.1:8920", "SE"), self.status)
                        kinds = [json.loads(call.args[0].data)["kind"] for call in request.call_args_list]
                        self.assertEqual(kinds, ["state", "openshaders", "openshaders", "shadercache"])
                    else:
                        with self.assertRaises(ValueError):
                            verifier.live_status("http://127.0.0.1:8920", "SE")

    def test_session_checks_all_files_after_live_counters_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "CommunityShaders.log"
            log.write_text(self.log)
            cache = {"entries": {"Sky/1.pso": "a" * 32}, "info": self.snapshot["cache"]["info"],
                     "blobs": {"Sky/1.pso": {"sha256": "same", "mtime_ns": 1}}}
            snapshot = {**self.snapshot, "schemaVersion": 1, "runtime": "SE", "data": str(root),
                        "log": str(log), "cache": cache, "installation": {"Shaders/Sky.hlsl": "same"}}
            with patch.object(verifier, "live_status", return_value=self.status), \
                    patch.object(verifier, "installation_hashes", return_value=snapshot["installation"]), \
                    patch.object(verifier, "cache_snapshot", return_value=cache):
                self.assertEqual(verifier.verify_session(snapshot, "http://localhost:8920")["unchangedEntries"], 1)
                for label, value in (("entries", {"Sky/1.pso": "b" * 32}),
                                     ("blobs", {"Sky/1.pso": {"sha256": "same", "mtime_ns": 2}}),
                                     ("blobs", {**cache["blobs"], "Sky/2.pso": {"sha256": "new", "mtime_ns": 2}})):
                    with patch.object(verifier, "cache_snapshot", return_value={**cache, label: value}), \
                            self.assertRaisesRegex(ValueError, "changed during game session"):
                        verifier.verify_session(snapshot, "http://localhost:8920")

                def compile_during_inspection(_):
                    log.write_text(self.log + "Shader compilation started (10 tasks queued)\n")
                    return cache

                with patch.object(verifier, "cache_snapshot", side_effect=compile_during_inspection), \
                        self.assertRaisesRegex(ValueError, "compiled or invalidated"):
                    verifier.verify_session(snapshot, "http://localhost:8920")
                log.write_text(self.log)
                with patch.object(verifier, "live_status", side_effect=[self.status, {**self.status, "totalTasks": 11}]), \
                        self.assertRaisesRegex(ValueError, "workload changed"):
                    verifier.verify_session(snapshot, "http://localhost:8920")


if __name__ == "__main__":
    unittest.main()

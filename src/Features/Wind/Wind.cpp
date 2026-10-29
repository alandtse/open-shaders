#include "Wind.h"

#include "Features/Wind/TransientWindImpulse.h"
#include "Features/Wind/WindMath.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Trees/TreeWindPatcher.h"
#include "Utils/DevBenchUx.h"
#include "Utils/Format.h"

#include "RE/B/BSGeometry.h"
#include "RE/B/BSLeafAnimNode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

using namespace WindSettingsLimits;

namespace
{
	constexpr uint32_t kNearTransientFieldMask = 1u << 0u;
	constexpr uint32_t kMidTransientFieldMask = 1u << 1u;
	constexpr uint32_t kFarTransientFieldMask = 1u << 2u;

	constexpr auto kTreeBendDescriptor = static_cast<uint32_t>(State::ExtraShaderDescriptors::TreeBend);

	bool IsExplicitTrunkGeometry(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return false;

		const char* geometryName = a_geometry->name.c_str();
		return geometryName && (std::strcmp(geometryName, "trunk") == 0 || std::strncmp(geometryName, "OS_TRUNK", 8) == 0);
	}

	bool IsExplicitTreeLeavesGeometry(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return false;

		const char* geometryName = a_geometry->name.c_str();
		return geometryName && std::strcmp(geometryName, "leaves") == 0 &&
		       netimmerse_cast<RE::BSLeafAnimNode*>(a_geometry->parent);
	}

	bool IsTreeGeometry(const RE::BSGeometry* a_geometry)
	{
		return IsExplicitTrunkGeometry(a_geometry) || IsExplicitTreeLeavesGeometry(a_geometry);
	}

	bool SupportsTreeBend(const RE::BSShader& a_shader, uint32_t a_vertexDescriptor)
	{
		if (a_shader.shaderType == RE::BSShader::Type::Lighting) {
			const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_vertexDescriptor >> 24) & 0x3F);
			return technique != SIE::ShaderCache::LightingShaderTechniques::LODObjects &&
			       technique != SIE::ShaderCache::LightingShaderTechniques::LODObjectHD;
		}

		return a_shader.shaderType == RE::BSShader::Type::Utility &&
		       (a_vertexDescriptor & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::LodObject)) == 0;
	}

	uint32_t GetRenderPassVertexDescriptor(const RE::BSRenderPass& a_pass)
	{
		constexpr uint32_t kLightingTechniqueStart = 0x4800002D;
		if (a_pass.shader && a_pass.shader->shaderType == RE::BSShader::Type::Lighting && a_pass.passEnum >= kLightingTechniqueStart)
			return a_pass.passEnum - kLightingTechniqueStart;
		return a_pass.passEnum;
	}

	bool IsTreeRenderPass(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->shader || !a_pass->geometry)
			return false;

		const auto shaderType = a_pass->shader->shaderType.get();
		return (shaderType == RE::BSShader::Type::Lighting || shaderType == RE::BSShader::Type::Utility) &&
		       IsTreeGeometry(a_pass->geometry);
	}
}

void Wind::SanitizeSettings(Settings& a_settings)
{
	const Settings defaults{};
	a_settings.trunkWindBendSensitivity = Util::ClampFinite(a_settings.trunkWindBendSensitivity,
		kTrunkWindSensitivityMin, kTrunkWindSensitivityMax, defaults.trunkWindBendSensitivity);
	a_settings.treeLeafBaseWindFlutterGain = Util::ClampFinite(a_settings.treeLeafBaseWindFlutterGain,
		kTreeLeafBaseWindFlutterGainMin, kTreeLeafBaseWindFlutterGainMax, defaults.treeLeafBaseWindFlutterGain);
	a_settings.treeWindGustScale = Util::ClampFinite(a_settings.treeWindGustScale,
		kTreeWindGustScaleMin, kTreeWindGustScaleMax, defaults.treeWindGustScale);
	a_settings.treeWindGustSoftLimit = Util::ClampFinite(a_settings.treeWindGustSoftLimit,
		kTreeWindGustSoftLimitMin, kTreeWindGustSoftLimitMax, defaults.treeWindGustSoftLimit);
	a_settings.treeWindSpringFrequency = Util::ClampFinite(a_settings.treeWindSpringFrequency,
		kTreeWindSpringFrequencyMin, kTreeWindSpringFrequencyMax, defaults.treeWindSpringFrequency);
	a_settings.treeWindSpringDamping = Util::ClampFinite(a_settings.treeWindSpringDamping,
		kTreeWindSpringDampingMin, kTreeWindSpringDampingMax, defaults.treeWindSpringDamping);
	a_settings.treeTransientSpringFrequency = Util::ClampFinite(a_settings.treeTransientSpringFrequency,
		kTreeTransientSpringFrequencyMin, kTreeTransientSpringFrequencyMax, defaults.treeTransientSpringFrequency);
	a_settings.treeTransientSpringDamping = Util::ClampFinite(a_settings.treeTransientSpringDamping,
		kTreeTransientSpringDampingMin, kTreeTransientSpringDampingMax, defaults.treeTransientSpringDamping);
	a_settings.windFieldGustScale = Util::ClampFinite(a_settings.windFieldGustScale,
		kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
	a_settings.windFieldGustCrosswindScale = Util::ClampFinite(a_settings.windFieldGustCrosswindScale,
		kWindFieldGustCrosswindScaleMin, kWindFieldGustCrosswindScaleMax, defaults.windFieldGustCrosswindScale);
	a_settings.windFieldGustAmplitude = Util::ClampFinite(a_settings.windFieldGustAmplitude,
		kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, defaults.windFieldGustAmplitude);
	a_settings.windFieldGustAdvectionMultiplier = Util::ClampFinite(a_settings.windFieldGustAdvectionMultiplier,
		kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, defaults.windFieldGustAdvectionMultiplier);
	a_settings.windFieldGustCoverage = Util::ClampFinite(a_settings.windFieldGustCoverage,
		kWindFieldGustCoverageMin, kWindFieldGustCoverageMax, defaults.windFieldGustCoverage);
	a_settings.windFieldGustEdgeSoftness = Util::ClampFinite(a_settings.windFieldGustEdgeSoftness,
		kWindFieldGustEdgeSoftnessMin, kWindFieldGustEdgeSoftnessMax, defaults.windFieldGustEdgeSoftness);
	a_settings.windFieldGustDistortionStrength = Util::ClampFinite(a_settings.windFieldGustDistortionStrength,
		kWindFieldGustDistortionStrengthMin, kWindFieldGustDistortionStrengthMax, defaults.windFieldGustDistortionStrength);
	a_settings.windFieldGustDistortionScale = Util::ClampFinite(a_settings.windFieldGustDistortionScale,
		kWindFieldGustDistortionScaleMin, kWindFieldGustDistortionScaleMax, defaults.windFieldGustDistortionScale);
	a_settings.windFieldGustDistortionSpeed = Util::ClampFinite(a_settings.windFieldGustDistortionSpeed,
		kWindFieldGustDistortionSpeedMin, kWindFieldGustDistortionSpeedMax, defaults.windFieldGustDistortionSpeed);
	for (uint32_t index = 0; index < a_settings.windFieldGustAdvectionResponse.size(); ++index)
		a_settings.windFieldGustAdvectionResponse[index] = Util::ClampFinite(
			a_settings.windFieldGustAdvectionResponse[index], kWindResponseMin, kWindResponseMax,
			defaults.windFieldGustAdvectionResponse[index]);
	a_settings.windFieldDirectionTransitionDuration = Util::ClampFinite(a_settings.windFieldDirectionTransitionDuration,
		kWindFieldDirectionTransitionDurationMin, kWindFieldDirectionTransitionDurationMax,
		defaults.windFieldDirectionTransitionDuration);
	a_settings.grassTransientFlutterStrength = Util::ClampFinite(a_settings.grassTransientFlutterStrength,
		kGrassTransientFlutterStrengthMin, kGrassTransientFlutterStrengthMax,
		defaults.grassTransientFlutterStrength);
	a_settings.grassTransientFlutterFrequency = Util::ClampFinite(a_settings.grassTransientFlutterFrequency,
		kGrassTransientFlutterFrequencyMin, kGrassTransientFlutterFrequencyMax,
		defaults.grassTransientFlutterFrequency);
	SanitizeGrassWindSettings(a_settings);
}

uint32_t Wind::GetTransientFieldMask() const
{
	return kNearTransientFieldMask |
	       (settings.processMidRangeTransients ? kMidTransientFieldMask : 0u) |
	       (settings.processFarRangeTransients ? kFarTransientFieldMask : 0u);
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings::GrassWindSpringQualityRange,
	textureSize,
	maxDistance)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Wind::Settings,
	enableTrunkBend,
	overrideTrunkWindIntensity,
	trunkWindIntensityOverride,
	trunkWindBendSensitivity,
	treeLeafBaseWindFlutterGain,
	treeWindGustScale,
	treeWindGustSoftLimit,
	treeWindSpringFrequency,
	treeWindSpringDamping,
	treeTransientSpringFrequency,
	treeTransientSpringDamping,
	windFieldGustScale,
	windFieldGustCrosswindScale,
	windFieldGustAmplitude,
	windFieldGustAdvectionMultiplier,
	windFieldGustAdvectionResponse,
	windFieldGustCoverage,
	windFieldGustEdgeSoftness,
	enableWindFieldGustDistortion,
	windFieldGustDistortionStrength,
	windFieldGustDistortionScale,
	windFieldGustDistortionSpeed,
	windFieldDirectionTransitionDuration,
	processMidRangeTransients,
	processFarRangeTransients,
	grassTransientBendStrength,
	grassTransientFlutterHalfLife,
	grassTransientFlutterStrength,
	grassTransientFlutterFrequency,
	enableAmbientGrassWind,
	enableGrassWindSpring,
	enableGrassWindSpringBend,
	grassWindResponse,
	grassWindSensitivity,
	grassWindMaximumTilt,
	grassWindCompressionToBend,
	grassWindSpringFrequency,
	grassWindSpringDamping,
	grassWindSpringQuality,
	grassWindFlutterStrength,
	grassWindFlutterFrequency,
	grassWindFlutterGustInfluence,
	grassWindFlutterWaveScale,
	grassWindFlutterAmplitudeResponse)

void Wind::SetTreeWindTestEnabled(bool a_enabled)
{
	if (runtimeState.treeWindTest.enabled == a_enabled)
		return;

	if (a_enabled) {
		runtimeState.treeWindTest.speed = Util::ClampFinite(windFieldSelectedSpeed, 0.0f, 2.0f, 1.0f);
		runtimeState.treeWindTest.gustScale = Util::ClampFinite(windFieldTuning.gustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, settings.windFieldGustScale);
		runtimeState.treeWindTest.gustAmplitude = Util::ClampFinite(windFieldTuning.gustAmplitude,
			kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, settings.windFieldGustAmplitude);
		runtimeState.treeWindTest.gustAdvectionMultiplier = Util::ClampFinite(
			windFieldTuning.gustAdvectionMultiplier,
			kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax, settings.windFieldGustAdvectionMultiplier);
	}
	runtimeState.treeWindTest.enabled = a_enabled;
}

json Wind::GetDiagnostics()
{
	const auto [universalOverrideEnabled, universalValues] = TreeWindPatcher::GetUniversalOverride();
	return json{
		{ "treeWindRuleCount", TreeWindPatcher::GetRuleCount() },
		{ "treeWindUnsavedRuleCount", TreeWindPatcher::GetUnsavedRuleCount() },
		{ "treeWindConflictingFiles", TreeWindPatcher::GetConflictingFiles() },
		{ "treeWindTestEnabled", runtimeState.treeWindTest.enabled },
		{ "treeWindTestSpeed", runtimeState.treeWindTest.speed },
		{ "treeWindTestGustScale", runtimeState.treeWindTest.gustScale },
		{ "treeWindTestGustAmplitude", runtimeState.treeWindTest.gustAmplitude },
		{ "treeWindTestGustAdvectionMultiplier", runtimeState.treeWindTest.gustAdvectionMultiplier },
		{ "treeWindGustScale", settings.treeWindGustScale },
		{ "treeWindGustSoftLimit", settings.treeWindGustSoftLimit },
		{ "treeWindSpringFrequency", settings.treeWindSpringFrequency },
		{ "treeWindSpringDamping", settings.treeWindSpringDamping },
		{ "treeTransientSpringFrequency", settings.treeTransientSpringFrequency },
		{ "treeTransientSpringDamping", settings.treeTransientSpringDamping },
		{ "processMidRangeTransients", settings.processMidRangeTransients },
		{ "processFarRangeTransients", settings.processFarRangeTransients },
		{ "grassTransientBendStrength", settings.grassTransientBendStrength },
		{ "grassTransientFlutterHalfLife", settings.grassTransientFlutterHalfLife },
		{ "grassTransientFlutterStrength", settings.grassTransientFlutterStrength },
		{ "grassTransientFlutterFrequency", settings.grassTransientFlutterFrequency },
		{ "universalTreeResponseOverride", universalOverrideEnabled },
		{ "universalTreeResponse", {
									   { "bendSensitivity", universalValues.bend },
									   { "leafAmbientSensitivity", universalValues.leafAmbient },
									   { "upperBendRange", universalValues.upperBendRange },
									   { "maximumDisplacementPercent", universalValues.maximumDisplacementPercent },
									   { "trunkGustInfluence", universalValues.trunkGustInfluence },
									   { "leafGustInfluence", universalValues.leafGustInfluence },
									   { "transientWindInfluence", universalValues.transientWindInfluence },
									   { "leafTransientWindInfluence", universalValues.leafTransientWindInfluence },
									   { "leafTransientFlutterMaximum", universalValues.leafTransientFlutterMaximum },
									   { "transientMaximumBendMultiplier", universalValues.transientMaximumBendMultiplier },
								   } },
		{ "treeLeafBaseWindFlutterGain", settings.treeLeafBaseWindFlutterGain },
	};
}

void Wind::RegisterUxActions()
{
	FEATURE_COMMAND("setTransientShapes",
		"Show active transient volumes with the menu closed. Requires the global overlay toggle; flat-screen only. Args: enabled (boolean).",
		[](Feature* feature, const json& args) {
			if (args.contains("enabled") && args["enabled"].is_boolean())
				static_cast<Wind*>(feature)->SetRuntimeFlag("TransientShapes", args["enabled"].get<bool>());
		});
	FEATURE_QUERY("transientSources",
		"Inspect active renderer transient sources, including banked wakes. Returns type, origin, direction, strength, extent, width, vertical reach and roll. No args; available in SE, AE and VR.",
		([](const Feature* feature, const json&) -> json {
			const auto* wind = static_cast<const Wind*>(feature);
			json sources = json::array();
			for (uint32_t index = 0; index < wind->activeTransientWindImpulseCount; ++index) {
				const auto& source = wind->transientWindImpulses[index];
				sources.push_back({ { "type", static_cast<uint32_t>(source.type) },
					{ "origin", { source.origin.x, source.origin.y, source.origin.z } },
					{ "direction", { source.direction.x, source.direction.y, source.direction.z } },
					{ "strength", source.strength }, { "maxDistance", source.maxDistance },
					{ "wavefrontDistance", source.wavefrontDistance }, { "waveHalfWidth", source.waveHalfWidth },
					{ "collisionRadius", source.collisionRadius }, { "verticalScale", source.verticalScale },
					{ "orientationRoll", source.orientationRoll } });
			}
			return { { "providerConnected", wind->provider != nullptr }, { "sources", sources } };
		}));
	FEATURE_COMMAND("setGrassTransientBendStrength",
		"Set grass transient bend strength from 0 to 180 degrees per unit, independently of ambient bending. Args: strength (number).",
		[](Feature* feature, const json& args) {
			if (!args.contains("strength") || !args["strength"].is_number())
				return;
			auto* wind = static_cast<Wind*>(feature);
			wind->settings.grassTransientBendStrength = Util::ClampFinite(
				args["strength"].get<float>(), kGrassWindResponseMin, kGrassWindResponseMax,
				wind->settings.grassTransientBendStrength);
		});
	FEATURE_COMMAND("setGrassTransientFlutterHalfLife",
		"Set grass impulse flutter decay half-life from 0.01 to 2 seconds. Args: halfLife (number).",
		[](Feature* feature, const json& args) {
			if (!args.contains("halfLife") || !args["halfLife"].is_number())
				return;
			auto* wind = static_cast<Wind*>(feature);
			wind->settings.grassTransientFlutterHalfLife = Util::ClampFinite(
				args["halfLife"].get<float>(), kGrassTransientFlutterHalfLifeMin, kGrassTransientFlutterHalfLifeMax,
				wind->settings.grassTransientFlutterHalfLife);
		});
	FEATURE_COMMAND("setGrassTransientFlutterStrength",
		"Set the additive grass impulse flutter strength from 0 to 2. Args: strength (number). Zero disables the extra oscillation without changing spring bending.",
		[](Feature* feature, const json& args) {
			if (!args.contains("strength") || !args["strength"].is_number())
				return;
			auto* wind = static_cast<Wind*>(feature);
			wind->settings.grassTransientFlutterStrength = Util::ClampFinite(
				args["strength"].get<float>(), kGrassTransientFlutterStrengthMin,
				kGrassTransientFlutterStrengthMax, wind->settings.grassTransientFlutterStrength);
		});
	FEATURE_COMMAND("setGrassTransientFlutterFrequency",
		"Set the additive grass impulse flutter frequency from 0.25 to 12 Hz. Args: frequency (number).",
		[](Feature* feature, const json& args) {
			if (!args.contains("frequency") || !args["frequency"].is_number())
				return;
			auto* wind = static_cast<Wind*>(feature);
			wind->settings.grassTransientFlutterFrequency = Util::ClampFinite(
				args["frequency"].get<float>(), kGrassTransientFlutterFrequencyMin,
				kGrassTransientFlutterFrequencyMax, wind->settings.grassTransientFlutterFrequency);
		});
	FEATURE_COMMAND("setUniversalTreeResponse",
		"Set the runtime-only response used for every tree. All response params and enabled are optional.",
		[](Feature* feature, const json& args) {
			json request = args;
			request["action"] = "setUniversalTreeResponse";
			const auto result = static_cast<Wind*>(feature)->RunProviderCommand(request);
			if (!result.value("success", false))
				logger::warn("Open Wind authoring command failed: {}", result.dump());
		});
	FEATURE_COMMAND("setTreeWindRule",
		"Apply live per-model tree wind tuning. Params: mesh (string), bendSensitivity and leafAmbientSensitivity (0-4), "
		"upperBendRange (5-100), maximumDisplacementPercent (0-10), trunkGustInfluence and leafGustInfluence (0-2), "
		"transientWindInfluence, leafTransientWindInfluence, "
		"leafTransientFlutterMaximum (0-20), and transientMaximumBendMultiplier (0-5).",
		[](Feature* feature, const json& args) {
			json request = args;
			request["action"] = "setTreeRule";
			const auto result = static_cast<Wind*>(feature)->RunProviderCommand(request);
			if (!result.value("success", false))
				logger::warn("Open Wind authoring command failed: {}", result.dump());
		});
	FEATURE_COMMAND("setTreeBendEnabled", "Enable or disable the tree-bending render path for one mesh, retaining its saved sensitivity and normal TreeAnim animation. Required args: mesh (string), enabled (boolean). Disabled meshes remain excluded under Global Override.",
		[](Feature* feature, const json& args) {
			json request = args;
			request["action"] = "setTreeBendEnabled";
			const auto result = static_cast<Wind*>(feature)->RunProviderCommand(request);
			if (!result.value("success", false))
				logger::warn("Open Wind authoring command failed: {}", result.dump());
		});
	FEATURE_COMMAND("saveTreeWindRules", "Write changed tree response values to each tree's last-scanned source JSON.",
		[](Feature* feature, const json& args) {
			json request = args;
			request["action"] = "saveTreeRules";
			const auto result = static_cast<Wind*>(feature)->RunProviderCommand(request);
			if (!result.value("success", false))
				logger::warn("Open Wind authoring command failed: {}", result.dump());
		});
	FEATURE_COMMAND("revertTreeWindRules", "Revert live tree wind edits made since the JSON was loaded or saved.",
		[](Feature* feature, const json& args) {
			json request = args;
			request["action"] = "revertTreeRules";
			const auto result = static_cast<Wind*>(feature)->RunProviderCommand(request);
			if (!result.value("success", false))
				logger::warn("Open Wind authoring command failed: {}", result.dump());
		});
	FEATURE_COMMAND("setTreeWindTestConditions",
		"Set runtime-only tree wind test conditions. Optional params: enabled (boolean), speed (0-2), gustScale (128-16384), gustAmplitude (0-1), gustAdvectionMultiplier (0-8).",
		[](Feature* feature, const json& args) {
			auto* utility = static_cast<Wind*>(feature);
			if (args.contains("enabled") && args["enabled"].is_boolean())
				utility->SetTreeWindTestEnabled(args["enabled"].get<bool>());
			if (args.contains("speed") && args["speed"].is_number())
				utility->runtimeState.treeWindTest.speed = Util::ClampFinite(args["speed"].get<float>(), 0.0f, 2.0f, utility->runtimeState.treeWindTest.speed);
			if (args.contains("gustScale") && args["gustScale"].is_number())
				utility->runtimeState.treeWindTest.gustScale = Util::ClampFinite(args["gustScale"].get<float>(),
					kWindFieldGustScaleMin, kWindFieldGustScaleMax, utility->runtimeState.treeWindTest.gustScale);
			if (args.contains("gustAmplitude") && args["gustAmplitude"].is_number())
				utility->runtimeState.treeWindTest.gustAmplitude = Util::ClampFinite(args["gustAmplitude"].get<float>(),
					kWindFieldGustAmplitudeMin, kWindFieldGustAmplitudeMax, utility->runtimeState.treeWindTest.gustAmplitude);
			if (args.contains("gustAdvectionMultiplier") && args["gustAdvectionMultiplier"].is_number())
				utility->runtimeState.treeWindTest.gustAdvectionMultiplier = Util::ClampFinite(args["gustAdvectionMultiplier"].get<float>(),
					kWindFieldGustAdvectionMultiplierMin, kWindFieldGustAdvectionMultiplierMax,
					utility->runtimeState.treeWindTest.gustAdvectionMultiplier);
		});
	FEATURE_QUERY("treeWindRules",
		"Search live tree wind rules. Params: search (string, optional), offset (integer, default 0), limit (integer, 1-500, default 100).",
		([](const Feature* feature, const json& args) -> json {
			json request = args;
			request["action"] = "treeRules";
			return static_cast<const Wind*>(feature)->RunProviderCommand(request);
		}));
}

json Wind::GetRuntimeFlags()
{
	const bool universalOverrideEnabled = TreeWindPatcher::GetUniversalOverride().first;
	return json{
		{ "VisualizeWindField", runtimeState.visualizeWindField },
		{ "TransientShapes", uiState.showTransientShapes },
		{ "WindFieldUseRealSpeed", runtimeState.windFieldUseRealSpeed },
		{ "WindFieldUseRealDirection", runtimeState.windFieldUseRealDirection },
		{ "TreeWindTestOverride", runtimeState.treeWindTest.enabled },
		{ "UniversalTreeResponseOverride", universalOverrideEnabled },
	};
}

bool Wind::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "TransientShapes") {
		uiState.showTransientShapes = a_value;
		return true;
	}

	if (a_name == "VisualizeWindField") {
		runtimeState.visualizeWindField = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealSpeed") {
		runtimeState.windFieldUseRealSpeed = a_value;
		return true;
	}
	if (a_name == "WindFieldUseRealDirection") {
		runtimeState.windFieldUseRealDirection = a_value;
		return true;
	}
	if (a_name == "TreeWindTestOverride") {
		SetTreeWindTestEnabled(a_value);
		return true;
	}
	if (a_name == "UniversalTreeResponseOverride") {
		const auto [enabled, values] = TreeWindPatcher::GetUniversalOverride();
		(void)enabled;
		TreeWindPatcher::SetUniversalOverride(a_value, values);
		return true;
	}
	return false;
}

void Wind::LoadSettings(json& o_json)
{
	const Settings defaults{};
	settings = o_json;
	if (!o_json.contains("windFieldGustCrosswindScale")) {
		const float legacyGustScale = Util::ClampFinite(settings.windFieldGustScale,
			kWindFieldGustScaleMin, kWindFieldGustScaleMax, defaults.windFieldGustScale);
		settings.windFieldGustCrosswindScale = legacyGustScale * WindField::WindTuning{}.frontAspectRatio;
	}
	if (!o_json.contains("grassWindSpringQuality")) {
		if (o_json.contains("grassWindSpringTextureSize") && o_json["grassWindSpringTextureSize"].is_number_unsigned()) {
			const auto legacyTextureSize = o_json["grassWindSpringTextureSize"].get<uint32_t>();
			for (auto& range : settings.grassWindSpringQuality)
				range.textureSize = legacyTextureSize;
		}
		if (o_json.contains("grassWindSpringWorldSize") && o_json["grassWindSpringWorldSize"].is_number()) {
			const float legacyWorldSize = o_json["grassWindSpringWorldSize"].get<float>();
			if (std::isfinite(legacyWorldSize))
				settings.grassWindSpringQuality.back().maxDistance = legacyWorldSize * 0.5f;
		}
	}
	if (!o_json.contains("grassWindSpringFrequency") && o_json.contains("grassWindSpringLag") &&
		o_json["grassWindSpringLag"].is_number()) {
		const float legacyLag = std::max(o_json["grassWindSpringLag"].get<float>(), 0.01f);
		settings.grassWindSpringFrequency = 0.25f / legacyLag;
	}
	if (!o_json.contains("grassWindSpringDamping"))
		settings.grassWindSpringDamping = defaults.grassWindSpringDamping;
	if (!o_json.contains("treeLeafBaseWindFlutterGain")) {
		if (o_json.contains("treeLeafWindSensitivity") && o_json["treeLeafWindSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafWindSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
		else if (o_json.contains("treeLeafAmbientSensitivity") && o_json["treeLeafAmbientSensitivity"].is_number())
			settings.treeLeafBaseWindFlutterGain =
				o_json["treeLeafAmbientSensitivity"].get<float>() * defaults.treeLeafBaseWindFlutterGain;
	}
	if (uiProvider) {
		json document;
		if (ReadProviderSettings(document)) {
			if (o_json.contains("windEffects"))
				document["windEffects"] = o_json["windEffects"];
			const auto text = document.dump();
			uiProvider->LoadSettings(text.c_str());
			RefreshProviderSettings();
		}
	} else {
		pendingProviderSettings = o_json;
	}
	SanitizeSettings(settings);
	CommitProviderSettings();
}

void Wind::SaveSettings(json& o_json)
{
	SanitizeSettings(settings);
	o_json = settings;
	json document;
	if (ReadProviderSettings(document) && document.contains("windEffects"))
		o_json["windEffects"] = document["windEffects"];
	else if (pendingProviderSettings.contains("windEffects"))
		o_json["windEffects"] = pendingProviderSettings["windEffects"];
}

void Wind::RestoreDefaultSettings()
{
	settings = {};
	if (uiProvider) {
		uiProvider->RestoreDefaults();
		RefreshProviderSettings();
	}
	CommitProviderSettings();
	TreeWindPatcher::SetUniversalOverride(false, {});
}

void Wind::RestoreCurrentPageDefaultSettings()
{
	const Settings defaults{};
	switch (uiState.activeSettingsPage) {
	case SettingsPage::WindField:
		settings.windFieldGustScale = defaults.windFieldGustScale;
		settings.windFieldGustCrosswindScale = defaults.windFieldGustCrosswindScale;
		settings.windFieldGustAmplitude = defaults.windFieldGustAmplitude;
		settings.windFieldGustAdvectionMultiplier = defaults.windFieldGustAdvectionMultiplier;
		settings.windFieldGustAdvectionResponse = defaults.windFieldGustAdvectionResponse;
		settings.windFieldGustCoverage = defaults.windFieldGustCoverage;
		settings.windFieldGustEdgeSoftness = defaults.windFieldGustEdgeSoftness;
		settings.enableWindFieldGustDistortion = defaults.enableWindFieldGustDistortion;
		settings.windFieldGustDistortionStrength = defaults.windFieldGustDistortionStrength;
		settings.windFieldGustDistortionScale = defaults.windFieldGustDistortionScale;
		settings.windFieldGustDistortionSpeed = defaults.windFieldGustDistortionSpeed;
		settings.windFieldDirectionTransitionDuration = defaults.windFieldDirectionTransitionDuration;
		break;
	case SettingsPage::WindEffects:
		settings.processMidRangeTransients = defaults.processMidRangeTransients;
		settings.processFarRangeTransients = defaults.processFarRangeTransients;
		(void)RunProviderAction({ { "action", "resetEffect" }, { "id", uiState.activeWindEffect } });
		break;
	case SettingsPage::Trees:
		settings.enableTrunkBend = defaults.enableTrunkBend;
		settings.trunkWindBendSensitivity = defaults.trunkWindBendSensitivity;
		settings.treeLeafBaseWindFlutterGain = defaults.treeLeafBaseWindFlutterGain;
		settings.treeWindGustScale = defaults.treeWindGustScale;
		settings.treeWindGustSoftLimit = defaults.treeWindGustSoftLimit;
		settings.treeWindSpringFrequency = defaults.treeWindSpringFrequency;
		settings.treeWindSpringDamping = defaults.treeWindSpringDamping;
		settings.treeTransientSpringFrequency = defaults.treeTransientSpringFrequency;
		settings.treeTransientSpringDamping = defaults.treeTransientSpringDamping;
		break;
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		TreeWindPatcher::SetUniversalOverride(false, {});
		break;
	case SettingsPage::Grass:
		ResetGrassWindSettings();
		break;
	}
}

bool Wind::ReapplyCurrentPageOverrideSettings()
{
	static constexpr std::array<std::string_view, 12> windFieldKeys{
		"windFieldGustScale",
		"windFieldGustCrosswindScale",
		"windFieldGustAmplitude",
		"windFieldGustAdvectionMultiplier",
		"windFieldGustAdvectionResponse",
		"windFieldGustCoverage",
		"windFieldGustEdgeSoftness",
		"enableWindFieldGustDistortion",
		"windFieldGustDistortionStrength",
		"windFieldGustDistortionScale",
		"windFieldGustDistortionSpeed",
		"windFieldDirectionTransitionDuration"
	};
	static constexpr std::array<std::string_view, 3> windEffectKeys{
		"processMidRangeTransients",
		"processFarRangeTransients",
		"windEffects"
	};
	static constexpr std::array<std::string_view, 9> treeKeys{
		"enableTrunkBend",
		"trunkWindBendSensitivity",
		"treeLeafBaseWindFlutterGain",
		"treeWindGustScale",
		"treeWindGustSoftLimit",
		"treeWindSpringFrequency",
		"treeWindSpringDamping",
		"treeTransientSpringFrequency",
		"treeTransientSpringDamping"
	};
	static constexpr std::array<std::string_view, 22> grassKeys{
		"grassTransientBendStrength",
		"grassTransientFlutterHalfLife",
		"grassTransientFlutterStrength",
		"grassTransientFlutterFrequency",
		"overrideTrunkWindIntensity",
		"trunkWindIntensityOverride",
		"enableAmbientGrassWind",
		"enableGrassWindSpring",
		"enableGrassWindSpringBend",
		"grassWindResponse",
		"grassWindSensitivity",
		"grassWindMaximumTilt",
		"grassWindCompressionToBend",
		"grassWindSpringFrequency",
		"grassWindSpringDamping",
		"grassWindSpringQuality",
		"grassWindFlutterStrength",
		"grassWindFlutterFrequency",
		"grassWindFlutterGustInfluence",
		"grassWindFlutterWaveScale",
		"grassWindFlutterAmplitudeResponse"
	};

	switch (uiState.activeSettingsPage) {
	case SettingsPage::WindField:
		return ReapplyOverrideSettingsForKeys(windFieldKeys);
	case SettingsPage::WindEffects:
		return ReapplyOverrideSettingsForKeys(windEffectKeys);
	case SettingsPage::Trees:
		return ReapplyOverrideSettingsForKeys(treeKeys);
	case SettingsPage::TreeMeshes:
		TreeWindPatcher::RevertUnsavedChanges();
		TreeWindPatcher::SetUniversalOverride(false, {});
		return true;
	case SettingsPage::Grass:
		return ReapplyOverrideSettingsForKeys(grassKeys);
	}
	return false;
}

void Wind::SetupResources()
{
	SetupGrassWindResources();
	SetupTreeWindResources();
}

bool Wind::IsTreeBendRenderPass(const RE::BSRenderPass* a_pass) const
{
	if (!loaded || !provider || !settings.enableTrunkBend || !IsTreeRenderPass(a_pass) ||
		!SupportsTreeBend(*a_pass->shader, GetRenderPassVertexDescriptor(*a_pass)))
		return false;

	return true;
}

std::function<void()> Wind::OnRenderPassBegin(const RE::BSRenderPass* a_pass)
{
	auto* state = globals::state;
	if (!state || !IsTreeBendRenderPass(a_pass))
		return nullptr;

	auto& permutationData = state->permutationData;
	const auto previous = permutationData;

	const auto sensitivities = TreeWindPatcher::GetSensitivities(a_pass->geometry);
	if (!sensitivities.bendEnabled)
		return nullptr;
	permutationData.ExtraShaderDescriptor |= kTreeBendDescriptor;
	permutationData.TreeBendModelSensitivity = sensitivities.bend;
	permutationData.TreeLeafModelSensitivity = sensitivities.leafAmbient;
	permutationData.TreeWindUpperBendRange = sensitivities.upperBendRange;
	permutationData.TreeWindMaximumDisplacementPercent = sensitivities.maximumDisplacementPercent;
	permutationData.TreeWindTrunkGustInfluence = sensitivities.trunkGustInfluence;
	permutationData.TreeLeafGustInfluence = sensitivities.leafGustInfluence;
	permutationData.TreeTransientWindInfluence = sensitivities.transientWindInfluence;
	permutationData.TreeLeafTransientWindInfluence = sensitivities.leafTransientWindInfluence;
	permutationData.TreeLeafTransientFlutterMaximum = sensitivities.leafTransientFlutterMaximum;
	permutationData.TreeTransientMaximumBendMultiplier = sensitivities.transientMaximumBendMultiplier;
	if (sensitivities.hasBounds) {
		permutationData.TreeWindBoundsBase = sensitivities.boundMinimumZ;
		permutationData.TreeWindBoundsHeight = sensitivities.boundHeight;
		permutationData.TreeWindProbeBase = float4(sensitivities.probeBase.x, sensitivities.probeBase.y, sensitivities.probeBase.z, 0.0f);
		permutationData.TreeWindProbeTop = float4(sensitivities.probeTop.x, sensitivities.probeTop.y, sensitivities.probeTop.z, 0.0f);
	} else {
		permutationData.TreeWindBoundsBase = 0.0f;
		permutationData.TreeWindBoundsHeight = 0.0f;
		permutationData.TreeWindProbeBase = float4();
		permutationData.TreeWindProbeTop = float4();
	}
	UpdateTreeWindSpring();

	state->UpdatePermutationBuffer();
	state->BindVertexPermutationData(a_pass->shader);

	return [state, previous]() {
		state->permutationData.ExtraShaderDescriptor = previous.ExtraShaderDescriptor;
		state->permutationData.TreeBendModelSensitivity = previous.TreeBendModelSensitivity;
		state->permutationData.TreeLeafModelSensitivity = previous.TreeLeafModelSensitivity;
		state->permutationData.TreeWindUpperBendRange = previous.TreeWindUpperBendRange;
		state->permutationData.TreeWindMaximumDisplacementPercent = previous.TreeWindMaximumDisplacementPercent;
		state->permutationData.TreeWindBoundsBase = previous.TreeWindBoundsBase;
		state->permutationData.TreeWindBoundsHeight = previous.TreeWindBoundsHeight;
		state->permutationData.TreeWindProbeBase = previous.TreeWindProbeBase;
		state->permutationData.TreeWindProbeTop = previous.TreeWindProbeTop;
		state->permutationData.TreeWindTrunkGustInfluence = previous.TreeWindTrunkGustInfluence;
		state->permutationData.TreeLeafGustInfluence = previous.TreeLeafGustInfluence;
		state->permutationData.TreeTransientWindInfluence = previous.TreeTransientWindInfluence;
		state->permutationData.TreeLeafTransientWindInfluence = previous.TreeLeafTransientWindInfluence;
		state->permutationData.TreeLeafTransientFlutterMaximum = previous.TreeLeafTransientFlutterMaximum;
		state->permutationData.TreeTransientMaximumBendMultiplier = previous.TreeTransientMaximumBendMultiplier;
		state->UpdatePermutationBuffer();
	};
}

Wind::PerFrameData Wind::GetCommonBufferData() const
{
	return {
		provider && runtimeState.visualizeWindField ? 1u : 0u,
		static_cast<uint32_t>(runtimeState.windFieldDebugView),
		{}
	};
}

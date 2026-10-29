#include "FurShells.h"

#include "Features/FurShells/AssetScan.h"
#include "Features/Wind/Wind.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <Tracy/Tracy.hpp>

#define I18N_KEY_PREFIX "feature.fur_shells."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FurShells::Settings,
	Enabled,
	ShellCount,
	Length,
	BodyLength,
	Droop,
	RootThreshold,
	TipThreshold,
	RootDarkening,
	ShellColor,
	FadeStart,
	FadeEnd,
	HideCoveredFur,
	OverlayFur,
	MinFurPixels,
	WindStrength,
	WetStrength,
	InertiaStrength,
	InertiaPreset,
	InertiaFrequency,
	InertiaGain,
	InertiaLengthExponent,
	InertiaDroopExponent,
	InertiaMaxBend,
	TouchStrength,
	ShowTouchEdges)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FurShells::ShellOverride,
	Enabled,
	ShellCount,
	Length,
	Droop,
	RootThreshold,
	TipThreshold,
	RootDarkening,
	ShellColor,
	FadeStart,
	FadeEnd)

namespace
{
	constexpr uint32_t FurFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::FurShells);
	constexpr uint32_t FurDepthFlag = FurFlag | static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::FurShellsDepth);
	constexpr uint32_t ModelSpaceNormalsFlag = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals);
	constexpr uint32_t ReflectionsFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);

	constexpr uint32_t MaxShells = 32;
	constexpr uint32_t ShellTextureSlot = 124;
	constexpr uint32_t NormalTextureSlot = 126;
	constexpr uint32_t DepthTextureSlot = 125;
	constexpr uint32_t VertexPerPassSlot = 7;
	constexpr uint32_t PixelPerPassSlot = 10;
	constexpr uint32_t ResolvesPerFrame = 16;
	constexpr size_t MaxEntries = 8192;
	constexpr size_t MaxMissingShells = 65536;
	constexpr uint32_t InertiaExpireFrames = 120;
	constexpr uint32_t MaxOverrideSize = 65536;
	constexpr float PixelsPerShell = 1.0f;
	constexpr float MaxWindSpeed = 1.5f;
	constexpr float TouchActorRange = 800.0f;
	constexpr float TouchReach = 10.0f;

	/** @brief Reports, per skinned mesh of an actor, whether the engine kept CPU copies of vertex positions and skin weights. */
	json ProbeSkinData(RE::Actor* a_actor)
	{
		constexpr size_t MaxMeshes = 48;
		json meshes = json::array();
		RE::NiAVObject* thirdPersonRoot = a_actor->Get3D(false);
		RE::NiAVObject* firstPersonRoot = a_actor->Get3D(true);
		const auto findIn = [](RE::NiAVObject* a_root, const char* a_name) -> const RE::NiAVObject* {
			RE::NiNode* node = a_root ? a_root->AsNode() : nullptr;
			return node ? node->GetObjectByName(RE::BSFixedString(a_name)) : nullptr;
		};
		Util::ForEachActorGeometry(a_actor, [&](RE::BSGeometry* a_geometry) {
			if (meshes.size() >= MaxMeshes)
				return;
			const auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
			if (!skin)
				return;
			json entry = { { "name", a_geometry->name.c_str() ? a_geometry->name.c_str() : "" }, { "hasSkinData", skin->skinData != nullptr }, { "bones", skin->skinData ? skin->skinData->GetBoneCount() : 0u } };
			if (skin->bones) {
				json names = json::array();
				uint32_t handBones = 0;
				uint32_t inThirdPerson = 0;
				uint32_t inFirstPerson = 0;
				for (uint32_t bone = 0; bone < skin->numMatrices; ++bone) {
					const char* boneName = skin->bones[bone] && skin->bones[bone]->name.c_str() ? skin->bones[bone]->name.c_str() : "";
					const std::string_view view{ boneName };
					if (Util::BodyCapsules::IsHandBone(view)) {
						++handBones;
						inThirdPerson += skin->bones[bone] == findIn(thirdPersonRoot, boneName) ? 1u : 0u;
						inFirstPerson += skin->bones[bone] == findIn(firstPersonRoot, boneName) ? 1u : 0u;
					}
					if (names.size() < 24)
						names.push_back(boneName);
				}
				entry["handBones"] = handBones;
				entry["handBonesInThirdPersonTree"] = inThirdPerson;
				entry["handBonesInFirstPersonTree"] = inFirstPerson;
				entry["boneNames"] = std::move(names);
			}
			if (const auto* partition = skin->skinPartition.get()) {
				entry["vertexCount"] = partition->vertexCount;
				json parts = json::array();
				for (uint32_t index = 0; index < partition->numPartitions && index < 8; ++index) {
					const auto& part = partition->partitions[index];
					RE::BSGraphics::VertexDesc desc = part.vertexDesc;
					parts.push_back({ { "vertices", part.vertices }, { "vertexSize", desc.GetSize() }, { "skinnedVertex", desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) }, { "buffData", part.buffData != nullptr }, { "rawVertexData", part.buffData && part.buffData->rawVertexData != nullptr }, { "cpuBones", part.bones != nullptr }, { "cpuWeights", part.weights != nullptr }, { "bonesPerVertex", part.bonesPerVertex } });
				}
				entry["partitions"] = std::move(parts);
			}
			meshes.push_back(std::move(entry));
		});
		json trees = json::object();
		for (const auto& [label, root] : { std::pair{ "thirdPerson", thirdPersonRoot }, std::pair{ "firstPerson", firstPersonRoot } }) {
			json sides = json::object();
			for (const char* name : { "NPC R Hand [RHnd]", "NPC L Hand [LHnd]" }) {
				const auto* node = findIn(root, name);
				sides[name] = node ? json{ node->world.translate.x, node->world.translate.y, node->world.translate.z } : json(nullptr);
			}
			trees[label] = std::move(sides);
		}
		return json{ { "handNodePositionsByTree", std::move(trees) }, { "sameRoot", thirdPersonRoot == firstPersonRoot }, { "meshes", std::move(meshes) } };
	}

	/** @brief World positions of each hand node and its middle fingertip joint, or null when not found. */
	json HandPositionsJson(const Util::BodyCapsules::Tracker& a_tracker)
	{
		json result = json::object();
		for (const bool left : { true, false }) {
			float3 hand;
			float3 joint;
			result[left ? "left" : "right"] = a_tracker.GetHandPositions(left, hand, joint) ?
			                                      json{ { "hand", { hand.x, hand.y, hand.z } }, { "middleFingerDistalJoint", { joint.x, joint.y, joint.z } } } :
			                                      json(nullptr);
		}
		return result;
	}

	/** @brief Each hand's finger joint positions, fitted radii and fingertip extents, for checking which fingers are tracked. */
	json HandFingersJson(const Util::BodyCapsules::Tracker& a_tracker)
	{
		json result = json::object();
		for (const bool left : { true, false }) {
			const auto report = a_tracker.GetHandReport(left);
			json fingers = json::array();
			for (size_t finger = 0; finger < report.Fingers; ++finger) {
				json joints = json::array();
				for (size_t joint = 0; joint < report.Joints; ++joint) {
					const size_t index = finger * report.Joints + joint;
					joints.push_back(report.found[index] ?
										 json{ { "position", { report.position[index].x, report.position[index].y, report.position[index].z } }, { "radius", report.radius[index] } } :
										 json(nullptr));
				}
				fingers.push_back({ { "joints", std::move(joints) }, { "tipExtent", report.tipExtent[finger] } });
			}
			result[left ? "left" : "right"] = { { "palmRadius", report.palmRadius }, { "fingers", std::move(fingers) } };
		}
		return result;
	}

	/** @brief What HIGGS returned for each hand's weapon, grabbed object and hand body, to see why a held item is missing. */
	json HeldJson(const Util::BodyCapsules::Tracker& a_tracker)
	{
		json result = json::object();
		for (const bool left : { true, false }) {
			json slots = json::object();
			size_t slot = 0;
			for (const char* name : { "weapon", "grabbed", "hand" }) {
				const auto report = a_tracker.GetHeldReport(left, slot++);
				slots[name] = { { "interface", report.available }, { "body", report.body }, { "inWorld", report.inWorld }, { "shapeType", report.shapeType }, { "capsule", report.capsule }, { "children", report.children }, { "parts", report.parts }, { "drawn", report.drawn }, { "collisionDisabled", report.collisionDisabled } };
			}
			result[left ? "left" : "right"] = std::move(slots);
		}
		return result;
	}

	/** @brief The capsules each fur mesh took, with the strength that dents fade to. */
	template <class T>
	json SelectionsJson(const std::vector<T>& a_selections)
	{
		json result = json::array();
		for (const auto& selection : a_selections) {
			json capsules = json::array();
			for (const auto& capsule : selection.capsules) {
				capsules.push_back({ { "a", { capsule.pointA.x, capsule.pointA.y, capsule.pointA.z } }, { "b", { capsule.pointB.x, capsule.pointB.y, capsule.pointB.z } }, { "radius", capsule.radius }, { "side", capsule.side }, { "appendage", capsule.appendage }, { "strength", capsule.strength } });
			}
			result.push_back({ { "mesh", selection.mesh }, { "capsules", std::move(capsules) } });
		}
		return result;
	}

	/** @brief Form ID of the reference that owns a mesh, found by walking up to the root that carries the reference, or 0. */
	RE::FormID GetWearer(const RE::NiAVObject* a_object)
	{
		constexpr int MaxDepth = 16;
		for (int depth = 0; a_object && depth < MaxDepth; ++depth, a_object = a_object->parent) {
			if (const auto* reference = a_object->GetUserData())
				return reference->GetFormID();
		}
		return 0;
	}
	constexpr std::string_view EmptyOverlayPath = "textures\\actors\\character\\overlays\\default.dds";

	bool IsFirstPersonPass()
	{
		auto* accumulator = *globals::game::currentAccumulator.get();
		return accumulator && accumulator->GetRuntimeData().renderMode == RE::BSShaderAccumulator::RENDER_MODE::kFirstPersonView;
	}

	bool CanDrawShells(RE::BSGeometry* a_geometry)
	{
		if (a_geometry->GetGeometryRuntimeData().skinInstance)
			return true;

		const auto type = a_geometry->GetType().get();
		return type == RE::BSGeometry::Type::kTriShape || type == RE::BSGeometry::Type::kMeshLODTriShape;
	}

	bool IsFurTechnique(uint32_t a_descriptor)
	{
		using enum SIE::ShaderCache::LightingShaderTechniques;

		switch (static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_descriptor >> 24) & 0x3F)) {
		case None:
		case Envmap:
		case Glowmap:
		case Parallax:
		case Facegen:
		case FacegenRGBTint:
		case ParallaxOcc:
		case MultilayerParallax:
			return true;
		default:
			return false;
		}
	}

	bool IsSkinTechnique(uint32_t a_descriptor)
	{
		using enum SIE::ShaderCache::LightingShaderTechniques;

		const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>((a_descriptor >> 24) & 0x3F);
		return technique == Facegen || technique == FacegenRGBTint;
	}

	float GetFurPixels(float a_length, float a_distance)
	{
		const float projection = std::abs(Util::GetCameraData(0).projMat.m[1][1]);
		const float height = Util::ConvertToDynamic({ 0.0f, static_cast<float>(globals::game::graphicsState->screenHeight) }).y;
		const float pixels = a_length * 0.5f * height * projection / std::max(a_distance, 1.0f);
		return std::isfinite(pixels) ? pixels : static_cast<float>(MaxShells) * PixelsPerShell;
	}

	std::string NormalizeTexturePath(const char* a_path)
	{
		std::string path = a_path;
		std::ranges::transform(path, path.begin(), [](char a_char) {
			return a_char == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(a_char)));
		});
		if (path.starts_with("data\\"))
			path.erase(0, 5);
		if (!path.starts_with("textures\\"))
			path.insert(0, "textures\\");
		return path;
	}

	DXGI_FORMAT GetDepthViewFormat(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R24G8_TYPELESS:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32G8X24_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R16_TYPELESS:
			return DXGI_FORMAT_R16_UNORM;
		default:
			return DXGI_FORMAT_UNKNOWN;
		}
	}

	std::string GetOverridePath(const std::string& a_shellPath)
	{
		return a_shellPath.substr(0, a_shellPath.size() - 4) + ".json";
	}

	void ClampOverride(FurShells::ShellOverride& a_values)
	{
		a_values.ShellCount = std::clamp(a_values.ShellCount, 1u, MaxShells);
		a_values.Length = std::clamp(a_values.Length, 0.05f, 6.0f);
		a_values.Droop = std::clamp(a_values.Droop, 0.0f, 2.0f);
		a_values.RootThreshold = std::clamp(a_values.RootThreshold, 0.0f, 1.0f);
		a_values.TipThreshold = std::clamp(a_values.TipThreshold, 0.0f, 1.0f);
		a_values.RootDarkening = std::clamp(a_values.RootDarkening, 0.0f, 1.0f);
		a_values.ShellColor = std::clamp(a_values.ShellColor, 0.0f, 1.0f);
		a_values.FadeStart = std::clamp(a_values.FadeStart, 0.0f, 8000.0f);
		a_values.FadeEnd = std::clamp(a_values.FadeEnd, 0.0f, 8000.0f);
	}

	RE::NiSourceTexturePtr LoadShell(const std::string& a_path, bool& a_exists, std::string& a_shellPath)
	{
		constexpr std::string_view pbrPrefix = "textures\\pbr\\";

		a_exists = false;
		if (!a_path.ends_with(".dds"))
			return nullptr;

		const std::string stem = a_path.substr(0, a_path.size() - 4);
		std::array<std::string, 2> candidates{ stem + "_shell.dds", std::string() };
		if (stem.starts_with(pbrPrefix))
			candidates[1] = "textures\\" + stem.substr(pbrPrefix.size()) + "_shell.dds";

		for (const auto& candidate : candidates) {
			if (candidate.empty() || !RE::BSResourceNiBinaryStream(candidate).good())
				continue;
			a_exists = true;

			RE::NiPointer<RE::NiTexture> texture;
			RE::BSShaderManager::GetTexture(candidate.c_str(), true, texture, false);
			if (texture && texture->GetRTTI() == globals::rtti::NiSourceTextureRTTI.get()) {
				logger::info("[Fur Shells] {} uses {}", a_path, candidate);
				a_shellPath = candidate;
				return RE::NiSourceTexturePtr(static_cast<RE::NiSourceTexture*>(texture.get()));
			}
		}

		return nullptr;
	}
}

void FurShells::RestoreDefaultSettings()
{
	settings = {};
}

void FurShells::LoadSettings(json& o_json)
{
	settings = o_json;

	const auto tuning = FurInertia::Sanitize({ settings.InertiaFrequency, settings.InertiaGain, settings.InertiaLengthExponent, settings.InertiaDroopExponent, settings.InertiaMaxBend });
	settings.InertiaFrequency = tuning.frequencyHz;
	settings.InertiaGain = tuning.gain;
	settings.InertiaLengthExponent = tuning.lengthExponent;
	settings.InertiaDroopExponent = tuning.droopExponent;
	settings.InertiaMaxBend = tuning.maxBend;
	settings.InertiaStrength = Util::ClampFinite(settings.InertiaStrength, 0.0f, FurInertia::MaxStrength, Settings{}.InertiaStrength);
}

void FurShells::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FurShells::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("enabled_tooltip"), "Draws fur shells on meshes whose texture has a _shell.dds beside it."));

	int shellCount = static_cast<int>(settings.ShellCount);
	if (ImGui::SliderInt(T(TKEY("shell_count"), "Shells"), &shellCount, 1, static_cast<int>(MaxShells), "%d", ImGuiSliderFlags_AlwaysClamp))
		settings.ShellCount = static_cast<uint32_t>(shellCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shell_count_tooltip"), "Layers drawn up close. Each one is another lighting pass over the fur."));

	ImGui::SliderFloat(T(TKEY("length"), "Length"), &settings.Length, 0.1f, 6.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("length_tooltip"), "Fur length in game units."));

	ImGui::SliderFloat(T(TKEY("body_length"), "Body Length"), &settings.BodyLength, 0.05f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("body_length_tooltip"), "Fur length on skin, such as khajiit bodies and heads, in game units."));

	ImGui::SliderFloat(T(TKEY("droop"), "Droop"), &settings.Droop, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("droop_tooltip"), "How far the tips sag, in game units."));

	ImGui::SliderFloat(T(TKEY("root_threshold"), "Root Cutoff"), &settings.RootThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("root_threshold_tooltip"), "Shell texture alpha needed at the root. Lower is denser."));

	ImGui::SliderFloat(T(TKEY("tip_threshold"), "Tip Cutoff"), &settings.TipThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("tip_threshold_tooltip"), "Shell texture alpha needed at the tip. Higher thins the strands toward the end."));

	ImGui::SliderFloat(T(TKEY("root_darkening"), "Root Brightness"), &settings.RootDarkening, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("root_darkening_tooltip"), "Brightness at the root. Tips stay at full brightness."));

	ImGui::SliderFloat(T(TKEY("shell_color"), "Shell Texture Color"), &settings.ShellColor, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shell_color_tooltip"), "0 colors the fur from the armor's own texture, 1 from the shell texture."));

	ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("fade_tooltip"), "Shells thin out between these distances and stop past the end."));

	ImGui::Checkbox(T(TKEY("hide_covered_fur"), "Hide Fur Under Clothing"), &settings.HideCoveredFur);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("hide_covered_fur_tooltip"), "Skips fur whose roots are covered by clothing, armor or anything else in front of them, so it stops poking through. Works in the world view, not in first person or menus."));

	ImGui::SliderFloat(T(TKEY("min_fur_pixels"), "Minimum Fur Size"), &settings.MinFurPixels, 0.0f, 16.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("min_fur_pixels_tooltip"), "Fur thins out as its on-screen height falls below this many pixels and is skipped at zero. 0 disables the cutoff."));

	ImGui::SliderFloat(T(TKEY("wind_strength"), "Wind Strength"), &settings.WindStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("wind_strength_tooltip"), "How strongly the world wind bends the fur. Fur is only blown outdoors. 0 disables wind."));

	ImGui::SliderFloat(T(TKEY("wet_strength"), "Wet Fur Strength"), &settings.WetStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("wet_strength_tooltip"), "How much rain mats the fur: wet fur thins into clumps, darkens and hangs heavier. Water beads and drips on the outer layer come from Wetness Effects. 0 disables wet fur."));

	DrawInertiaSettings();

	ImGui::SliderFloat(T(TKEY("touch_strength"), "Touch Strength"), &settings.TouchStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("touch_strength_tooltip"), "How strongly hands, fingers and bodies press and drag the fur they touch, including fur on your own gear in VR. 0 disables touch."));

	ImGui::Checkbox(T(TKEY("show_touch_edges"), "Show Touch Edges"), &settings.ShowTouchEdges);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("show_touch_edges_tooltip"), "Tints touched fur to show how far a hand or body reaches: red inside the touching shape, magenta along the edge of the affected fur, cyan to yellow as the push grows. For tuning only."));

	ImGui::Checkbox(T(TKEY("overlay_fur"), "Show Overlays On Fur"), &settings.OverlayFur);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("overlay_fur_tooltip"), "Carries RaceMenu overlays such as tattoos, body paint and fur patterns up through the fur, so they color it instead of staying hidden under it. Each overlay in use draws the fur once more."));

	ImGui::Text("%s: %u", T(TKEY("fur_passes"), "Fur passes last frame"), lastPassCount);

	DrawOverrideSettings();
}

void FurShells::DrawOverrideSettings()
{
	if (!ImGui::TreeNodeEx(T(TKEY("shell_overrides"), "Shell Texture Overrides"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	if (ImGui::BeginCombo(T(TKEY("shell_texture"), "Shell Texture"), selectedShellName.c_str())) {
		for (auto& [name, data] : shellTextures) {
			if (ImGui::Selectable(name.c_str(), name == selectedShellName)) {
				selectedShellName = name;
				selectedShell = &data;
				overrideStatus.clear();
			}
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("shell_texture_tooltip"), "Shell textures drawn this session. If one is missing, bring its armor or creature on screen first."));

	if (selectedShell) {
		auto& values = selectedShell->values;
		if (!selectedShell->overridden)
			values = GetGlobalValues(selectedShell->skin);

		ImGui::TextDisabled("%s", selectedShell->overridden ? T(TKEY("override_active"), "This texture uses its own values.") : T(TKEY("override_inactive"), "No override yet. This texture follows the settings above."));

		bool edited = ImGui::Checkbox(T(TKEY("override_enabled"), "Enabled"), &values.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("override_enabled_tooltip"), "Turn off to stop fur growing on meshes that use this shell texture."));

		int shellCount = static_cast<int>(values.ShellCount);
		if (ImGui::SliderInt(T(TKEY("shell_count"), "Shells"), &shellCount, 1, static_cast<int>(MaxShells), "%d", ImGuiSliderFlags_AlwaysClamp)) {
			values.ShellCount = static_cast<uint32_t>(shellCount);
			edited = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shell_count_tooltip"), "Layers drawn up close. Each one is another lighting pass over the fur."));

		edited |= ImGui::SliderFloat(T(TKEY("length"), "Length"), &values.Length, 0.05f, 6.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("length_tooltip"), "Fur length in game units."));

		edited |= ImGui::SliderFloat(T(TKEY("droop"), "Droop"), &values.Droop, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("droop_tooltip"), "How far the tips sag, in game units."));

		edited |= ImGui::SliderFloat(T(TKEY("root_threshold"), "Root Cutoff"), &values.RootThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("root_threshold_tooltip"), "Shell texture alpha needed at the root. Lower is denser."));

		edited |= ImGui::SliderFloat(T(TKEY("tip_threshold"), "Tip Cutoff"), &values.TipThreshold, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("tip_threshold_tooltip"), "Shell texture alpha needed at the tip. Higher thins the strands toward the end."));

		edited |= ImGui::SliderFloat(T(TKEY("root_darkening"), "Root Brightness"), &values.RootDarkening, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("root_darkening_tooltip"), "Brightness at the root. Tips stay at full brightness."));

		edited |= ImGui::SliderFloat(T(TKEY("shell_color"), "Shell Texture Color"), &values.ShellColor, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("shell_color_tooltip"), "0 colors the fur from the armor's own texture, 1 from the shell texture."));

		edited |= ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &values.FadeStart, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		edited |= ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &values.FadeEnd, 0.0f, 8000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("fade_tooltip"), "Shells thin out between these distances and stop past the end."));

		if (edited) {
			selectedShell->overridden = true;
			UpdateOverrideFadeEnd();
		}

		if (ImGui::Button(T(TKEY("create_override"), "Create Override"))) {
			selectedShell->overridden = true;
			UpdateOverrideFadeEnd();
			overrideStatusFailed = !SaveOverride(selectedShellName, values, overrideStatus);
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("create_override_tooltip"), "Saves these values as a .json beside the shell texture. Mod Organizer 2 puts the new file in Overwrite, so the texture's mod stays untouched."));

		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("discard_override"), "Discard Changes"))) {
			LoadOverride(selectedShellName, *selectedShell);
			overrideStatus.clear();
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("discard_override_tooltip"), "Reloads the saved override for this texture, or returns it to the settings above when none is saved."));

		if (!overrideStatus.empty()) {
			if (overrideStatusFailed)
				ImGui::TextColored(Util::Colors::GetError(), "%s: %s", T(TKEY("override_failed"), "Could not write"), overrideStatus.c_str());
			else
				ImGui::TextColored(Util::Colors::GetSuccess(), "%s: %s", T(TKEY("override_saved"), "Saved"), overrideStatus.c_str());
		}
	}

	ImGui::TreePop();
}

FurShells::ShellOverride FurShells::GetGlobalValues(bool a_skin) const
{
	return {
		.Enabled = true,
		.ShellCount = settings.ShellCount,
		.Length = a_skin ? settings.BodyLength : settings.Length,
		.Droop = settings.Droop,
		.RootThreshold = settings.RootThreshold,
		.TipThreshold = settings.TipThreshold,
		.RootDarkening = settings.RootDarkening,
		.ShellColor = settings.ShellColor,
		.FadeStart = settings.FadeStart,
		.FadeEnd = settings.FadeEnd
	};
}

void FurShells::LoadOverride(const std::string& a_shellPath, ShellData& a_data)
{
	a_data.overridden = false;

	const std::string path = GetOverridePath(a_shellPath);
	RE::BSResourceNiBinaryStream stream(path);
	if (stream.good()) {
		const uint32_t size = stream.stream->totalSize;
		std::string text(std::min(size, MaxOverrideSize), '\0');
		if (size != 0 && size <= MaxOverrideSize && stream.read(text.data(), size)) {
			try {
				a_data.values = json::parse(text, nullptr, true, true).get<ShellOverride>();
				ClampOverride(a_data.values);
				a_data.overridden = true;
				logger::info("[Fur Shells] {} uses override {}", a_shellPath, path);
			} catch (const json::exception& e) {
				logger::warn("[Fur Shells] Failed to parse {}: {}", path, e.what());
			}
		} else {
			logger::warn("[Fur Shells] Failed to read {}", path);
		}
	}

	UpdateOverrideFadeEnd();
}

bool FurShells::SaveOverride(const std::string& a_shellPath, const ShellOverride& a_values, std::string& a_outputPath)
{
	a_outputPath = "Data\\" + GetOverridePath(a_shellPath);
	if (!a_shellPath.starts_with("textures\\") || !a_shellPath.ends_with("_shell.dds") || a_shellPath.find("..") != std::string::npos || a_shellPath.find(':') != std::string::npos) {
		logger::error("[Fur Shells] Refused to write {}", a_outputPath);
		return false;
	}

	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(a_outputPath).parent_path(), error);

	std::ofstream fileStream(a_outputPath);
	if (fileStream.is_open())
		fileStream << std::setw(4) << json(a_values);
	fileStream.close();
	if (fileStream.fail()) {
		logger::error("[Fur Shells] Failed to write {}", a_outputPath);
		return false;
	}

	logger::info("[Fur Shells] Wrote override {}", a_outputPath);
	return true;
}

void FurShells::UpdateOverrideFadeEnd()
{
	overrideFadeEnd = 0.0f;
	for (const auto& shellTexture : shellTextures)
		if (shellTexture.second.overridden)
			overrideFadeEnd = std::max(overrideFadeEnd, shellTexture.second.values.FadeEnd);
}

void FurShells::Reset()
{
	resolveBudget = ResolvesPerFrame;
	++frameIndex;
	lastMaxInertia = maxInertia;
	maxInertia = 0.0f;
	if (globals::state->IsMainOrLoadingMenuOpen())
		inertiaStates.clear();
	else
		std::erase_if(inertiaStates, [this](const auto& a_state) { return frameIndex - a_state.second.frame > InertiaExpireFrames; });
	lastPassCount = passCount;
	passCount = 0;
	lastStereoDraws = stereoDraws;
	lastMonoDraws = monoDraws;
	lastMaxShells = maxShells;
	lastMinDensity = minDensity;
	lastWindSpeed = windSpeed;
	windSpeed = 0.0f;
	auto& bodies = Util::BodyCapsules::GetTracker();
	lastTouchCapsules = static_cast<uint32_t>(bodies.GetCount());
	lastMaxTouchPerPass = maxTouchPerPass;
	lastTouchSelections = std::move(touchSelections);
	touchSelections.clear();
	lastTouchNearestGap = touchNearestGap;
	touchNearestGap = FLT_MAX;
	maxTouchPerPass = 0;
	if (!IsTouchActive()) {
		dentMemory.Clear();
	} else {
		if (lastPassCount > 0)
			bodies.Update(TouchActorRange);
		dentMemory.Advance();
	}
	maxShells = 0;
	minDensity = 1.0f;
	stereoDraws = 0;
	monoDraws = 0;
	lastDiffuse = nullptr;
	lastLookup = {};
	if (!entries.empty() && globals::state->IsMainOrLoadingMenuOpen())
		entries.clear();
	deferredPasses.clear();
	deferredOverlayPasses.clear();
	frameBodies.clear();
	deferralClosed = false;
}

void FurShells::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	using Flags = SIE::ShaderCache::LightingShaderFlags;
	using Techniques = SIE::ShaderCache::LightingShaderTechniques;

	if (a_shader->shaderType != RE::BSShader::Type::Lighting)
		return;

	constexpr uint32_t vertexColor = static_cast<uint32_t>(Flags::VC);
	constexpr std::array techniques{ Techniques::None, Techniques::Envmap };
	constexpr std::array alphaTestFlags{ 0u, static_cast<uint32_t>(Flags::DoAlphaTest) };
	constexpr std::array deferredFlags{ 0u, static_cast<uint32_t>(Flags::Deferred) };
	constexpr std::array vertexColorFlags{ 0u, vertexColor };
	constexpr std::array skinnedFlags{ 0u, static_cast<uint32_t>(Flags::Skinned) };

	auto* shaderCache = globals::shaderCache;
	for (const auto technique : techniques) {
		const uint32_t type = static_cast<uint32_t>(technique) << 24;
		for (const uint32_t alphaTest : alphaTestFlags)
			for (const uint32_t deferred : deferredFlags) {
				std::ignore = shaderCache->GetPixelShader(*a_shader, type | vertexColor | alphaTest | deferred | FurFlag);
				std::ignore = shaderCache->GetPixelShader(*a_shader, type | vertexColor | alphaTest | deferred | FurDepthFlag);
			}
		for (const uint32_t color : vertexColorFlags)
			for (const uint32_t skinned : skinnedFlags)
				std::ignore = shaderCache->GetVertexShader(*a_shader, type | color | skinned | FurFlag);
	}

	if (globals::features::truePBR.loaded) {
		for (const uint32_t alphaTest : alphaTestFlags)
			for (const uint32_t deferred : deferredFlags) {
				std::ignore = shaderCache->GetPixelShader(*a_shader, static_cast<uint32_t>(Flags::TruePbr) | vertexColor | alphaTest | deferred | FurFlag);
				std::ignore = shaderCache->GetPixelShader(*a_shader, static_cast<uint32_t>(Flags::TruePbr) | vertexColor | alphaTest | deferred | FurDepthFlag);
			}
	}
}

FurShells::ShellLookup FurShells::FindShell(RE::BSRenderPass* a_pass)
{
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
	if (!diffuse)
		return {};
	if (diffuse == lastDiffuse)
		return lastLookup;

	auto it = entries.find(diffuse);
	if (it == entries.end() || it->second.name != diffuse->name) {
		RE::NiSourceTexturePtr shell;
		ShellData* data = nullptr;
		std::string previousPath;
		const char* setPath = material->textureSet ? material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
		for (const char* diffusePath : { diffuse->name.c_str(), setPath }) {
			if (shell || !diffusePath || !*diffusePath)
				continue;

			std::string path = NormalizeTexturePath(diffusePath);
			if (path == previousPath || missingShells.contains(path))
				continue;
			if (resolveBudget == 0)
				return {};
			--resolveBudget;

			bool exists = false;
			std::string shellPath;
			shell = LoadShell(path, exists, shellPath);
			if (shell) {
				const auto [dataIt, inserted] = shellTextures.try_emplace(shellPath);
				if (inserted)
					LoadOverride(shellPath, dataIt->second);
				data = &dataIt->second;
			}
			if (!exists) {
				if (missingShells.size() >= MaxMissingShells)
					missingShells.clear();
				missingShells.emplace(path);
			}
			previousPath = std::move(path);
		}

		const bool emptyOverlay = NormalizeTexturePath(diffuse->name.c_str()) == EmptyOverlayPath || (setPath && NormalizeTexturePath(setPath) == EmptyOverlayPath);
		if (entries.size() >= MaxEntries)
			entries.clear();
		it = entries.insert_or_assign(diffuse, Entry{ diffuse->name, std::move(shell), data, emptyOverlay }).first;
	}

	lastDiffuse = diffuse;
	lastLookup = { it->second.shell.get(), it->second.data, true, it->second.emptyOverlay };
	return lastLookup;
}

const FurShells::FrameBody* FurShells::FindFrameBody(RE::BSGeometry* a_geometry) const
{
	if (frameBodies.empty())
		return nullptr;

	auto* skinInstance = a_geometry->GetGeometryRuntimeData().skinInstance.get();
	auto* partition = skinInstance ? skinInstance->skinPartition.get() : nullptr;
	if (!partition)
		return nullptr;

	for (const auto& body : frameBodies)
		if (body.partition == partition && body.rootParent == skinInstance->rootParent && body.geometry != a_geometry)
			return &body;

	return nullptr;
}

void FurShells::RecordFrameBody(RE::NiSkinInstance* a_skinInstance, RE::BSGeometry* a_geometry, const PerPass& a_perPass, ID3D11ShaderResourceView* a_shellView, ID3D11ShaderResourceView* a_normalView, bool a_deferred)
{
	auto* partition = a_skinInstance->skinPartition.get();
	if (!partition)
		return;

	auto it = std::ranges::find_if(frameBodies, [&](const FrameBody& a_body) { return a_body.geometry == a_geometry; });
	if (it == frameBodies.end())
		it = frameBodies.emplace(frameBodies.end());

	it->partition = partition;
	it->rootParent = a_skinInstance->rootParent;
	it->geometry = a_geometry;
	it->shellView.copy_from(a_shellView);
	it->normalView.copy_from(a_normalView);
	it->perPass = a_perPass;
	it->deferred = a_deferred;
}

void FurShells::BeginPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	using enum RE::BSGraphics::DepthStencilDepthMode;

	if (instanceCount != 0)
		EndPass();

	if (!settings.Enabled || !shellAssetsPresent.load(std::memory_order_relaxed) || !perPassCB || !noColorWrite || !a_pass || !globals::shaderCache->IsEnabled())
		return;

	auto* state = globals::state;
	if ((state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0 || !IsFurTechnique(state->currentPixelDescriptor))
		return;

	auto* geometry = a_pass->geometry;
	if (!geometry)
		return;

	if (const auto* body = FindFrameBody(geometry)) {
		BeginOverlayPass(a_shader, a_pass, *body);
		return;
	}

	auto* shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState)
	if (depthStencilDepthMode != kTestEqual && depthStencilDepthMode != kTestWrite)
		return;

	const float distance = IsFirstPersonPass() ? 0.0f : std::max(0.0f, geometry->worldBound.center.GetDistance(Util::GetEyePosition(0)) - geometry->worldBound.radius);
	if (distance >= std::max(settings.FadeEnd, overrideFadeEnd) || !CanDrawShells(geometry))
		return;

	auto* skinInstance = geometry->GetGeometryRuntimeData().skinInstance.get();
	const bool modelSpaceNormals = (state->modifiedVertexDescriptor & ModelSpaceNormalsFlag) != 0;
	if (modelSpaceNormals && !skinInstance)
		return;

	const auto lookup = FindShell(a_pass);
	auto* data = lookup.data;
	auto* shellView = lookup.shell && lookup.shell->rendererTexture ? Util::AsReal(lookup.shell->rendererTexture->resourceView) : nullptr;
	if (!shellView || !data)
		return;

	data->skin = IsSkinTechnique(state->currentPixelDescriptor);
	const ShellOverride values = data->overridden ? data->values : GetGlobalValues(data->skin);
	if (!values.Enabled)
		return;

	const float fade = std::clamp((values.FadeEnd - distance) / std::max(values.FadeEnd - values.FadeStart, 1.0f), 0.0f, 1.0f);
	const float pixels = GetFurPixels(values.Length, distance);
	const float sizeFade = settings.MinFurPixels > 0.0f ? std::clamp(pixels / settings.MinFurPixels, 0.0f, 1.0f) : 1.0f;
	const float density = fade * sizeFade;
	if (density <= 0.0f)
		return;

	ID3D11ShaderResourceView* normalView = nullptr;
	if (modelSpaceNormals) {
		auto* normal = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)->normalTexture.get();
		normalView = normal && normal->rendererTexture ? Util::AsReal(normal->rendererTexture->resourceView) : nullptr;
		if (!normalView)
			return;
	}

	const float layers = std::clamp(pixels / PixelsPerShell, 1.0f, static_cast<float>(std::min(values.ShellCount, MaxShells)));
	const auto shells = static_cast<uint32_t>(std::ceil(layers));
	const float tipWeight = layers - static_cast<float>(shells - 1);
	maxShells = std::max(maxShells, shells);
	minDensity = std::min(minDensity, density);

	auto* shaderCache = globals::shaderCache;
	auto* vertexShader = shaderCache->GetVertexShader(*a_shader, state->modifiedVertexDescriptor | FurFlag);
	auto* pixelShader = shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | FurFlag);
	auto* depthShader = shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | FurDepthFlag);
	if (!vertexShader || !pixelShader || !depthShader)
		return;

	const bool defer = !replaying && !deferralClosed && settings.HideCoveredFur && state->inWorld && currentPass.pass == a_pass;

	const float3 wind = GetWind(geometry->worldBound.center);
	windSpeed = std::max(windSpeed, wind.Length());
	PerPass perPass{ values.Length, static_cast<float>(shells), values.Droop, values.RootThreshold, values.TipThreshold, values.RootDarkening, values.ShellColor, 0.0f, density, tipWeight };
	perPass.Wind = float4(wind.x, wind.y, wind.z, 0.0f);
	perPass.WetStrength = settings.WetStrength;
	const float3 inertia = GetInertia(geometry, values.Length, values.Droop);
	perPass.Inertia = inertia;
	FillTouch(perPass, geometry);
	if (skinInstance && settings.OverlayFur)
		RecordFrameBody(skinInstance, geometry, perPass, shellView, normalView, defer || replaying);

	if (defer) {
		deferredPasses.push_back(currentPass);
		return;
	}

	perPass.RootTest = replaying && rootTest ? 1.0f : 0.0f;
	furVertexShader = reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader);
	furPixelShader = reinterpret_cast<ID3D11PixelShader*>(pixelShader->shader);
	depthPixelShader = reinterpret_cast<ID3D11PixelShader*>(depthShader->shader);
	BindPass(perPass, shellView, normalView);
}

void FurShells::FillTouch(PerPass& a_perPass, const RE::BSGeometry* a_geometry)
{
	ZoneScopedN("FurShells::FillTouch");
	std::array<FurInteractors::Capsule, FurInteractors::MaxCapsulesPerPass> touches;
	size_t count = 0;
	if (IsTouchActive()) {
		std::array<FurInteractors::Capsule, Util::BodyCapsules::MaxSelectedCapsules> candidates;
		const size_t found = Util::BodyCapsules::GetTracker().Select(a_geometry->worldBound.center, a_geometry->worldBound.radius, TouchReach, GetWearer(a_geometry), candidates, touchNearestGap);
		count = dentMemory.Nearest(a_geometry, std::span(candidates.data(), found), std::span(touches.data(), FurInteractors::MaxLiveCapsulesPerPass));
	}
	if (IsTouchActive())
		count = dentMemory.Remember(a_geometry, a_perPass.Length, count, touches);
	if (touchSelections.size() < 8)
		touchSelections.push_back({ a_geometry->name.c_str() ? a_geometry->name.c_str() : "", { touches.begin(), touches.begin() + static_cast<std::ptrdiff_t>(count) } });
	maxTouchPerPass = std::max(maxTouchPerPass, static_cast<uint32_t>(count));
	a_perPass.TouchCount = static_cast<float>(count);
	a_perPass.TouchStrength = settings.TouchStrength;
	a_perPass.TouchDebug = settings.ShowTouchEdges ? 1.0f : 0.0f;
	for (int eye = 0; eye < 2; ++eye) {
		const auto position = Util::GetEyePosition(eye);
		a_perPass.TouchEye[eye][0] = position.x;
		a_perPass.TouchEye[eye][1] = position.y;
		a_perPass.TouchEye[eye][2] = position.z;
	}
	for (size_t index = 0; index < count; ++index) {
		const auto& touch = touches[index];
		a_perPass.TouchA[index][0] = touch.pointA.x;
		a_perPass.TouchA[index][1] = touch.pointA.y;
		a_perPass.TouchA[index][2] = touch.pointA.z;
		a_perPass.TouchA[index][3] = touch.radius;
		a_perPass.TouchB[index][0] = touch.pointB.x;
		a_perPass.TouchB[index][1] = touch.pointB.y;
		a_perPass.TouchB[index][2] = touch.pointB.z;
		a_perPass.TouchB[index][3] = touch.remembered ? 1.0f : 0.0f;
		a_perPass.TouchMove[index][0] = touch.move.x;
		a_perPass.TouchMove[index][1] = touch.move.y;
		a_perPass.TouchMove[index][2] = touch.move.z;
		a_perPass.TouchMove[index][3] = touch.strength;
	}
}

FurInertia::Tuning FurShells::GetInertiaTuning() const
{
	const FurInertia::Tuning custom{ settings.InertiaFrequency, settings.InertiaGain, settings.InertiaLengthExponent, settings.InertiaDroopExponent, settings.InertiaMaxBend };
	return FurInertia::GetTuning(static_cast<FurInertia::Preset>(std::min(settings.InertiaPreset, static_cast<uint32_t>(FurInertia::Preset::Custom))), custom);
}

void FurShells::DrawInertiaSettings()
{
	using FurInertia::Preset;

	ImGui::SliderFloat(T(TKEY("inertia_strength"), "Inertia Strength"), &settings.InertiaStrength, 0.0f, FurInertia::MaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_strength_tooltip"), "How far the fur tips lag behind a moving mesh before springing back, so fur trails when the wearer starts, stops or turns. 0 disables inertia."));

	const std::array<std::string, 3> names{ T(TKEY("inertia_preset_stylized"), "Stylized"), T(TKEY("inertia_preset_realistic"), "Realistic"), T(TKEY("inertia_preset_custom"), "Custom") };
	const auto selected = std::min(settings.InertiaPreset, static_cast<uint32_t>(Preset::Custom));
	if (ImGui::BeginCombo(T(TKEY("inertia_preset"), "Inertia Preset"), names[selected].c_str())) {
		for (uint32_t i = 0; i < names.size(); ++i) {
			if (ImGui::Selectable(names[i].c_str(), i == selected) && i != selected) {
				if (i == static_cast<uint32_t>(Preset::Custom)) {
					const auto tuning = GetInertiaTuning();
					settings.InertiaFrequency = tuning.frequencyHz;
					settings.InertiaGain = tuning.gain;
					settings.InertiaLengthExponent = tuning.lengthExponent;
					settings.InertiaDroopExponent = tuning.droopExponent;
					settings.InertiaMaxBend = tuning.maxBend;
				}
				settings.InertiaPreset = i;
			}
		}
		ImGui::EndCombo();
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_preset_tooltip"), "Stylized is the tuned-by-eye look. Realistic uses the physics of a real hair, so short fur barely moves and long fur swings. Custom exposes the spring."));

	if (settings.InertiaPreset != static_cast<uint32_t>(Preset::Custom))
		return;

	ImGui::SliderFloat(T(TKEY("inertia_frequency"), "Spring Frequency (Hz)"), &settings.InertiaFrequency, FurInertia::MinFrequencyHz, FurInertia::MaxTuningFrequencyHz, "%.1f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_frequency_tooltip"), "How fast fur of the reference length and droop swings and settles. Higher is stiffer."));

	if (!ImGui::TreeNode(T(TKEY("inertia_advanced"), "Advanced Inertia")))
		return;

	ImGui::SliderFloat(T(TKEY("inertia_gain"), "Lag Gain"), &settings.InertiaGain, FurInertia::MinGain, FurInertia::MaxGain, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_gain_tooltip"), "Scales how far the tips lag for a given acceleration, before Inertia Strength. Stylized uses 2, Realistic uses 1."));

	ImGui::SliderFloat(T(TKEY("inertia_max_bend"), "Maximum Bend"), &settings.InertiaMaxBend, FurInertia::MinBendLimit, FurInertia::MaxBendLimit, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_max_bend_tooltip"), "The furthest the tips may lag, as a fraction of the fur length."));

	ImGui::SliderFloat(T(TKEY("inertia_length_exponent"), "Length Softening"), &settings.InertiaLengthExponent, 0.0f, FurInertia::MaxLengthExponent, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_length_exponent_tooltip"), "How much longer fur swings slower and lags more. 0 ignores length; 2 follows a real hair."));

	ImGui::SliderFloat(T(TKEY("inertia_droop_exponent"), "Droop Softening"), &settings.InertiaDroopExponent, 0.0f, FurInertia::MaxDroopExponent, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("inertia_droop_exponent_tooltip"), "How much a droopier fur asset swings slower and lags more. 0 ignores droop."));
	ImGui::TreePop();
}

float3 FurShells::GetInertia(const RE::BSGeometry* a_geometry, float a_length, float a_droop)
{
	if (settings.InertiaStrength <= 0.0f)
		return {};

	const auto& center = a_geometry->worldBound.center;
	const float3 position{ center.x, center.y, center.z };
	auto [it, inserted] = inertiaStates.try_emplace(a_geometry, InertiaState{ FurInertia::Spring::At(position), frameIndex });
	auto& state = it->second;
	if (state.frame != frameIndex) {
		state.frame = frameIndex;
		const auto* skinInstance = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		const float scale = skinInstance && skinInstance->rootParent ? skinInstance->rootParent->world.scale : a_geometry->world.scale;
		const float worldLength = FurInertia::GetWorldLength(a_length, scale);
		const auto tuning = GetInertiaTuning();
		state.spring.Step(position, globals::game::deltaTime ? *globals::game::deltaTime : 0.0f, tuning.maxBend * worldLength, FurInertia::GetResponse(tuning, worldLength, a_droop));
	}
	const float3 inertia = state.spring.offset * settings.InertiaStrength;
	maxInertia = std::max(maxInertia, inertia.Length());
	return inertia;
}

float3 FurShells::GetWind(const RE::NiPoint3& a_position) const
{
	auto& windFeature = globals::features::wind;
	if (!windFeature.loaded || settings.WindStrength <= 0.0f || Util::IsInterior())
		return {};
	const float3 position{ a_position.x, a_position.y, a_position.z };
	Wind::PublishedWindSample sample;
	if (!windFeature.SamplePublishedWind({ &position, 1 }, { &sample, 1 }))
		return {};
	float3 velocity = sample.windExcludingHavokImpulses * settings.WindStrength;
	velocity.z = 0.0f;
	const float speed = velocity.Length();
	return speed > MaxWindSpeed ? velocity * (MaxWindSpeed / speed) : velocity;
}

void FurShells::BeginOverlayPass(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, const FrameBody& a_body)
{
	const auto lookup = FindShell(a_pass);
	if (!lookup.resolved || lookup.emptyOverlay || static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)->materialAlpha <= 0.0f)
		return;

	auto* state = globals::state;
	const bool modelSpaceNormals = (state->modifiedVertexDescriptor & ModelSpaceNormalsFlag) != 0;
	if (modelSpaceNormals != static_cast<bool>(a_body.normalView))
		return;

	auto* shaderCache = globals::shaderCache;
	auto* vertexShader = shaderCache->GetVertexShader(*a_shader, state->modifiedVertexDescriptor | FurFlag);
	auto* pixelShader = shaderCache->GetPixelShader(*a_shader, state->modifiedPixelDescriptor | FurFlag);
	if (!vertexShader || !pixelShader)
		return;

	if (!replaying && !deferralClosed && a_body.deferred) {
		if (currentPass.pass == a_pass)
			deferredOverlayPasses.push_back(currentPass);
		return;
	}

	PerPass perPass = a_body.perPass;
	perPass.ShellColor = 0.0f;
	perPass.RootTest = a_body.deferred && rootTest ? 1.0f : 0.0f;
	if (perPass.RootTest > 0.0f) {
		ID3D11ShaderResourceView* view = depthCopyView.get();
		globals::d3d::context->PSSetShaderResources(DepthTextureSlot, 1, &view);
	}

	furVertexShader = reinterpret_cast<ID3D11VertexShader*>(vertexShader->shader);
	furPixelShader = reinterpret_cast<ID3D11PixelShader*>(pixelShader->shader);
	overlayPass = true;
	BindPass(perPass, a_body.shellView.get(), a_body.normalView.get());
}

void FurShells::BindPass(const PerPass& a_perPass, ID3D11ShaderResourceView* a_shellView, ID3D11ShaderResourceView* a_normalView)
{
	if (std::memcmp(&a_perPass, &lastPerPass, sizeof(PerPass)) != 0) {
		perPassCB->Update(a_perPass);
		lastPerPass = a_perPass;
	}

	auto* context = globals::d3d::context;
	context->VSGetShader(savedVertexShader.put(), nullptr, nullptr);
	context->PSGetShader(savedPixelShader.put(), nullptr, nullptr);
	if (!overlayPass) {
		context->VSSetShader(furVertexShader, nullptr, 0);
		context->PSSetShader(furPixelShader, nullptr, 0);
	}

	ID3D11Buffer* buffer = perPassCB->CB();
	context->VSSetConstantBuffers(VertexPerPassSlot, 1, &buffer);
	context->PSSetConstantBuffers(PixelPerPassSlot, 1, &buffer);
	context->PSSetShaderResources(ShellTextureSlot, 1, &a_shellView);
	if (a_normalView)
		context->VSSetShaderResources(NormalTextureSlot, 1, &a_normalView);

	const auto shellInstances = static_cast<uint32_t>(a_perPass.ShellCount);
	instanceCount = overlayPass || replaying ? shellInstances : shellInstances + 1;
	++passCount;
}

void FurShells::EndPass()
{
	if (instanceCount == 0)
		return;
	instanceCount = 0;
	overlayPass = false;

	auto* context = globals::d3d::context;
	context->VSSetShader(savedVertexShader.get(), nullptr, 0);
	context->PSSetShader(savedPixelShader.get(), nullptr, 0);
	savedVertexShader = nullptr;
	savedPixelShader = nullptr;
}

const FurShells::DepthStates* FurShells::GetDepthStates(ID3D11DepthStencilState* a_source)
{
	if (!a_source)
		return nullptr;

	auto it = depthStates.find(a_source);
	if (it == depthStates.end()) {
		DepthStates states;
		states.source.copy_from(a_source);

		D3D11_DEPTH_STENCIL_DESC desc;
		a_source->GetDesc(&desc);

		D3D11_DEPTH_STENCIL_DESC overlayDesc = desc;
		overlayDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		overlayDesc.StencilWriteMask = 0;

		desc.DepthEnable = TRUE;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;

		auto* device = globals::d3d::device;
		if (SUCCEEDED(device->CreateDepthStencilState(&desc, states.prepass.put()))) {
			desc.DepthFunc = D3D11_COMPARISON_EQUAL;
			desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			if (SUCCEEDED(device->CreateDepthStencilState(&desc, states.shade.put())) && SUCCEEDED(device->CreateDepthStencilState(&overlayDesc, states.overlay.put()))) {
				Util::SetResourceName(states.prepass.get(), "FurShells::PrepassDepthState");
				Util::SetResourceName(states.shade.get(), "FurShells::ShadeDepthState");
				Util::SetResourceName(states.overlay.get(), "FurShells::OverlayDepthState");
			} else {
				states.prepass = nullptr;
			}
		}
		if (!states.prepass)
			logger::warn("[Fur Shells] Failed to create depth states");

		it = depthStates.emplace(a_source, std::move(states)).first;
	}

	return it->second.prepass ? &it->second : nullptr;
}

void FurShells::DrawInstanced(UINT a_indexCount, UINT a_instanceCount, UINT a_startIndexLocation, INT a_baseVertexLocation)
{
	issuingDraw = true;
	globals::d3d::context->DrawIndexedInstanced(a_indexCount, a_instanceCount, a_startIndexLocation, a_baseVertexLocation, 0);
	issuingDraw = false;
}

bool FurShells::DrawShells(UINT a_indexCount, UINT a_engineInstances, UINT a_startIndexLocation, INT a_baseVertexLocation)
{
	if (instanceCount == 0)
		return replaying;

	CS_GPU_PASS("FurShells::Draw");

	const UINT totalInstances = instanceCount * a_engineInstances;
	++(a_engineInstances > 1 ? stereoDraws : monoDraws);
	auto* context = globals::d3d::context;

	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	UINT stencilRef = 0;
	context->OMGetDepthStencilState(depthState.put(), &stencilRef);

	const auto* states = GetDepthStates(depthState.get());
	if (!states) {
		EndPass();
		return replaying;
	}

	if (overlayPass) {
		context->OMSetDepthStencilState(states->overlay.get(), stencilRef);
		context->VSSetShader(furVertexShader, nullptr, 0);
		context->PSSetShader(furPixelShader, nullptr, 0);
		DrawInstanced(a_indexCount, totalInstances, a_startIndexLocation, a_baseVertexLocation);
		context->VSSetShader(savedVertexShader.get(), nullptr, 0);
		context->PSSetShader(savedPixelShader.get(), nullptr, 0);
		context->OMSetDepthStencilState(depthState.get(), stencilRef);
		return replaying;
	}

	winrt::com_ptr<ID3D11BlendState> blendState;
	FLOAT blendFactor[4]{};
	UINT sampleMask = 0;
	context->OMGetBlendState(blendState.put(), blendFactor, &sampleMask);

	context->OMSetBlendState(noColorWrite.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(states->prepass.get(), stencilRef);
	context->PSSetShader(depthPixelShader, nullptr, 0);
	DrawInstanced(a_indexCount, totalInstances, a_startIndexLocation, a_baseVertexLocation);

	context->OMSetBlendState(blendState.get(), blendFactor, sampleMask);
	context->OMSetDepthStencilState(states->shade.get(), stencilRef);
	context->PSSetShader(furPixelShader, nullptr, 0);
	DrawInstanced(a_indexCount, totalInstances, a_startIndexLocation, a_baseVertexLocation);

	context->OMSetDepthStencilState(depthState.get(), stencilRef);
	return true;
}

struct FurShells::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			func(a_shader, a_pass, a_renderFlags);
			globals::features::furShells.BeginPass(a_shader, a_pass);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSLightingShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			globals::features::furShells.EndPass();
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawIndexed
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndexLocation, INT a_baseVertexLocation)
		{
			auto& furShells = globals::features::furShells;
			if (!furShells.IsDrawing() || a_context != globals::d3d::context || !furShells.DrawShells(a_indexCount, 1, a_startIndexLocation, a_baseVertexLocation))
				func(a_context, a_indexCount, a_startIndexLocation, a_baseVertexLocation);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawIndexedInstanced
	{
		static void thunk(ID3D11DeviceContext* a_context, UINT a_indexCountPerInstance, UINT a_instanceCount, UINT a_startIndexLocation, INT a_baseVertexLocation, UINT a_startInstanceLocation)
		{
			auto& furShells = globals::features::furShells;
			if (!furShells.IsDrawing() || a_context != globals::d3d::context || a_startInstanceLocation != 0 || !furShells.DrawShells(a_indexCountPerInstance, a_instanceCount, a_startIndexLocation, a_baseVertexLocation))
				func(a_context, a_indexCountPerInstance, a_instanceCount, a_startIndexLocation, a_baseVertexLocation, a_startInstanceLocation);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
		{
			auto& furShells = globals::features::furShells;
			furShells.currentPass = { a_pass, a_technique, a_alphaTest, a_renderFlags };
			func(a_pass, a_technique, a_alphaTest, a_renderFlags);
			furShells.currentPass.pass = nullptr;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

bool FurShells::CopySceneDepth()
{
	auto* texture = Util::AsReal(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
	if (!texture || depthCopyFailed)
		return false;

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);

	D3D11_TEXTURE2D_DESC copyDesc{};
	if (depthCopy)
		depthCopy->GetDesc(&copyDesc);

	if (!depthCopyView || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format) {
		depthCopy = nullptr;
		depthCopyView = nullptr;

		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.Format = GetDepthViewFormat(desc.Format);
		viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MipLevels = 1;

		auto* device = globals::d3d::device;
		if (viewDesc.Format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 || FAILED(device->CreateTexture2D(&desc, nullptr, depthCopy.put())) || FAILED(device->CreateShaderResourceView(depthCopy.get(), &viewDesc, depthCopyView.put()))) {
			depthCopy = nullptr;
			depthCopyView = nullptr;
			depthCopyFailed = true;
			logger::warn("[Fur Shells] Failed to create depth copy, fur under clothing stays visible");
			return false;
		}
		Util::SetResourceName(depthCopy.get(), "FurShells::DepthCopy");
		Util::SetResourceName(depthCopyView.get(), "FurShells::DepthCopy SRV");
	}

	globals::d3d::context->CopyResource(depthCopy.get(), texture);
	return true;
}

void FurShells::OnBeforeBlendedDecals()
{
	auto* state = globals::state;
	if (!state->inWorld || (state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0)
		return;

	deferralClosed = true;
	if (deferredPasses.empty())
		return;

	CS_GPU_PASS("FurShells::DeferredShells");

	{
		CS_GPU_PASS("FurShells::DepthCopy");
		rootTest = CopySceneDepth();
	}
	if (rootTest) {
		ID3D11ShaderResourceView* view = depthCopyView.get();
		globals::d3d::context->PSSetShaderResources(DepthTextureSlot, 1, &view);
	}

	auto* shadowState = globals::game::shadowState;
	auto* stateUpdateFlags = globals::game::stateUpdateFlags;
	GET_INSTANCE_MEMBER(alphaBlendMode, shadowState)
	GET_INSTANCE_MEMBER(alphaBlendWriteMode, shadowState)
	GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState)
	const auto savedAlphaBlendMode = alphaBlendMode;
	const auto savedAlphaBlendWriteMode = alphaBlendWriteMode;
	const auto savedDepthMode = depthStencilDepthMode;

	alphaBlendMode = 0;
	alphaBlendWriteMode = 1;
	depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestEqual;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

	{
		CS_GPU_PASS("FurShells::ReplayPasses");
		replaying = true;
		for (const auto& deferred : deferredPasses)
			Hooks::BSBatchRenderer_RenderPassImmediately::func(deferred.pass, deferred.technique, deferred.alphaTest, deferred.renderFlags);
		for (const auto& deferred : deferredOverlayPasses)
			Hooks::BSBatchRenderer_RenderPassImmediately::func(deferred.pass, deferred.technique, deferred.alphaTest, deferred.renderFlags);
		replaying = false;
	}
	deferredPasses.clear();
	deferredOverlayPasses.clear();

	alphaBlendMode = savedAlphaBlendMode;
	alphaBlendWriteMode = savedAlphaBlendWriteMode;
	depthStencilDepthMode = savedDepthMode;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
}

json FurShells::GetDiagnostics()
{
	size_t overridden = 0;
	json textures = json::array();
	for (const auto& [path, data] : shellTextures) {
		overridden += data.overridden ? 1 : 0;
		textures.push_back({ { "path", path }, { "overridden", data.overridden }, { "skin", data.skin } });
	}

	D3D11_TEXTURE2D_DESC depthDesc{};
	if (depthCopy)
		depthCopy->GetDesc(&depthDesc);

	return {
		{ "enabled", settings.Enabled },
		{ "depthCopyWidth", depthDesc.Width },
		{ "depthCopyHeight", depthDesc.Height },
		{ "depthCopyFormat", static_cast<uint32_t>(depthDesc.Format) },
		{ "vr", globals::game::isVR },
		{ "passesLastFrame", lastPassCount },
		{ "stereoDrawsLastFrame", lastStereoDraws },
		{ "monoDrawsLastFrame", lastMonoDraws },
		{ "maxShellsLastFrame", lastMaxShells },
		{ "inertiaMaxOffsetLastFrame", lastMaxInertia },
		{ "inertiaTracked", inertiaStates.size() },
		{ "minDensityLastFrame", lastMinDensity },
		{ "windSpeedLastFrame", lastWindSpeed },
		{ "touchCapsulesLastFrame", lastTouchCapsules },
		{ "touchMaxPerPassLastFrame", lastMaxTouchPerPass },
		{ "touchDents", dentMemory.GetDentCount() },
		{ "touchHandNodesFound", Util::BodyCapsules::GetTracker().GetHandNodeCount() },
		{ "touchFitMeshes", Util::BodyCapsules::GetTracker().GetFitMeshCount() },
		{ "touchHandPositions", HandPositionsJson(Util::BodyCapsules::GetTracker()) },
		{ "touchHandFingers", HandFingersJson(Util::BodyCapsules::GetTracker()) },
		{ "touchSelections", SelectionsJson(lastTouchSelections) },
		{ "touchHeld", HeldJson(Util::BodyCapsules::GetTracker()) },
		{ "touchFingerRadius", Util::BodyCapsules::GetTracker().GetAverageFingerRadius() },
		{ "touchPalmRadius", Util::BodyCapsules::GetTracker().GetAveragePalmRadius() },
		{ "playerSkinData", globals::game::player ? ProbeSkinData(globals::game::player) : json::object() },
		{ "touchNearestGapLastFrame", lastTouchNearestGap == FLT_MAX ? -1.0f : lastTouchNearestGap },
		{ "deferredPasses", deferredPasses.size() },
		{ "deferredOverlayPasses", deferredOverlayPasses.size() },
		{ "frameBodies", frameBodies.size() },
		{ "rootTest", rootTest },
		{ "depthCopyFailed", depthCopyFailed },
		{ "resolvedShellTextures", shellTextures.size() },
		{ "overriddenShellTextures", overridden },
		{ "shellTextures", std::move(textures) },
	};
}

void FurShells::SetupResources()
{
	static bool drawHookInstalled = false;

	perPassCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerPass>(), "FurShells::PerPass");

	noColorWrite = nullptr;
	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = 0;
	if (SUCCEEDED(globals::d3d::device->CreateBlendState(&blendDesc, noColorWrite.put())))
		Util::SetResourceName(noColorWrite.get(), "FurShells::NoColorWrite");
	else
		logger::warn("[Fur Shells] Failed to create blend state");

	if (!drawHookInstalled && globals::d3d::context) {
		drawHookInstalled = true;
		stl::detour_vfunc<12, Hooks::ID3D11DeviceContext_DrawIndexed>(globals::d3d::context);
		if (globals::game::isVR)
			stl::detour_vfunc<20, Hooks::ID3D11DeviceContext_DrawIndexedInstanced>(globals::d3d::context);
		logger::info("[Fur Shells] Installed draw hook");
	}
}

void FurShells::PostPostLoad()
{
	stl::write_vfunc<0x6, Hooks::BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, Hooks::BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_thunk_call<Hooks::BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	logger::info("[Fur Shells] Installed hooks");

	std::thread([this] {
		const auto start = std::chrono::steady_clock::now();
		const bool present = FurShellAssets::AnyShellTextures(Util::PathHelpers::GetDataPath());
		shellAssetsPresent.store(present, std::memory_order_relaxed);
		logger::info("[Fur Shells] Shell texture scan found {} in {} ms", present ? "shells (or an unreadable archive)" : "none, passes skipped", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	}).detach();
}

#undef I18N_KEY_PREFIX

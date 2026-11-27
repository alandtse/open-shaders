#include "ShadowMeshExclusions.h"

#include "CSEditor/LightPicker.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "ShadowMeshPaths.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"
#include <imgui_stdlib.h>

#define I18N_KEY_PREFIX "feature.light_limit_fix.shadow_meshes."

namespace ShadowMeshExclusions
{
	namespace
	{
		const std::filesystem::path directory{ R"(Data\SKSE\Plugins\CommunityShaders\LightLimitFix\ShadowMeshExclusions)" };
		constexpr std::string_view context = "point-light shadow mesh exclusions";

		struct Catalog
		{
			std::map<std::string, MeshPaths> files;
			std::vector<std::string> invalidFiles;
			MeshPaths unresolved;
			std::vector<RE::NiPointer<RE::NiNode>> models;
			std::unordered_set<const RE::BSGraphics::TriShape*> shapes;
			std::unordered_set<const RE::NiSkinData*> skins;
		};

		std::atomic<std::shared_ptr<const Catalog>> catalog{ std::make_shared<Catalog>() };
		std::atomic<bool> active{ false };
		std::atomic<bool> refresh{ false };
		std::atomic<uint64_t> skippedDraws{ 0 };

		void LoadModel(Catalog& next, const std::string& path)
		{
			RE::NiPointer<RE::NiNode> root;
			auto model = path;
			std::ranges::replace(model, '/', '\\');
			if (RE::BSModelDB::Demand(model.c_str(), root, {}) != RE::BSResource::ErrorCode::kNone || !root) {
				next.unresolved.insert(path);
				return;
			}
			bool drawable = false;
			RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* geometry) {
				const auto& data = geometry->GetGeometryRuntimeData();
				if (data.rendererData) {
					next.shapes.insert(data.rendererData);
					drawable = true;
				}
				if (data.skinInstance && data.skinInstance->skinData) {
					next.skins.insert(data.skinInstance->skinData.get());
					drawable = true;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (drawable)
				next.models.push_back(std::move(root));  // Keep the shared shape and skin identities alive across cell unloads.
			else
				next.unresolved.insert(path);
		}

		std::filesystem::path TargetFile(std::string name)
		{
			if (name.empty() || name != Util::FileHelpers::SanitizeFileName(name))
				return {};
			const auto firstDot = name.find('.');
			const auto stem = name.substr(0, firstDot);
			if (stem.empty() || Util::FileHelpers::SanitizeFileName(stem) != stem)
				return {};
			auto file = Util::PathHelpers::FromUtf8(name);
			if (!Util::IEquals(Util::PathHelpers::ToUtf8(file.extension()), ".json"))
				file += L".json";
			return directory / file;
		}

		bool EditFile(const std::filesystem::path& file, const std::string& mesh, bool add)
		{
			std::error_code error;
			const bool exists = std::filesystem::exists(file, error);
			if (error || (!exists && !add))
				return false;
			auto document = exists ? Util::FileHelpers::ReadJsonFile(file, context) :
			                         std::optional<json>(json{ { "meshes", json::array() } });
			if (!document || !UpdateMeshList(*document, mesh, add))
				return false;
			const auto target = exists ? Util::FileHelpers::ResolveExistingFile(file) : file;
			if (target.empty() || !Util::FileHelpers::WriteJsonAtomically(target, *document, 2, context))
				return false;
			Reload();
			return true;
		}
	}

	void Reload()
	{
		auto next = std::make_shared<Catalog>();
		MeshPaths combined;
		try {
			if (std::filesystem::exists(directory)) {
				for (const auto& entry : std::filesystem::directory_iterator(directory)) {
					const auto& file = entry.path();
					if (entry.is_directory() || !Util::IEquals(Util::PathHelpers::ToUtf8(file.extension()), ".json"))
						continue;
					auto document = Util::FileHelpers::ReadJsonFile(file, context);
					auto meshes = document ? ReadMeshes(*document) : std::nullopt;
					auto name = Util::PathHelpers::ToUtf8(file.filename());
					if (!meshes) {
						logger::warn("[LLF] Invalid shadow mesh exclusion file '{}'", name);
						next->invalidFiles.push_back(std::move(name));
						continue;
					}
					combined.insert(meshes->begin(), meshes->end());
					next->files.emplace(std::move(name), std::move(*meshes));
				}
			}
		} catch (const std::filesystem::filesystem_error& e) {
			logger::warn("[LLF] Could not scan shadow mesh exclusions: {}", e.what());
			return;
		}
		for (const auto& path : combined)
			LoadModel(*next, path);
		const auto previous = catalog.exchange(next);
		active.store(!next->shapes.empty() || !next->skins.empty(), std::memory_order_release);
		if (previous->shapes != next->shapes || previous->skins != next->skins)
			refresh.store(true, std::memory_order_release);
	}

	bool ShouldSkip(const RE::BSRenderPass* pass)
	{
		if (!active.load(std::memory_order_acquire) || !pass || !pass->geometry)
			return false;
		const auto* accumulator = *globals::game::currentAccumulator.get();
		using enum RE::BSShaderAccumulator::RENDER_MODE;
		if (!accumulator || (accumulator->GetRuntimeData().renderMode != kShadowMapPb && accumulator->GetRuntimeData().renderMode != kShadowMapCube))
			return false;
		const auto current = catalog.load();
		const auto& data = pass->geometry->GetGeometryRuntimeData();
		if (!current->shapes.contains(data.rendererData) && !(data.skinInstance && current->skins.contains(data.skinInstance->skinData.get())))
			return false;
		skippedDraws.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool ConsumeShadowRefresh()
	{
		return refresh.exchange(false, std::memory_order_acq_rel);
	}

	json Inspect()
	{
		const auto current = catalog.load();
		return { { "files", current->files }, { "invalidFiles", current->invalidFiles }, { "unresolvedMeshes", current->unresolved }, { "skippedDraws", skippedDraws.load() } };
	}

	void DrawSettings()
	{
		static LightPicker picker;
		static std::string mesh, filename = "MyExclusions.json";
		static bool writeFailed = false;
		ImGui::TextWrapped("%s", T(TKEY("description"), "Meshes in these lists stop casting point-light shadows. Each JSON file contains a meshes array of NIF paths relative to the meshes folder."));
		ImGui::TextWrapped("%s", Util::PathHelpers::ToUtf8(directory).c_str());
		if (Util::Button(T(TKEY("reload"), "Reload lists")))
			Reload();

		ImGui::InputText(T(TKEY("filename"), "JSON filename"), &filename);
		ImGui::InputText(T(TKEY("mesh_path"), "NIF path"), &mesh);
		if (!globals::game::isVR) {
			if (Util::Button(T("feature.light_editor.select_mesh", "Select Mesh")))
				picker.BeginPick();
			if (picker.IsPicking()) {
				int mode = static_cast<int>(picker.pickMode);
				ImGui::RadioButton(T("feature.light_editor.pick_mode_collision", "Collision"), &mode, 0);
				ImGui::SameLine();
				ImGui::RadioButton(T("feature.light_editor.pick_mode_effect", "Effect mesh"), &mode, 1);
				if (mode != static_cast<int>(picker.pickMode)) {
					picker.pickMode = static_cast<LightPicker::PickMode>(mode);
					picker.InvalidateHover();
				}
				ImGui::TextWrapped("%s", T(TKEY("pick_hint"), "Click a mesh in the world. Right-click or press Escape to cancel."));
				picker.Update();
				if (auto hit = picker.TakeResult(); hit.valid)
					mesh = hit.modelPath;
			}
		}
		const auto target = TargetFile(filename);
		const auto normalized = MeshPath(mesh);
		{
			const auto disabled = Util::DisableGuard(target.empty() || normalized.empty());
			if (Util::Button(T(TKEY("add"), "Add mesh to JSON")))
				writeFailed = !EditFile(target, normalized, true);
		}
		if (writeFailed)
			ImGui::TextWrapped("%s", T(TKEY("write_failed"), "Could not update the file. Check its JSON format and write permissions."));
		static Util::ConfirmationPopup removeFilePopup;
		static std::string pendingRemoval;
		const auto drawRemoveFile = [&](const std::string& name) {
			ImGui::SameLine();
			if (ImGui::SmallButton(T(TKEY("remove"), "Remove"))) {
				pendingRemoval = name;
				removeFilePopup.title = T(TKEY("remove_file"), "Remove JSON file");
				removeFilePopup.message = std::vformat(T(TKEY("confirm_remove_file"), "Are you sure you want to delete {} and all its mesh exclusions?"), std::make_format_args(name));
				removeFilePopup.confirmLabel = T(TKEY("remove"), "Remove");
				removeFilePopup.focus = Util::ConfirmationPopup::Focus::Cancel;
				removeFilePopup.Request();
			}
		};
		const auto current = catalog.load();
		for (const auto& name : current->invalidFiles) {
			ImGui::PushID(name.c_str());
			ImGui::TextWrapped(T(TKEY("invalid"), "Invalid mesh list: %s"), name.c_str());
			drawRemoveFile(name);
			ImGui::PopID();
		}
		for (const auto& path : current->unresolved)
			ImGui::TextWrapped(T(TKEY("unresolved"), "Could not resolve mesh: %s"), path.c_str());
		for (const auto& [name, paths] : current->files) {
			ImGui::PushID(name.c_str());
			const bool expanded = ImGui::TreeNodeEx("##file", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanLabelWidth, "%s (%zu)", name.c_str(), paths.size());
			drawRemoveFile(name);
			if (expanded) {
				ImGui::TreePush("##file");
				for (const auto& path : paths) {
					ImGui::PushID(path.c_str());
					if (ImGui::SmallButton(T(TKEY("remove"), "Remove")))
						writeFailed = !EditFile(directory / Util::PathHelpers::FromUtf8(name), path, false);
					ImGui::SameLine();
					ImGui::TextWrapped("%s", path.c_str());
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (removeFilePopup.Draw()) {
			const auto file = directory / Util::PathHelpers::FromUtf8(pendingRemoval);
			writeFailed = !::DeleteFileW(file.c_str());
			if (writeFailed)
				logger::warn("[LLF] Could not remove shadow mesh list '{}' (Win32 error {})", pendingRemoval, ::GetLastError());
			Reload();
		}
	}
}

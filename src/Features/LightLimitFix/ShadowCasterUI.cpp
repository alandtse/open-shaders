// ShadowCasterUI.cpp
// ImGui surfaces for the shadow caster scheduler: caster table, stats, overlay panels, settings.

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

#define I18N_KEY_PREFIX "feature.light_limit_fix."

namespace ShadowCasterManager
{
	// =========================================================================
	// DrawShadowLightTable
	// =========================================================================
	// Interactive shadow caster table: suppress/re-enable per light or by type,
	// filter by type name/range/address, sort by any column.
	// Rows are keyed by lightKey (light object pointer) so suppression persists
	// across slot reassignments as the player moves around.
	//
	// compact=true  -> auto-sizes height (up to 15 rows visible)
	// compact=false -> fills available window height (resizable overlay window)
	// showColor     -> adds a golden-ratio hue swatch column (visualization mode 8)
	// =========================================================================

	void DrawShadowLightTable(bool compact, bool showColor, bool sceneOnly, bool readOnly)
	{
		// Hover key is set per-row here and consumed (cleared) once per frame
		// by UpdateLights. Do NOT clear it at function entry -- if both the
		// settings-menu table and the overlay table render in the same frame,
		// a top-of-function clear would let the second table clobber the
		// hover set by the first. Only one row hovers at a time, so the two
		// callsites can't fight.

		struct SlotRow
		{
			uint32_t idx;           // shadow slot index; only meaningful when inScene=true
			bool inScene;           // currently occupies a shadow slot this frame
			bool converted;         // demoted to non-shadow rendering via ConvertExcessToNormal
			bool isFocus{ false };  // engine-owned focus shadow slot (read-only row)
			ShadowSlotInfo info;
			float importance{ 0.0f };  // contribution-weighted importance (luminance × fade × attenuation²)
			double score{ 0.0 };       // unified priority (ScoreFormula value)
			bool highImp{ false };     // importance > 0.1, light meaningfully illuminates the viewer area
		};

		// Build index of lights currently in scene (slot -> info).
		// Static containers avoid per-frame heap allocation.
		static std::unordered_map<uintptr_t, uint32_t> sceneSlot;
		sceneSlot.clear();
		for (uint32_t i = 0; i < static_cast<uint32_t>(s_shadowSlotInfos.size()); ++i)
			if (s_shadowSlotInfos[i].valid)
				sceneSlot[s_shadowSlotInfos[i].lightKey] = i;

		// Build lightKey -> LightEntry* lookup for debug columns.
		static std::unordered_map<uintptr_t, const LightEntry*> lightEntryByKey;
		lightEntryByKey.clear();
		for (int li = 0; li < s_lights.Size; ++li) {
			const auto& e = s_lights.Lights[li];
			if (e.Light)
				lightEntryByKey[reinterpret_cast<uintptr_t>(e.Light)] = &e;
		}

		auto applyEntryDebug = [&](SlotRow& row) {
			auto it = lightEntryByKey.find(row.info.lightKey);
			if (it != lightEntryByKey.end()) {
				row.importance = it->second->lastImportance;
				row.score = it->second->lastScore;
				row.highImp = row.importance > 0.1f;
			}
		};

		// Build set of converted-light keys (shadow lights demoted to non-shadow
		// rendering via ConvertExcessToNormal). These don't occupy a shadow slot
		// this frame but are still active in the scene as normal lights; we want
		// them visible in the table with a "Conv" indicator and the same suppress
		// toggle so users can hide them like any other shadow caster.
		static std::unordered_set<uintptr_t> convertedKeys;
		convertedKeys.clear();
		ForEachConvertedLight([&](RE::BSShadowLight* light) {
			convertedKeys.insert(reinterpret_cast<uintptr_t>(light));
		});

		// Build row list.
		static std::vector<SlotRow> rows;
		rows.clear();
		auto addConvertedRows = [&]() {
			for (uintptr_t key : convertedKeys) {
				if (sceneSlot.count(key))
					continue;  // simultaneously a shadow caster this frame
				SlotRow r{ 0, false, true, false, {} };
				auto it = s_knownLights.find(key);
				if (it != s_knownLights.end()) {
					r.info = it->second;
					r.info.valid = false;  // no shadow slot this frame
				} else {
					// First-frame convert: no cached metadata yet. Surface a minimal
					// row so the user can still toggle suppression by address.
					r.info.lightKey = key;
				}
				applyEntryDebug(r);
				rows.push_back(r);
			}
		};
		if (sceneOnly) {
			rows.reserve(sceneSlot.size() + convertedKeys.size());
			for (auto& [key, idx] : sceneSlot) {
				SlotRow r{ idx, true, false, false, s_shadowSlotInfos[idx] };
				applyEntryDebug(r);
				rows.push_back(r);
			}
			addConvertedRows();
		} else {
			// All scene lights first, then converted lights, then suppressed lights
			// not currently in scene at all.
			rows.reserve(sceneSlot.size() + convertedKeys.size() + s_suppressedLights.size());
			for (auto& [key, idx] : sceneSlot) {
				SlotRow r{ idx, true, false, false, s_shadowSlotInfos[idx] };
				applyEntryDebug(r);
				rows.push_back(r);
			}
			addConvertedRows();
			for (uintptr_t key : s_suppressedLights) {
				if (sceneSlot.count(key) || convertedKeys.count(key))
					continue;
				auto it = s_knownLights.find(key);
				if (it != s_knownLights.end()) {
					SlotRow r{ 0, false, false, false, it->second };
					applyEntryDebug(r);
					rows.push_back(r);
				}
			}
		}

		// Engine-owned focus shadow rows. One per active focus actor at the
		// matching kSHADOWMAPS slot. Synthetic lightKey encodes the slot index
		// so each row is unique without colliding with real BSShadowLight
		// pointers (top-half-set is impossible for user-mode allocations).
		for (int32_t i = 0; i < s_focusShadowSlots; ++i) {
			SlotRow r{};
			r.idx = static_cast<uint32_t>(kFocusShadowBaseSlotIndex + i);
			r.inScene = true;
			r.isFocus = true;
			r.info.valid = true;
			r.info.lightKey = 0xFEFE'0000ULL | static_cast<uint64_t>(r.idx);
			r.info.type = 0;  // surfaced as "Focus" in the type column override below
			rows.push_back(r);
		}

		if (rows.empty()) {
			ImGui::TextDisabled(T(TKEY("no_shadow_slots_this_frame"), "No shadow slots this frame."));
			return;
		}

		// === "Last changed" tracking (debug aid) ===========================
		// Stamp the time each light's role (shadow / converted / absent) last
		// changed, so the table's "Changed" column shows "N ago" and can be
		// sorted to float just-promoted/demoted/converted lights to the top --
		// stable while the player is still, jumps when something transitions.
		// Render-side and debug-only; no coupling to the scheduler.
		// Role codes for "Changed" tracking, ordered by shadow involvement.
		enum : uint8_t
		{
			kRoleAbsent = 0,
			kRoleConverted = 1,
			kRoleShadow = 2
		};
		static std::unordered_map<uintptr_t, uint8_t> s_rowRole;      // values are kRole* above
		static std::unordered_map<uintptr_t, double> s_rowChangedAt;  // ImGui time of last role change
		{
			const double now = ImGui::GetTime();
			static std::unordered_set<uintptr_t> seen;
			seen.clear();
			for (const auto& r : rows) {
				if (r.isFocus)
					continue;  // engine-owned; not user churn
				const uint8_t role = r.inScene ? kRoleShadow : (r.converted ? kRoleConverted : kRoleAbsent);
				seen.insert(r.info.lightKey);
				auto [it, inserted] = s_rowRole.try_emplace(r.info.lightKey, role);
				if (inserted) {
					// First sighting is itself a transition -- stamp it so a just-
					// appeared light shows an age and sorts as recently changed.
					s_rowChangedAt[r.info.lightKey] = now;
				} else if (it->second != role) {
					it->second = role;
					s_rowChangedAt[r.info.lightKey] = now;
				}
			}
			// A light that dropped out of the table (e.g. promoted shadow then
			// disabled) stamps a change so it re-surfaces when it reappears.
			for (auto& [key, role] : s_rowRole) {
				if (role != kRoleAbsent && !seen.count(key)) {
					role = kRoleAbsent;
					s_rowChangedAt[key] = now;
				}
			}
		}

		// -- Header: active count + suppression badge ----------------------
		ImGui::Text(T(TKEY("shadow_slots_active"), "Shadow slots: %u active"), s_shadowSlotUsage);
		if (!s_suppressedLights.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), T(TKEY("suppressed_count"), "  %zu suppressed"), s_suppressedLights.size());
		}

		// Group hover repopulates the highlight set; clear it every frame so
		// stale entries drop once a hover ends or the controls stop rendering.
		ClearHighlight();

		// -- Group toggle buttons ------------------------------------------
		// green = at least one unsuppressed; grey = all suppressed; click flips.
		// Predicate-based so we can mix type filters (Spot/Hemi/Omni) with state
		// filters (Conv = converted-to-normal lights).
		// Hidden in readOnly overlays -- these are live suppression controls.
		if (!readOnly) {
			using RowPred = std::function<bool(const SlotRow&)>;
			auto allSuppressedMatching = [&](const RowPred& pred) {
				for (auto& r : rows) {
					if (!pred(r))
						continue;
					if (!s_suppressedLights.count(r.info.lightKey))
						return false;
				}
				// No matching row is unsuppressed, including the case where
				// nothing matched at all -- grey/disabled either way (clicking
				// a no-op button does nothing).
				return true;
			};
			auto toggleMatching = [&](const RowPred& pred) {
				if (allSuppressedMatching(pred)) {
					for (auto& r : rows)
						if (pred(r))
							s_suppressedLights.erase(r.info.lightKey);
				} else {
					for (auto& r : rows)
						if (pred(r))
							s_suppressedLights.insert(r.info.lightKey);
				}
			};
			auto groupButton = [&](const char* label, const RowPred& pred, const char* tooltip, bool previewOnly = false) {
				// Counter shows visible/total: how many of the group's lights hold
				// a shadow slot this frame vs how many exist. Lets the user see a
				// group thin out as they move without opening every row.
				int total = 0, visible = 0;
				for (auto& r : rows) {
					if (!pred(r))
						continue;
					++total;
					if (r.inScene)
						++visible;
				}
				const std::string btnLabel = std::format("{} {}/{}", label, visible, total);

				bool allOff = allSuppressedMatching(pred);
				ImGui::PushStyleColor(ImGuiCol_Button,
					allOff ? ImVec4(0.35f, 0.35f, 0.35f, 1) : ImVec4(0.15f, 0.5f, 0.15f, 1));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
					allOff ? ImVec4(0.5f, 0.5f, 0.5f, 1) : ImVec4(0.2f, 0.7f, 0.2f, 1));
				if (ImGui::SmallButton(btnLabel.c_str()) && !previewOnly)
					toggleMatching(pred);
				ImGui::PopStyleColor(2);
				if (ImGui::IsItemHovered()) {
					// Hovering a group tints its whole set magenta in-world -- the
					// group-scale analogue of Shift+hover on one row. Populated
					// here, cleared at the table draw above; click toggles
					// suppression unless the group is preview-only.
					for (auto& r : rows)
						if (pred(r))
							AddHighlight(r.info.lightKey);
					if (tooltip)
						ImGui::SetTooltip("%s", tooltip);
				}
			};
			auto typePred = [](uint32_t type) {
				return [type](const SlotRow& r) { return r.info.type == type; };
			};
			groupButton(
				T(TKEY("group_btn_all"), "All"), [](const SlotRow&) { return true; }, nullptr);
			ImGui::SameLine();
			groupButton(T(TKEY("group_btn_spot"), "Spot"), typePred(0), T(TKEY("group_tip_spot"), "Toggle all spot/frustum shadow lights"));
			ImGui::SameLine();
			groupButton(T(TKEY("group_btn_hemi"), "Hemi"), typePred(1), T(TKEY("group_tip_hemi"), "Toggle all hemisphere shadow lights"));
			ImGui::SameLine();
			groupButton(T(TKEY("group_btn_omni"), "Omni"), typePred(2), T(TKEY("group_tip_omni"), "Toggle all omni shadow lights (dome projection, aka paraboloid)"));
			// Divider: left of the bar are stable type filters, right of it are
			// per-frame state filters whose membership changes as you move.
			ImGui::SameLine();
			ImGui::TextDisabled("|");
			ImGui::SameLine();
			groupButton(
				T(TKEY("group_btn_conv"), "Conv"), [](const SlotRow& r) { return r.converted; },
				T(TKEY("group_tip_conv"),
					"Toggle all lights currently demoted from shadow to normal\n"
					"(ConvertExcessToNormal). Hides their cluster-light contribution."));
			ImGui::SameLine();
			groupButton(
				T(TKEY("group_btn_high"), "High"), [](const SlotRow& r) { return r.inScene && r.highImp; },
				T(TKEY("group_tip_high"),
					"High-impact shadow lights (meaningfully light the view).\n"
					"Hover to tint them; click to toggle their suppression."));
			ImGui::SameLine();
			// Preview-only: the floor already culls these lights, so a suppress
			// click would have no visible effect. Disabled (dimmed) while the
			// floor is 0, since there's nothing for the group to preview.
			const bool floorOff = s_settings.ShadowImpactFloor <= 0.0f;
			if (floorOff)
				ImGui::BeginDisabled();
			groupButton(
				T(TKEY("group_btn_low"), "Low"),
				[](const SlotRow& r) { return IsBelowFloor(r.info.lightKey); },
				floorOff ?
					T(TKEY("group_tip_low_off"), "Light Impact Floor is 0, so nothing is culled to preview.") :
					T(TKEY("group_tip_low"),
						"Lights the Light Impact Floor is culling right now.\n"
						"Hover to highlight them in the world (preview only --\n"
						"clicking does nothing, the floor already hides these shadows)."),
				true);
			if (floorOff)
				ImGui::EndDisabled();

			// "Clear All": resets every debug override (suppress / pin shadow /
			// pin convert / solo) so the table returns to scheduler-auto. Only
			// shown when overrides are active so it doesn't take up space when
			// there's nothing to reset.
			if (HasAnyOverrides()) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.25f, 0.25f, 1));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.35f, 0.35f, 1));
				if (ImGui::SmallButton(T(TKEY("clear_all_btn"), "Clear All")))
					ClearAllOverrides();
				ImGui::PopStyleColor(2);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("clear_all_tooltip"),
												"Reset every debug override:\n"
												"  - clear suppression\n"
												"  - clear shadow / convert pins\n"
												"  - clear solo\n"
												"Returns the table to scheduler-auto behaviour."));
			}

			// Help marker: explains the per-row debug controls so users aren't
			// surprised by states / pulses they didn't know they could trigger.
			ImGui::SameLine();
			Util::HelpMarker(
				T(TKEY("per_row_controls_help"),
					"Per-row controls:\n"
					"  *  Cycle button (col 1): click to rotate this light through\n"
					"     Auto -> Shadow pin (S) -> Convert pin (C) -> Suppress (X) -> Auto.\n"
					"  *  Solo button (col 2): isolate this light against a black scene.\n"
					"     Click again to clear; only one light may be soloed at a time.\n"
					"  *  Hold Shift while hovering a row to highlight that light in the\n"
					"     world with a pulsing magenta tint. Release Shift or move the\n"
					"     cursor away to stop. Useful when you can't tell which entry\n"
					"     corresponds to which physical light. Does not affect rendering\n"
					"     when Shift is not held.\n\n"
					"Group buttons toggle suppression for every matching row at once.\n"
					"Hovering a group button highlights its lights in the world with\n"
					"no modifier needed -- Shift is only required for single-row\n"
					"hover highlighting in the table.\n"
					"Clear All appears when any override is active and resets everything."));
		}

		// -- Filter input --------------------------------------------------
		// Hidden in readOnly overlays; any filter set in the menu still applies.
		static std::string s_filterText;
		if (!readOnly) {
			char buf[128] = {};
			strncpy_s(buf, s_filterText.c_str(), sizeof(buf) - 1);
			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::InputText("##slotfilter", buf, sizeof(buf)))
				s_filterText = buf;
			ImGui::SameLine();
			ImGui::TextDisabled(sceneOnly ? T(TKEY("filter_hint_scene_only"), "filter (yes/conv/type/range/addr)") : T(TKEY("filter_hint"), "filter (yes/conv/no/type/range/addr)"));
			// Developer capture: same path as devbench capture kind=shadowmaps.
			if (AtlasActive()) {
				ImGui::SameLine();
				if (ImGui::SmallButton(T(TKEY("dump_atlas_btn"), "Dump Atlas")))
					RequestAtlasDump();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("dump_atlas_tooltip"),
												"Save the shadow atlas depth texture (DDS) plus a\n"
												"per-slot tile manifest (JSON) to\n"
												"Data/SKSE/Plugins/CommunityShaders/Captures.\n"
												"Written on the next shadow pass."));
			}
		}

		// Apply filter.
		static std::vector<SlotRow> filteredRows;
		filteredRows.clear();
		if (s_filterText.empty()) {
			filteredRows = rows;
		} else {
			std::string lower = s_filterText;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			char addrBuf[16];
			for (auto& r : rows) {
				std::string typeName = kShadowTypeNames[std::min(r.info.type, 2u)];
				std::transform(typeName.begin(), typeName.end(), typeName.begin(), ::tolower);
				// Range filter matches both raw units and rounded meters.
				char rangeBuf[32];
				snprintf(rangeBuf, sizeof(rangeBuf), "%.0f %.0f",
					r.info.range, Util::Units::GameUnitsToMeters(r.info.range));
				snprintf(addrBuf, sizeof(addrBuf), "%08x", static_cast<uint32_t>(r.info.lightKey & 0xFFFFFFFF));
				const char* statusStr = r.inScene ? "yes" : (r.converted ? "conv" : "no");
				std::string nameLower = r.info.name;
				std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (typeName.find(lower) != std::string::npos ||
					nameLower.find(lower) != std::string::npos ||
					std::string(rangeBuf).find(lower) != std::string::npos ||
					std::string(addrBuf).find(lower) != std::string::npos ||
					lower == statusStr)
					filteredRows.push_back(r);
			}
		}

		// Table-max priority for the Prio column colour ramp (user formulas
		// have arbitrary scale, so normalize to what is on screen).
		double maxRowScore = 1e-6;
		for (const auto& r : filteredRows)
			maxRowScore = std::max(maxRowScore, r.score);

		// -- Column layout -------------------------------------------------
		// Interactive (settings menu, or overlay with menu open):
		//     [Mode] [Solo] [Status] [Address] [Color?] [Type] [Range] [Imp]
		// Read-only (overlay with menu closed -- buttons would be dead pixels):
		//                   [Status] [Address] [Color?] [Type] [Range] [Imp]
		//
		// Status merges the old "In Scene" + "Slot" columns into one cell
		// showing one of: "Slot N" / "Conv" / "Out" / "Suppr". The old "Hi"
		// boolean column is gone -- highImp now tints the row instead, which
		// is what the column was being used for visually.
		const bool showButtons = !readOnly;
		const int modeColIdx = showButtons ? 0 : -1;
		const int soloColIdx = showButtons ? 1 : -1;
		const int statusColIdx = showButtons ? 2 : 0;
		const int addrColIdx = statusColIdx + 1;
		const int typeColIdx = addrColIdx + (showColor ? 2 : 1);
		const int radColIdx = typeColIdx + 1;
		const int resColIdx = radColIdx + 1;
		const int centrColIdx = resColIdx + 1;
		const int changedColIdx = centrColIdx + 1;

		std::vector<std::string> headers;
		if (showButtons) {
			headers.push_back(T(TKEY("col_mode"), "Mode"));  // cycle: Auto / Pin-S / Pin-C / Suppress
			headers.push_back(T(TKEY("col_solo"), "Solo"));
		}
		headers.push_back(T(TKEY("col_status"), "Status"));
		headers.push_back(T(TKEY("col_address"), "Address"));
		if (showColor)
			headers.push_back(T(TKEY("col_color"), "Color"));
		headers.push_back(T(TKEY("col_type"), "Type"));
		headers.push_back(T(TKEY("col_range"), "Range"));
		headers.push_back(T(TKEY("col_res"), "Res"));
		headers.push_back(T(TKEY("col_prio"), "Prio"));
		headers.push_back(T(TKEY("col_changed"), "Changed"));

		using SortFn = std::function<bool(const SlotRow&, const SlotRow&, bool)>;
		std::vector<SortFn> sorts(headers.size(), nullptr);
		// "Changed" sort: most-recently-changed role first. Stable while idle
		// (every age grows together); a just-transitioned light jumps to the top.
		sorts[changedColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			auto when = [](const SlotRow& r) -> double {
				auto it = s_rowChangedAt.find(r.info.lightKey);
				return it != s_rowChangedAt.end() ? it->second : -1.0;  // never-changed = oldest
			};
			double wa = when(a), wb = when(b);
			if (wa != wb)
				return asc ? wa > wb : wa < wb;  // ascending click => most recent on top
			return a.info.lightKey < b.info.lightKey;
		};
		// Status sort: in-scene shadow casters -> converted -> out-of-scene.
		// Suppressed lights sort to the end (treated as worst rank).
		sorts[statusColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			auto rank = [](const SlotRow& r) -> int {
				bool sup = s_suppressedLights.count(r.info.lightKey) > 0;
				if (sup)
					return 3;
				return r.inScene ? 0 : (r.converted ? 1 : 2);
			};
			int ra = rank(a), rb = rank(b);
			if (ra != rb)
				return asc ? ra < rb : ra > rb;
			return asc ? a.idx < b.idx : a.idx > b.idx;
		};
		sorts[addrColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			return asc ? a.info.lightKey < b.info.lightKey : a.info.lightKey > b.info.lightKey;
		};
		sorts[typeColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			return asc ? a.info.type < b.info.type : a.info.type > b.info.type;
		};
		sorts[radColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			return asc ? a.info.range < b.info.range : a.info.range > b.info.range;
		};
		sorts[centrColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			return asc ? a.score < b.score : a.score > b.score;
		};
		// Res sorts by the rendered tile scale the column displays; rows with no
		// tile (focus / out-of-scene, shown as "--") sink to the bottom.
		sorts[resColIdx] = [](const SlotRow& a, const SlotRow& b, bool asc) {
			auto res = [](const SlotRow& r) -> float {
				return (r.isFocus || !r.inScene) ? -1.0f : GetRenderedTileScale(static_cast<int32_t>(r.idx));
			};
			const float ra = res(a), rb = res(b);
			if (ra != rb)
				return asc ? ra < rb : ra > rb;
			return a.idx < b.idx;
		};

		// outerSize logic:
		//   * compact      auto-size up to 15 rows (handled by
		//                  ShowSortedStringTableCustom when y==0). Used in
		//                  the menu's Active Casters block where the table
		//                  is one of several elements in a long settings
		//                  list and shouldn't grab unbounded vertical space.
		//   * non-compact  fill remaining vertical space. The table itself
		//                  scrolls internally (ScrollY flag in the shared
		//                  helper) so summary stats above stay visible
		//                  regardless of how many lights exist or how the
		//                  user has sized the host window.
		ImVec2 outerSize = compact ? ImVec2(0, 0) : ImVec2(0, ImGui::GetContentRegionAvail().y);

		Util::ShowSortedStringTableCustom<SlotRow>(
			"##ShadowLightTbl",
			headers,
			filteredRows,
			static_cast<size_t>(statusColIdx),  // default sort: Status
			true,                               // ascending
			sorts,
			[&](int /*rowIdx*/, int col, const SlotRow& row) {
				const uintptr_t key = row.info.lightKey;
				const bool suppressed = s_suppressedLights.count(key) > 0;
				const bool pinShadow = s_pinShadow.count(key) > 0;
				const bool pinConvert = s_pinConvert.count(key) > 0;
				const bool isSolo = (s_soloLight == key && key != 0);

				// Sets s_hoverLightKey (magenta debug pulse in-world) only while
				// Shift is held, so normal row clicks don't trigger it.
				auto noteHover = [&]() {
					if (ImGui::IsItemHovered() && ImGui::GetIO().KeyShift)
						s_hoverLightKey = key;
				};

				// Row tint: highImp lights get a subtle yellow background so
				// the eye can pick out the lights actually contributing to the
				// frame at a glance. Replaces the dropped "Hi" column. Set on
				// col 0 so it applies to the whole row.
				if (col == 0 && row.highImp) {
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
						ImGui::GetColorU32(ImVec4(0.30f, 0.30f, 0.10f, 0.35f)));
				}

				// === Mode column: state cycle button =======================
				// Cycle: Auto (·) -> PinShadow (S) -> PinConvert (C) -> Suppress (X) -> Auto
				// Mutually exclusive (SetPinned* / suppressed.erase enforce that).
				// Hidden in readOnly mode (overlay with menu closed).
				// Focus rows skip Mode/Solo entirely -- engine owns the slot.
				if (row.isFocus && col == modeColIdx) {
					ImGui::TextDisabled(T(TKEY("mode_eng"), "eng"));
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("mode_eng_tooltip"), "Engine-controlled focus shadow; not pinnable/suppressible."));
					return;
				}
				if (row.isFocus && col == soloColIdx) {
					ImGui::TextDisabled("--");
					return;
				}
				if (showButtons && col == modeColIdx) {
					// Full pointer -- truncating to 32 bits let two different
					// lights collide onto the same ImGui ID.
					ImGui::PushID(reinterpret_cast<void*>(key));
					const char* label = "·";
					ImVec4 col4 = ImVec4(0.15f, 0.6f, 0.15f, 1);  // green = auto/active
					ImVec4 colH = ImVec4(0.2f, 0.75f, 0.2f, 1);
					const char* tip = T(TKEY("mode_tip_auto"), "Auto (scheduler decides)\nClick: pin as shadow caster");
					if (pinShadow) {
						label = "S";
						col4 = ImVec4(0.20f, 0.40f, 0.85f, 1);  // blue
						colH = ImVec4(0.30f, 0.55f, 1.0f, 1);
						tip = T(TKEY("mode_tip_pin_shadow"), "Pinned: forced shadow caster\nClick: pin as converted (non-shadow)");
					} else if (pinConvert) {
						label = "C";
						col4 = ImVec4(0.85f, 0.55f, 0.15f, 1);  // amber
						colH = ImVec4(1.0f, 0.7f, 0.25f, 1);
						tip = T(TKEY("mode_tip_pin_convert"), "Pinned: forced converted (non-shadow)\nClick: suppress entirely");
					} else if (suppressed) {
						label = "X";
						col4 = ImVec4(0.45f, 0.25f, 0.25f, 1);  // dim red
						colH = ImVec4(0.6f, 0.35f, 0.35f, 1);
						tip = T(TKEY("mode_tip_suppressed"), "Suppressed (hidden)\nClick: return to auto");
					}
					ImGui::PushStyleColor(ImGuiCol_Button, col4);
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colH);
					if (ImGui::SmallButton(label)) {
						// Cycle to next state.
						if (pinShadow) {
							SetPinnedShadow(key, false);
							SetPinnedConvert(key, true);
						} else if (pinConvert) {
							SetPinnedConvert(key, false);
							s_suppressedLights.insert(key);
						} else if (suppressed) {
							s_suppressedLights.erase(key);
						} else {
							SetPinnedShadow(key, true);
						}
					}
					ImGui::PopStyleColor(2);
					noteHover();
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", tip);
					ImGui::PopID();
					return;
				}

				// === Solo column ==========================================
				// Hidden in readOnly mode.
				if (showButtons && col == soloColIdx) {
					// Full pointer (see the Mode column's PushID above for why).
					ImGui::PushID(reinterpret_cast<void*>(key ^ 0xA1));
					ImVec4 col4 = isSolo ?
				                      ImVec4(0.85f, 0.7f, 0.15f, 1) :  // bright yellow when active
				                      ImVec4(0.30f, 0.30f, 0.30f, 1);
					ImVec4 colH = isSolo ? ImVec4(1.0f, 0.85f, 0.25f, 1) : ImVec4(0.45f, 0.45f, 0.45f, 1);
					ImGui::PushStyleColor(ImGuiCol_Button, col4);
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colH);
					if (ImGui::SmallButton(isSolo ? "!" : "·"))
						SetSoloLight(isSolo ? 0 : key);
					ImGui::PopStyleColor(2);
					noteHover();
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s",
							isSolo ?
								T(TKEY("solo_tip_active"), "Solo: this light is shown alone\nClick: clear solo") :
								T(TKEY("solo_tip"), "Solo this light\n(suppresses every other light\nuntil cleared)"));
					ImGui::PopID();
					return;
				}

				if (suppressed || (s_soloLight != 0 && !isSolo))
					ImGui::BeginDisabled();
				bool dimmed = suppressed || (s_soloLight != 0 && !isSolo);
				if (col == statusColIdx) {
					// Merged "In Scene" + "Slot" column. Four mutually-exclusive
					// states; suppressed wins because the user explicitly hid it.
					if (suppressed) {
						ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1), T(TKEY("status_suppr"), "Suppr"));
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", T(TKEY("status_suppr_tooltip"), "Suppressed by debug override.\nClick the Mode button to clear."));
					} else if (row.inScene) {
						ImGui::Text(T(TKEY("status_slot"), "Slot %u"), row.idx);
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(T(TKEY("status_slot_tooltip"), "Casting shadows this frame in slot %u."), row.idx);
					} else if (row.converted) {
						ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1), T(TKEY("status_conv"), "Conv"));
						if (ImGui::IsItemHovered()) {
							// Append the per-light demotion reason (captured from the
							// validation flags) so it's clear WHY this light has no shadow.
							const char* reason = ConvertReasonText(row.info.lightKey);
							ImGui::SetTooltip("%s%s%s", T(TKEY("status_conv_tooltip"),
															"Demoted to a normal (non-shadow) light this frame.\n"
															"Cluster lighting still illuminates it; no shadow-map cost."),
								reason ? "\n" : "", reason ? reason : "");
						}
					} else {
						ImGui::TextDisabled(T(TKEY("status_out"), "Out"));
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", T(TKEY("status_out_tooltip"), "Out of range / not active in the current frame."));
					}
				} else if (col == addrColIdx) {
					if (row.isFocus) {
						ImGui::TextDisabled(T(TKEY("addr_focus"), "focus[%u]"), row.idx - static_cast<uint32_t>(kFocusShadowBaseSlotIndex));
					} else {
						char addrFull[20];
						snprintf(addrFull, sizeof(addrFull), "0x%016llX", static_cast<unsigned long long>(row.info.lightKey));
						// Show the resolved world-light name when one exists;
						// the raw address stays available in the tooltip/copy.
						ImGui::Selectable(!row.info.name.empty() ? row.info.name.c_str() : addrFull + 10,
							false, ImGuiSelectableFlags_None);
						if (ImGui::IsItemClicked())
							ImGui::SetClipboardText(addrFull);
						noteHover();
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(T(TKEY("addr_click_to_copy"), "Click to copy: %s"), addrFull);
					}
				} else if (showColor && col == addrColIdx + 1) {
					ImVec4 c = ShadowSlotHueColor(row.idx);
					auto ri = static_cast<uint8_t>(c.x * 255.0f);
					auto gi = static_cast<uint8_t>(c.y * 255.0f);
					auto bi = static_cast<uint8_t>(c.z * 255.0f);
					ImGui::ColorButton("##col", c,
						ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(22, 16));
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("#%02X%02X%02X", ri, gi, bi);
				} else if (col == typeColIdx) {
					if (row.isFocus) {
						ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f), T(TKEY("type_focus"), "Focus"));
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(
								T(TKEY("type_focus_tooltip"),
									"Engine-owned focus shadow slot.\n"
									"FocusShadowActors[%u] = high-res shadow for a tracked\n"
									"actor (player + dialog/combat NPCs). SCM reserves\n"
									"this slot so the engine's focus render isn't trampled\n"
									"by point/spot lights."),
								row.idx - static_cast<uint32_t>(kFocusShadowBaseSlotIndex));
					} else {
						ImGui::TextUnformatted(kShadowTypeNames[std::min(row.info.type, 2u)]);
						noteHover();
					}
				} else if (col == radColIdx) {
					if (row.isFocus) {
						ImGui::TextDisabled("--");
					} else {
						ImGui::Text("%.0f u", row.info.range);
						noteHover();
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", Util::Units::FormatDistance(row.info.range).c_str());
					}
				} else if (col == resColIdx) {
					if (row.isFocus || !row.inScene) {
						ImGui::TextDisabled("--");
					} else {
						const float scale = GetRenderedTileScale(static_cast<int32_t>(row.idx));
						const float base = s_initialShadowMapResolution > 0 ? static_cast<float>(s_initialShadowMapResolution) : 2048.0f;
						ImGui::Text("%.0f", base * scale);
						noteHover();
						if (ImGui::IsItemHovered()) {
							AtlasTileTexels tileInfo{};
							if (AtlasActive() && GetSlotTileTexels(static_cast<int32_t>(row.idx), tileInfo))
								ImGui::SetTooltip(T(TKEY("res_tooltip_atlas"),
													  "Rendered shadow resolution (texels).\nAtlas tile: %ux%u at (%u, %u)%s"),
									tileInfo.size, tileInfo.size, tileInfo.x, tileInfo.y,
									tileInfo.contentValid ? "" : T(TKEY("res_tooltip_pending"), "\nContent pending first redraw."));
							else
								ImGui::SetTooltip("%s", T(TKEY("res_tooltip"),
															"Rendered shadow resolution (texels).\nImportant lights keep full size; minor ones shrink."));
						}
					}
				} else if (col == centrColIdx) {
					// Unified priority (ScoreFormula). Colour normalized to the
					// table max: user formulas have arbitrary scale.
					float t = static_cast<float>(std::clamp(row.score / maxRowScore, 0.0, 1.0));
					ImVec4 colour = ImVec4(1.0f - t * 0.7f, 1.0f, 1.0f - t * 0.7f, 1.0f);  // white → green
					ImGui::TextColored(colour, "%.2f", row.score);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("prio_tooltip"),
													"Priority (the Score Formula's value for this light).\n"
													"One number decides everything: which lights cast\n"
													"shadows, their order for atlas space within a\n"
													"resolution class, and how often they redraw (by\n"
													"priority rank).\n\n"
													"Edit the formula in the Formula Editor below.\n\n"
													"Rows tinted yellow deliver meaningful illumination\n"
													"near the camera or player."));
				} else if (col == changedColIdx) {
					auto chIt = s_rowChangedAt.find(key);
					if (row.isFocus || chIt == s_rowChangedAt.end()) {
						ImGui::TextDisabled("--");
					} else {
						double age = ImGui::GetTime() - chIt->second;
						if (age < 0.0)
							age = 0.0;
						constexpr int kSecPerMin = 60;
						constexpr double kSecPerHour = 3600.0;
						if (age < kSecPerMin)
							ImGui::Text("%.1fs", age);
						else if (age < kSecPerHour)
							ImGui::Text("%dm%02ds", static_cast<int>(age) / kSecPerMin, static_cast<int>(age) % kSecPerMin);
						else
							ImGui::Text("%dm", static_cast<int>(age) / kSecPerMin);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("changed_tooltip"), "Time since this light last changed role (shadow / converted / out).\nSort this column to bring just-changed lights to the top."));
				}
				// Hi column dropped -- highImp now tints the row background
				// (see TableSetBgColor at the top of this lambda) so the visual
				// signal is preserved without consuming a column.
				if (dimmed)
					ImGui::EndDisabled();
			},
			{},
			outerSize);
	}

	void DrawShadowSummary(uint32_t clusterCount, uint32_t clusterMax, uint32_t shadowUnshadowedLightCount)
	{
		// Canonical "where are we vs the limits" panel. Used by both the menu's
		// Active Casters block and the overlay header so testers see the same
		// numbers in the same format regardless of which view they're in.
		const uint32_t slotUsage = s_shadowSlotUsage;
		const uint32_t slots = GetInstalledSlotCount();
		// "Wanted" = total shadow-eligible demand this frame (active + dropped).
		// We don't track demand separately, but slotUsage + dropped is the
		// observable proxy that matches the user-visible "X dropped" signal.
		const uint32_t requested = slotUsage + shadowUnshadowedLightCount;

		if (clusterCount >= clusterMax)
			ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), T(TKEY("cluster_lights_overflow"), "Cluster lights : %u / %u (overflow)"), clusterCount, clusterMax);
		else
			ImGui::Text(T(TKEY("cluster_lights"), "Cluster lights : %u / %u"), clusterCount, clusterMax);

		// "lights" rather than "slots" matches the Shadow Light Count
		// setting name -- users think in lights, the engine thinks in
		// texture slots, so we use the user's word.
		if (shadowUnshadowedLightCount > 0)
			ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
				T(TKEY("shadow_lights_dropped"), "Shadow lights  : %u / %u  (%u wanted, %u dropped, %zu converted)"),
				slotUsage, slots, requested, shadowUnshadowedLightCount, s_normalConvert.size());
		else
			ImGui::Text(T(TKEY("shadow_lights"), "Shadow lights  : %u / %u  (%u wanted, 0 dropped, %zu converted)"),
				slotUsage, slots, requested, s_normalConvert.size());

		if (s_highImportanceLightCount > 0 && ImGui::IsItemHovered())
			ImGui::SetTooltip(T(TKEY("high_importance_tooltip"), "%u high-importance (near camera/player)."),
				s_highImportanceLightCount);
	}

	void DrawShadowSchedulerStats()
	{
		// Avg redraws/frame: rolling average of how many shadow casters per frame
		// the scheduler decided to (re)render. Bounded by MaxRedrawPerFrame.
		float avgRedraws = static_cast<float>(s_redrawSum) / static_cast<float>(kRedrawHistorySize);
		ImGui::Text(T(TKEY("avg_redraws_per_frame"), "Avg redraws/frame : %.1f  (cap: %d)"), avgRedraws, s_settings.MaxRedrawPerFrame);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(T(TKEY("avg_redraws_tooltip"), "Rolling average over the last %d frames."), kRedrawHistorySize);

		// Avg per-light cost: budget tracker's measured GPU cost per shadow caster.
		// Used by the formula budget mode to decide how many casters fit in the
		// per-frame time budget.
		int32_t avgCost = s_budget.GetAverageCostUs();
		if (avgCost > 0)
			ImGui::Text(T(TKEY("avg_light_cost"), "Avg light cost    : %.2f ms"), avgCost / 1000.0f);

		if (AtlasActive()) {
			int classCounts[5] = {};  // full .. sixteenth
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				const auto& e = s_lights.Lights[i];
				if (!e.Light)
					continue;
				int cls = 0;
				for (float s = kTileScaleFull; e.renderedScale < s && cls < 4; s *= 0.5f)
					cls++;
				classCounts[cls]++;
			}
			ImGui::Text(T(TKEY("tile_class_counts"), "Shadow resolution : %d full / %d half / %d quarter / %d eighth / %d sixteenth"),
				classCounts[0], classCounts[1], classCounts[2], classCounts[3], classCounts[4]);
			if (AtlasActive()) {
				ImGui::Text(T(TKEY("atlas_usage"), "Shadow atlas       : %ux%u, %.0f%% used, %.0f MB"),
					AtlasDim(), AtlasDim(), AtlasOccupancy() * 100.0f,
					static_cast<float>(AtlasVRAMBytes()) / (1024.f * 1024.f));
				ImGui::Text(T(TKEY("atlas_reallocs"), "Tile reallocations : %u since launch"),
					GetAtlasClearStats().tileReallocs);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("atlas_reallocs_tooltip"),
												"Each reallocation is a light changing resolution class,\n"
												"which discards its cached shadow and forces a redraw.\n"
												"A fast-growing number means the scene is fighting the\n"
												"class hysteresis; a slow one means the cache is holding."));
			}
		}

		// ---- Budget verdict ---------------------------------------------
		// Cross-checks measured shadow cost against the user-chosen budget
		// to surface "is your setup actually working?" without making the
		// user math it out themselves. We compare measured shadow time to
		// the user's chosen shadow budget -- not to total frame time -- so
		// this is "are we honouring your settings?" not "are your settings
		// right for your hardware?". The latter genuinely needs data we
		// don't own (frame target, GPU headroom, async overlap).
		const float budgetMs = s_autoBudgetMs;  // active budget (Manual = slider, Formula = computed)
		const float costMs = avgCost / 1000.0f;
		const float usedMs = avgRedraws * costMs;
		const int32_t cap = s_settings.MaxRedrawPerFrame;
		const bool capLimited = avgCost > 0 && avgRedraws >= static_cast<float>(cap) * 0.95f;
		const bool slotLimited = (s_shadowSlotUsage + 0u) >= GetInstalledSlotCount();
		const bool overBudget = avgCost > 0 && budgetMs > 0.0f && usedMs > budgetMs * 1.0f;
		const bool headroom = avgCost > 0 && budgetMs > 0.0f && usedMs < budgetMs * 0.5f && !capLimited;

		if (avgCost <= 0 || budgetMs <= 0.0f) {
			ImGui::TextDisabled(T(TKEY("budget_usage_warming_up"), "Budget usage      : (warming up)"));
			return;
		}

		// Verdicts named after the user-visible settings, not internal
		// engineering terms. Tooltips kept to one short line each so the
		// hover doesn't grow into a wall of text.
		ImVec4 col;
		const char* verdict;
		const char* tip;
		if (overBudget) {
			col = ImVec4(0.95f, 0.35f, 0.35f, 1);
			verdict = T(TKEY("verdict_over_budget"), "OVER BUDGET");
			tip = T(TKEY("verdict_over_budget_tip"), "Shadow time exceeds Redraw Budget. Lower Max Redraws or raise Redraw Budget.");
		} else if (capLimited && slotLimited) {
			col = ImVec4(0.95f, 0.65f, 0.25f, 1);
			verdict = T(TKEY("verdict_at_limits"), "AT LIMITS");
			tip = T(TKEY("verdict_at_limits_tip"), "Both Max Redraws and Shadow Light Count are full. Enable Convert to Normal or raise Shadow Light Count.");
		} else if (slotLimited) {
			col = ImVec4(0.95f, 0.65f, 0.25f, 1);
			verdict = T(TKEY("verdict_light_limited"), "LIGHT LIMITED");
			tip = T(TKEY("verdict_light_limited_tip"), "Shadow Light Count is full. Enable Convert to Normal or raise Shadow Light Count.");
		} else if (capLimited) {
			col = ImVec4(0.95f, 0.85f, 0.25f, 1);
			verdict = T(TKEY("verdict_redraw_limited"), "REDRAW LIMITED");
			tip = T(TKEY("verdict_redraw_limited_tip"), "Hitting Max Redraws Per Frame. Raise it to spend the unused Redraw Budget.");
		} else if (headroom) {
			col = ImVec4(0.55f, 0.85f, 0.55f, 1);
			verdict = T(TKEY("verdict_headroom"), "HEADROOM");
			tip = T(TKEY("verdict_headroom_tip"), "Under half the Redraw Budget is being used. Raise Max Redraws or accept the slack.");
		} else {
			col = ImVec4(0.55f, 0.85f, 0.55f, 1);
			verdict = T(TKEY("verdict_ok"), "OK");
			tip = T(TKEY("verdict_ok_tip"), "Within Redraw Budget; no limits hit.");
		}
		// Budget gauge: progress bar tinted by the verdict colour so the
		// state is readable at a glance, with the numeric reading and
		// verdict label inside the bar. One widget replaces the old
		// separate progress bar (in SCM settings) + verdict text line.
		const float fraction = std::min(usedMs / budgetMs, 1.0f);
		char overlay[80];
		snprintf(overlay, sizeof(overlay), "%.2f / %.2f ms  -  %s", usedMs, budgetMs, verdict);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
		ImGui::Text("%s", T(TKEY("budget_usage_label"), "Budget usage      :"));
		ImGui::SameLine();
		ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tip);

		// ---- Shadow VRAM progress bar ----
		// Bar fills `currentUsage / budget` (process headroom); overlay text
		// shows the kSHADOWMAPS array's share of that. Same DXGI data source
		// as PerformanceOverlay.
		auto vinfo = GetVRAMInfo();
		if (vinfo.valid && vinfo.budgetBytes > 0) {
			const std::uint64_t freeBytes = vinfo.budgetBytes > vinfo.currentUsageBytes ? vinfo.budgetBytes - vinfo.currentUsageBytes : 0;
			const float arrayMB = static_cast<float>(vinfo.shadowArrayBytes) / (1024.f * 1024.f);
			const float freeMB = static_cast<float>(freeBytes) / (1024.f * 1024.f);
			const float usageMB = static_cast<float>(vinfo.currentUsageBytes) / (1024.f * 1024.f);
			const float budgetMBf = static_cast<float>(vinfo.budgetBytes) / (1024.f * 1024.f);
			const float perSliceMB = static_cast<float>(vinfo.bytesPerSlice) / (1024.f * 1024.f);
			// Disambiguated from the budget-verdict string above.
			const VRAMVerdict vramVerdict = EvaluateVRAMVerdict(vinfo.shadowArrayBytes, freeBytes, vinfo.budgetBytes);
			const float fillFraction = std::min(1.0f,
				static_cast<float>(vinfo.currentUsageBytes) / static_cast<float>(vinfo.budgetBytes));
			char overlayText[96];
			snprintf(overlayText, sizeof(overlayText),
				T(TKEY("shadow_vram_overlay"), "%.0f / %.0f MB  -  shadows %.0f MB (%u slices)"),
				usageMB, budgetMBf, arrayMB, vinfo.shadowSlices);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vramVerdict.colour);
			ImGui::Text("%s", T(TKEY("shadow_vram_label"), "Shadow VRAM       :"));
			ImGui::SameLine();
			ImGui::ProgressBar(fillFraction, ImVec2(-1.0f, 0.0f), overlayText);
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					T(TKEY("shadow_vram_tooltip"),
						"Bar fill = process VRAM usage / graphics memory budget (same\n"
						"data the performance overlay reports). Overlay text shows the\n"
						"shadow array's contribution to that usage.\n"
						"\n"
						"Slices  : %u  (the sun uses its own shadow texture)\n"
						"Per slice : %.2f MB  (%u x %u @ %u B/pixel)\n"
						"Shadow array : %.1f MB\n"
						"Free in budget : %.1f MB\n"
						"\n"
						"Green when free VRAM and shadow share are comfortable.\n"
						"Yellow when free < 512 MB or shadow array > 25%% of budget.\n"
						"Red when free < 128 MB or shadow array > 50%% of budget --\n"
						"%s"),
					vinfo.shadowSlices, perSliceMB,
					vinfo.shadowWidth, vinfo.shadowHeight,
					vinfo.shadowWidth && vinfo.shadowHeight ? vinfo.bytesPerSlice / (vinfo.shadowWidth * vinfo.shadowHeight) : 0u,
					arrayMB, freeMB,
					// The atlas pins the array at the vanilla slice count, so the
					// light count no longer buys VRAM back; the atlas texture does.
					AtlasActive() ?
						T(TKEY("shadow_vram_remedy_atlas"), "lower Atlas Resolution.") :
						T(TKEY("shadow_vram_remedy_array"), "lower Shadow Light Count or the game's shadow resolution."));
			}
		}
	}

	void DrawOverlayShadowModeInfo(uint32_t mode, uint32_t /*shadowUnshadowedLightCount*/, uint32_t /*totalLightCount*/)
	{
		// Cluster light count, slot usage, requested/dropped/converted are all
		// covered by DrawShadowSummary above this in the overlay header. This
		// function now carries only mode-specific information that wouldn't be
		// meaningful elsewhere -- channel meanings, heatmap legends, etc.
		if (mode == 3) {
			ImGui::Text("%s", T(TKEY("mode3_r_channel"), "R channel  = directional soft shadow"));
			ImGui::Text("%s", T(TKEY("mode3_g_channel"), "G channel  = directional detailed shadow"));
			ImGui::TextDisabled("%s", T(TKEY("mode3_b_unused"), "(B = unused)"));
		} else if (mode == 4) {
			ImGui::TextDisabled("%s", T(TKEY("mode4_heatmap"), "Pixel heatmap: 0=blue  8+=red"));
		} else if (mode == 5) {
			ImGui::TextDisabled("%s", T(TKEY("mode5_lit_shadow"), "White = fully lit,  black = fully in shadow"));
		} else if (mode == 6) {
			ImGui::TextDisabled("%s", T(TKEY("mode6_heatmap"), "Pixel heatmap: 0=blue  8+=red (lights without shadow maps)"));
		} else if (mode == 7) {
			ImGui::TextDisabled("%s", T(TKEY("mode7_cool"), "Cool  Turbo[0.0-0.3] = 1-4 shadows"));
			ImGui::TextDisabled(T(TKEY("mode7_warm"), "Warm  Turbo[0.3-0.8] = 5-%u shadows"), GetInstalledSlotCount());
			ImGui::TextDisabled("%s", T(TKEY("mode7_red"), "Red                  = overflow"));
		} else if (mode == 9) {
			uint32_t spotC = 0, hemiC = 0, omniC = 0;
			for (const auto& info : GetSlotInfos()) {
				if (!info.valid)
					continue;
				if (info.type == 0)
					spotC++;
				else if (info.type == 1)
					hemiC++;
				else
					omniC++;
			}
			ImGui::Text(T(TKEY("mode9_spot"), "R  Spot (frustum)   : %u"), spotC);
			ImGui::Text(T(TKEY("mode9_hemi"), "G  Hemisphere       : %u"), hemiC);
			ImGui::Text(T(TKEY("mode9_omni"), "B  Omni (paraboloid): %u"), omniC);
		}
	}

	void DrawVisualisationTooltipShadowModes()
	{
		ImGui::Text("%s", T(TKEY("visualisation_tooltip_shadow_modes"),
							  "\n"
							  "Shadow Mask: R=directional soft shadow, G=directional detailed shadow.\n"
							  "\n"
							  "Shadow Light Count: Heatmap of shadow-casting point/spot lights per pixel (blue=0, red=8+).\n"
							  "Use to gauge shadow density; high counts indicate expensive shadow sampling.\n"
							  "\n"
							  "Point Light Shadow Factor: Brightness shows the darkest shadow value from any point/spot\n"
							  "light. White=fully lit, black=fully shadowed. Shows where PCF/PCSS filtering is active.\n"
							  "\n"
							  "Unshadowed Point Lights: Heatmap of point/spot lights without shadow maps (blue=0, red=8+).\n"
							  "High values where lights are bright indicate where the shadow slot limit is costing quality.\n"
							  "\n"
							  "Shadow Caster Density: Custom Turbo ranges show how heavily shadow slots are used.\n"
							  "  Cool (Turbo 0.0-0.3): 1-4 shadow lights per pixel.\n"
							  "  Warm (Turbo 0.3-0.8): 5 to ShadowMapSlots lights (dynamic range).\n"
							  "  Bright red: overflow - a light wanted a shadow slot but none was available.\n"
							  "\n"
							  "Shadow Slot Index Color: Assigns each shadow-map slot a unique high-contrast hue\n"
							  "(golden-ratio sequence) so you can identify which slot is casting the primary shadow.\n"
							  "First valid shadow light index per pixel is shown. Bright red = slot overflow.\n"
							  "\n"
							  "Light Type Visualization: RGB channels encode shadow light types per pixel.\n"
							  "  R = spot/frustum lights (ShadowParam.x == 0).\n"
							  "  G = hemisphere lights (single dome projection; ShadowParam.x == 1).\n"
							  "  B = omnidirectional lights (full dome projection; ShadowParam.x == 2).\n"
							  "  Dark grey = unshadowed lights only (no shadow maps assigned).\n"
							  "  Bright red = overflow (slot capacity exceeded).\n"
							  "Intensity scales with count (up to 4); channels blend for mixed-type pixels."));
	}

	// Shared by the Performance-tab quick presets and the Advanced-section
	// presets: applies floor+cull together and highlights the active match.
	static void DrawImpactCullPresetButton(Settings& settings, const char* label, const char* tip, float floor, float cull)
	{
		const bool active = settings.ShadowImpactFloor == floor &&
		                    settings.CasterCullAngularMin == cull;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::SmallButton(label)) {
			settings.ShadowImpactFloor = floor;
			settings.CasterCullAngularMin = cull;
		}
		if (active)
			ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tip);
	}

	void DrawImpactCullPresetButtons(Settings& settings)
	{
		DrawImpactCullPresetButton(settings, T(TKEY("preset_quality"), "Quality"),
			T(TKEY("preset_quality_tip"), "No shadow culling (default)."), 0.0f, 0.0f);
		ImGui::SameLine();
		DrawImpactCullPresetButton(settings, T(TKEY("preset_balanced"), "Balanced"),
			T(TKEY("preset_balanced_tip"),
				"Drop shadows you can barely see.\n"
				"Keeps carried and nearby lights shadowed."),
			0.001f, 0.008f);
		ImGui::SameLine();
		DrawImpactCullPresetButton(settings, T(TKEY("preset_performance"), "Performance"),
			T(TKEY("preset_performance_tip"),
				"Stronger impact floor.\n"
				"May drop shadows from minor distant lights."),
			0.025f, 0.012f);
	}

	void DrawImpactCullSliders(Settings& settings)
	{
		ImGui::SliderFloat(T(TKEY("caster_cull_angular"), "Caster Cull Screen Size Min"),
			&settings.CasterCullAngularMin, 0.0f, 0.1f, "%.4f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T(TKEY("caster_cull_angular_tooltip"),
										"Skip a shadow caster when its on-screen size from your viewpoint\n"
										"(bound radius / distance to camera) is below this. A close caster\n"
										"filling the view is kept even if small; a distant one in a corner\n"
										"is dropped even if large. Large speedup in cluttered interiors.\n"
										"0 disables. Useful range is small -- Balanced/Performance presets\n"
										"use 0.008/0.012. Ctrl+Click the slider to type an exact value.\n"
										"Lower it if distant shadows look like they pop in."));

		ImGui::SliderFloat(T(TKEY("shadow_impact_floor"), "Light Impact Floor"),
			&settings.ShadowImpactFloor, 0.0f, 0.2f, "%.3f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T(TKEY("shadow_impact_floor_tooltip"),
										"Drop the whole shadow from a light whose on-screen relevance\n"
										"(screen coverage, or how much it lights you/the camera) is below\n"
										"this. The light still lights the room; it just stops redrawing a\n"
										"near-invisible shadow. 0 disables. Hover the Low group button\n"
										"above the light table to preview which lights it would affect."));
	}

	void DrawImpactCullControls(Settings& settings)
	{
		DrawImpactCullPresetButtons(settings);
		DrawImpactCullSliders(settings);
	}

	void DrawSettings(Settings& settings)
	{
		ImGui::SeparatorText(T(TKEY("shadow_limit_fix_header"), "Shadow Limit Fix"));
		// The Performance hub's "Shadow Limit Fix" subsection link sets this anchor
		// (Menu::SelectFeatureMenu) so clicking it scrolls here even if the panel was
		// last left scrolled elsewhere, instead of relying on this being drawn first.
		if (auto* menu = Menu::GetSingleton(); menu && menu->ConsumeSectionAnchor("ShadowLimitFix"))
			ImGui::SetScrollHereY(0.0f);

		// ---- External conflict banner --------------------------------------
		if (s_externalConflict) {
			const auto& theme = Menu::GetSingleton()->GetTheme();
			ImGui::TextColored(theme.StatusPalette.Error, "%s", s_conflictMessage.c_str());
			ImGui::BeginDisabled();
		}

		// ---- Enable toggle (requires restart) ------------------------------
		ImGui::Checkbox(T(TKEY("enable_shadow_limit_fix"), "Enable Shadow Limit Fix"), &settings.Enabled);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T(TKEY("enable_shadow_limit_fix_tooltip"),
										"Extends Skyrim's hard limit of 4 simultaneous shadow-casting lights.\n"
										"Intelligently selects which lights cast shadows each frame based on\n"
										"distance, intensity, and a configurable priority formula.\n\n"
										"Based on Intellightent by meh321.\n"
										"https://www.nexusmods.com/skyrimspecialedition/mods/172423\n\n"
										"Restart required to take effect in either direction. The boot-time\n"
										"patches (extended atlas slices, depth buffer creation loop, color-mask\n"
										"pass replacement) cannot be safely reversed at runtime -- vanilla\n"
										"shadow scheduling crashes when run on top of them. Toggle and restart."));
		// Either direction requires restart -- the boot-time patches modify
		// the engine's shadow texture array, depth buffer creation, and
		// color-mask pass. Vanilla scheduling cannot run on top of those
		// (verified by AV in BSShadowDirectionalLight processing during a
		// runtime-disable test, 2026-05-17 crash logs).
		//
		// Compare the user's current value against the BOOT value, not
		// against s_settings -- s_settings updates when the user saves,
		// so a stale comparison against s_settings would hide the label
		// the instant the user clicked Save Settings, leaving them with
		// no indication that their change won't apply until restart.
		if (s_bootEnabledCaptured && settings.Enabled != s_bootEnabled) {
			const auto& theme = Menu::GetSingleton()->GetTheme();
			ImGui::TextColored(theme.StatusPalette.RestartNeeded,
				T(TKEY("restart_session_state"), "Restart required -- this session is %s."), s_bootEnabled ? T(TKEY("session_enabled"), "enabled") : T(TKEY("session_disabled"), "disabled"));
		}

		if (!settings.Enabled)
			ImGui::BeginDisabled();

		// ---- Shadow Light Count (requires restart) -------------------------
		// Upper bound of 127: the engine refuses to render any shadow caster
		// when ShadowLightCount >= 128 even though kSHADOWMAPS allocates
		// successfully -- some internal limit (likely an 8-bit shadow index
		// somewhere we haven't patched) silently disables shadow rendering.
		// 127 is the highest value that actually works.
		ImGui::SliderInt(T(TKEY("shadow_light_count"), "Shadow Light Count"), &settings.ShadowLightCount, 0, 127);
		// Compute projected VRAM for the slider's current value so the user
		// can see the cost of a higher count *before* committing the restart.
		// kSHADOWMAPS holds exactly ShadowLightCount slices -- the sun lives
		// in its own kSHADOWMAPS_ESRAM texture, so there's no +1.
		auto sliderVram = GetVRAMInfo();
		std::uint64_t projectedBytes = 0;
		std::uint64_t projectedFreeBytes = 0;
		bool projectionValid = sliderVram.valid;
		// With the atlas selected, the engine array stays at the vanilla
		// slice count after restart; the atlas texture carries the rest.
		const bool atlasSelected = settings.ShadowAtlas && settings.ShadowLightCount > 4;
		if (projectionValid) {
			const auto projectedSlices = atlasSelected ?
			                                 static_cast<std::uint32_t>(kVanillaShadowSliceCount) :
			                                 static_cast<std::uint32_t>(settings.ShadowLightCount);
			projectedBytes = ProjectShadowArrayBytes(projectedSlices);
			if (atlasSelected && sliderVram.shadowWidth && sliderVram.shadowHeight) {
				const std::uint64_t bpp = sliderVram.bytesPerSlice / (static_cast<std::uint64_t>(sliderVram.shadowWidth) * sliderVram.shadowHeight);
				projectedBytes += static_cast<std::uint64_t>(settings.AtlasResolution) * settings.AtlasResolution * bpp;
			}
			// Subtract the live atlas too, or its footprint is double-counted
			// against the projected array+atlas total.
			std::int64_t projectedUsage = static_cast<std::int64_t>(sliderVram.currentUsageBytes) -
			                              static_cast<std::int64_t>(sliderVram.shadowArrayBytes) -
			                              static_cast<std::int64_t>(AtlasVRAMBytes()) +
			                              static_cast<std::int64_t>(projectedBytes);
			if (projectedUsage < 0)
				projectedUsage = 0;
			projectedFreeBytes = (static_cast<std::int64_t>(sliderVram.budgetBytes) > projectedUsage) ? static_cast<std::uint64_t>(sliderVram.budgetBytes - projectedUsage) : 0;
		}
		if (ImGui::IsItemHovered()) {
			const char* kSliderBase =
				T(TKEY("shadow_light_count_tooltip"),
					"Maximum simultaneous shadow-casting point/spot lights (directional sun not counted).\n"
					"  0  = scheduler runs but selects no point lights (sun/directional unaffected).\n"
					"  4  = vanilla point light count with intelligent selection.\n"
					"  >4 = extended mode; depth buffer expanded when >8. Max 127.\n"
					"With the Shadow Atlas on, more lights cost scheduling and draw\n"
					"calls rather than video memory -- the atlas is a fixed size, and\n"
					"lights past its full-detail capacity drop to smaller tiles.\n"
					"Without it, each light adds a full shadow slice (watch the\n"
					"projected-VRAM bar).\n"
					"Requires a game restart to take effect.");
			if (projectionValid) {
				ImGui::SetTooltip(
					T(TKEY("shadow_light_count_projection_tooltip"),
						"%s\n"
						"\n"
						"Projected shadow VRAM at %d slots: %.1f MB (array + atlas)\n"
						"Per-slice cost: %.2f MB  (%u x %u, %u B/pixel)\n"
						"Projected free VRAM after restart: %.1f MB"),
					kSliderBase,
					settings.ShadowLightCount,
					static_cast<float>(projectedBytes) / (1024.f * 1024.f),
					static_cast<float>(sliderVram.bytesPerSlice) / (1024.f * 1024.f),
					sliderVram.shadowWidth, sliderVram.shadowHeight,
					sliderVram.shadowWidth && sliderVram.shadowHeight ?
						sliderVram.bytesPerSlice / (sliderVram.shadowWidth * sliderVram.shadowHeight) :
						0u,
					static_cast<float>(projectedFreeBytes) / (1024.f * 1024.f));
			} else {
				ImGui::SetTooltip("%s", kSliderBase);
			}
		}
		// Custom-drawn stacked bar against DXGI budget showing non-shadow /
		// current-shadow / projected-shadow segments. ImGui::ProgressBar
		// can't multi-segment.
		if (projectionValid && sliderVram.budgetBytes > 0) {
			const VRAMVerdict verdict = EvaluateVRAMVerdict(projectedBytes, projectedFreeBytes, sliderVram.budgetBytes);
			const float budgetMBf = static_cast<float>(sliderVram.budgetBytes) / (1024.f * 1024.f);
			// Current shadow storage = array + live atlas; keep the grey
			// non-shadow segment consistent with that split.
			const float atlasMB = static_cast<float>(AtlasVRAMBytes()) / (1024.f * 1024.f);
			const float nonShadowMB = std::max(0.0f,
				(static_cast<float>(sliderVram.currentUsageBytes) - static_cast<float>(sliderVram.shadowArrayBytes)) / (1024.f * 1024.f) - atlasMB);
			const float currentShadowMB = static_cast<float>(sliderVram.shadowArrayBytes) / (1024.f * 1024.f) + atlasMB;
			const float projectedShadowMB = static_cast<float>(projectedBytes) / (1024.f * 1024.f);

			ImGui::Text("%s", T(TKEY("projected_shadow_vram_label"), "Projected shadow VRAM :"));
			ImGui::SameLine();
			const ImVec2 cursor = ImGui::GetCursorScreenPos();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float barHeight = ImGui::GetFrameHeight();
			const float scale = fullWidth / budgetMBf;
			auto* draw = ImGui::GetWindowDrawList();
			// Background frame, then non-shadow / current / projected segments.
			draw->AddRectFilled(cursor, ImVec2(cursor.x + fullWidth, cursor.y + barHeight),
				ImGui::GetColorU32(ImGuiCol_FrameBg));
			const float nonShadowEndX = cursor.x + nonShadowMB * scale;
			draw->AddRectFilled(cursor, ImVec2(nonShadowEndX, cursor.y + barHeight),
				IM_COL32(120, 120, 120, 200));
			const float currentEndX = std::min(cursor.x + fullWidth, nonShadowEndX + currentShadowMB * scale);
			draw->AddRectFilled(ImVec2(nonShadowEndX, cursor.y),
				ImVec2(currentEndX, cursor.y + barHeight),
				IM_COL32(80, 130, 200, 220));
			// Projection outline anchored at the same start as current, so
			// the visual delta IS the difference. Solid fill for grow, dark
			// stripe for shrink.
			const float projectedEndX = std::min(cursor.x + fullWidth, nonShadowEndX + projectedShadowMB * scale);
			const ImU32 verdictColU32 = ImGui::GetColorU32(verdict.colour);
			draw->AddRect(ImVec2(nonShadowEndX, cursor.y), ImVec2(projectedEndX, cursor.y + barHeight),
				verdictColU32, 0.0f, 0, 2.0f);
			if (projectedShadowMB > currentShadowMB) {
				draw->AddRectFilled(ImVec2(currentEndX, cursor.y), ImVec2(projectedEndX, cursor.y + barHeight),
					(verdictColU32 & 0x00FFFFFFu) | 0xA0000000u);
			} else if (projectedShadowMB < currentShadowMB) {
				draw->AddRectFilled(ImVec2(projectedEndX, cursor.y), ImVec2(currentEndX, cursor.y + barHeight),
					IM_COL32(80, 80, 80, 120));
			}

			char overlay[128];
			snprintf(overlay, sizeof(overlay),
				T(TKEY("projected_shadow_vram_overlay"), "shadows %.0f -> %.0f MB  (%d slots,  %.0f MB free after restart)"),
				currentShadowMB, projectedShadowMB,
				settings.ShadowLightCount,
				static_cast<float>(projectedFreeBytes) / (1024.f * 1024.f));
			const ImVec2 textSize = ImGui::CalcTextSize(overlay);
			const ImVec2 textPos(cursor.x + (fullWidth - textSize.x) * 0.5f,
				cursor.y + (barHeight - textSize.y) * 0.5f);
			draw->AddText(textPos, IM_COL32(240, 240, 240, 255), overlay);
			ImGui::Dummy(ImVec2(fullWidth, barHeight));  // reserve layout space
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					T(TKEY("projected_shadow_vram_tooltip"),
						"Stacked VRAM bar against the graphics memory budget.\n"
						"  Grey block    : process VRAM not counted as shadow array\n"
						"  Blue block    : current kSHADOWMAPS allocation this session\n"
						"  Outlined block: what the slider's value would allocate\n"
						"                  after restart (colour reflects verdict)\n"
						"\n"
						"Solid colour past the blue: shadow array would GROW by that\n"
						"amount. Dark stripe inside the blue: shadow array would\n"
						"SHRINK by that amount.\n"
						"\n"
						"Slots requested  : %d (the sun uses its own shadow texture)\n"
						"Per-slice cost   : %.2f MB  (%u x %u @ %u B/pixel)\n"
						"Current array    : %.1f MB\n"
						"Projected array  : %.1f MB\n"
						"Free after restart : %.1f MB / %.0f MB budget\n"
						"%s"),
					settings.ShadowLightCount,
					static_cast<float>(sliderVram.bytesPerSlice) / (1024.f * 1024.f),
					sliderVram.shadowWidth, sliderVram.shadowHeight,
					sliderVram.shadowWidth && sliderVram.shadowHeight ?
						sliderVram.bytesPerSlice / (sliderVram.shadowWidth * sliderVram.shadowHeight) :
						0u,
					currentShadowMB,
					projectedShadowMB,
					static_cast<float>(projectedFreeBytes) / (1024.f * 1024.f),
					budgetMBf,
					verdict.over ?
						(atlasSelected ?
								T(TKEY("projected_vram_verdict_red_atlas"),
									"\nRED: this projection won't fit in the current VRAM budget.\n"
									"The driver will page or refuse the allocation and shadows\n"
									"will silently break. Lower Atlas Resolution -- with the\n"
									"atlas on, the light count no longer drives this.") :
								T(TKEY("projected_vram_verdict_red"),
									"\nRED: this projection won't fit in the current VRAM budget.\n"
									"The driver will page or refuse the allocation, leaving the\n"
									"shadow array smaller than requested -- shadows will silently\n"
									"break. Lower the slot count or the game's shadow resolution.")) :
					verdict.tight ?
						T(TKEY("projected_vram_verdict_yellow"),
							"\nYELLOW: tight headroom. A driver or OS spike could push\n"
							"shadow allocation into paging. Safe for testing, risky for\n"
							"long sessions or heavily-modded scenes.") :
						"");
			}
		}

		// ---- Allocation mismatch banner ----
		// Surface kSHADOWMAPS truncation visibly so users hit by a silent
		// "shadows don't work at high slot counts" failure can see why.
		// Reads the verified count directly (not the GetInstalledSlotCount
		// accessor, which falls back to the requested value).
		{
			uint32_t installed = s_installedSlotCount;
			uint32_t requested = s_requestedSlotCount;
			if (installed > 0 && requested > 0 && installed < requested) {
				ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1),
					T(TKEY("vram_exhausted_banner"), "VRAM exhausted: requested %u slots, GPU allocated %u."),
					requested, installed);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						T(TKEY("vram_exhausted_tooltip"),
							"The engine tried to create kSHADOWMAPS with %u slices but\n"
							"the GPU / driver returned a smaller array (likely out of\n"
							"VRAM at the configured iShadowMapResolution). The scheduler\n"
							"has clamped itself to the actual count so the existing %u\n"
							"slices work correctly, but to reach the requested %u you'll\n"
							"need to free VRAM (lower resolution, other features, etc)."),
						requested, installed, requested);
			} else if (installed == 0 && s_settings.Enabled && !s_externalConflict) {
				ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.25f, 1),
					"%s", T(TKEY("shadow_array_unverified_banner"), "Shadow array not yet verified -- load a save to confirm allocation."));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("shadow_array_unverified_tooltip"),
												"kSHADOWMAPS isn't readable yet (main menu / loading screen).\n"
												"Once you reach gameplay the scheduler verifies the actual\n"
												"slice count against your requested value. If they disagree\n"
												"this banner turns red."));
			}
		}

		if (settings.ShadowLightCount != s_installedShadowLightCount) {
			const auto& theme = Menu::GetSingleton()->GetTheme();
			ImGui::TextColored(theme.StatusPalette.RestartNeeded,
				T(TKEY("restart_session_lights"), "Restart required -- current session uses %d lights."), s_installedShadowLightCount);
		}

		// ---- Shadow Map Resolution (requires restart) ---------------------
		// Mirrors the launcher's resolution tiers (the four power-of-two values
		// Skyrim itself offers). Mutates the live iShadowMapResolution:Display
		// RE::Setting immediately; persistence to SkyrimPrefs.ini happens in
		// SCM::SaveINISettings (called from LightLimitFix::SaveSettings).
		if (auto* prefColl = RE::INIPrefSettingCollection::GetSingleton()) {
			if (auto* setting = prefColl->GetSetting("iShadowMapResolution:Display")) {
				static constexpr struct
				{
					const char* key;
					const char* label;
					std::int32_t value;
				} kResTiers[] = {
					{ TKEY("res_tier_low"), "Low (1024)", 1024 },
					{ TKEY("res_tier_medium"), "Medium (2048)", 2048 },
					{ TKEY("res_tier_high"), "High (4096)", 4096 },
					{ TKEY("res_tier_ultra"), "Ultra (8192)", 8192 },
				};
				constexpr int kTierCount = static_cast<int>(sizeof(kResTiers) / sizeof(kResTiers[0]));

				const std::int32_t currentRes = setting->GetInteger();
				int tierIdx = -1;
				for (int i = 0; i < kTierCount; ++i) {
					if (kResTiers[i].value == currentRes) {
						tierIdx = i;
						break;
					}
				}
				// Non-tier values (manual INI edits / third-party tools)
				// surface as "Custom (N)" so the user sees what the engine is
				// actually using, but we don't offer it as a selectable tier.
				char previewBuf[32];
				const char* preview;
				if (tierIdx >= 0) {
					preview = T(kResTiers[tierIdx].key, kResTiers[tierIdx].label);
				} else {
					snprintf(previewBuf, sizeof(previewBuf), T(TKEY("res_tier_custom"), "Custom (%d)"), currentRes);
					preview = previewBuf;
				}

				if (ImGui::BeginCombo(T(TKEY("shadow_map_resolution"), "Shadow Map Resolution"), preview)) {
					for (int i = 0; i < kTierCount; ++i) {
						const bool selected = (i == tierIdx);
						if (ImGui::Selectable(T(kResTiers[i].key, kResTiers[i].label), selected) &&
							kResTiers[i].value != currentRes) {
							setting->SetInteger(kResTiers[i].value);
							s_shadowResolutionDirty = true;
						}
						if (selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", T(TKEY("shadow_map_resolution_tooltip"),
												"Drives iShadowMapResolution:Display in SkyrimPrefs.ini.\n"
												"Affects both omni/spot shadow slices and the sun cascade\n"
												"texture; per-slice VRAM scales as resolution^2 * 4 bytes\n"
												"(4 / 16 / 64 / 256 MB at 1024 / 2048 / 4096 / 8192).\n"
												"Requires a game restart to take effect."));
				}

				if (s_initialShadowMapResolution > 0 && currentRes != s_initialShadowMapResolution) {
					const auto& theme = Menu::GetSingleton()->GetTheme();
					ImGui::TextColored(theme.StatusPalette.RestartNeeded,
						T(TKEY("restart_session_resolution"), "Restart required -- current session uses %d px shadow maps."),
						s_initialShadowMapResolution);
				}
			}
		}

		// ---- Temporal budget (dynamic) ------------------------------------

		// Migrate legacy Auto budget mode to Manual.
		if (settings.BudgetMode == BudgetModeEnum::Auto)
			settings.BudgetMode = BudgetModeEnum::Manual;

		// Budget mode selector (Manual or Formula).

		const char* budgetModeNames[] = { T(TKEY("budget_mode_manual"), "Manual"), T(TKEY("budget_mode_formula"), "Formula") };
		int budgetModeIdx = (settings.BudgetMode == BudgetModeEnum::Manual) ? 0 : 1;
		if (ImGui::Combo(T(TKEY("budget_mode"), "Budget Mode"), &budgetModeIdx, budgetModeNames, 2))
			settings.BudgetMode = (budgetModeIdx == 0) ? BudgetModeEnum::Manual : BudgetModeEnum::Formula;
		if (ImGui::IsItemHovered()) {
			if (budgetModeIdx == 0)
				ImGui::SetTooltip("%s", T(TKEY("budget_mode_manual_tooltip"),
											"Manual (default): fixed per-frame GPU time budget for shadow re-renders.\n"
											"Predictable; doesn't oscillate. Adjust the slider to trade FPS for shadow quality."));
			else
				ImGui::SetTooltip("%s", T(TKEY("budget_mode_formula_tooltip"),
											"Formula: user-editable math expression for per-frame budget.\n"
											"Default expression matches Intellightent's original behaviour\n"
											"(1 ms outdoors, 2 ms indoors). Edit the expression in the\n"
											"Advanced section below.\n"
											"\n"
											"Caveat: adaptive expressions referencing `frametime` tend to\n"
											"ping-pong because rendering shadows raises frametime, removing\n"
											"the headroom that allowed the budget. Stick to static or\n"
											"slowly-varying inputs (`isinterior`, `frametarget`)."));
		}

		// Per-mode controls.
		if (budgetModeIdx == 0) {
			ImGui::SliderFloat(T(TKEY("redraw_budget_ms"), "Redraw Budget (ms)"), &settings.RedrawBudgetMs, 0.1f, 32.0f, "%.2f ms");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("redraw_budget_ms_tooltip"),
											"Per-frame GPU time budget for shadow re-renders (milliseconds).\n"
											"Lights whose estimated render cost exceeds the remaining budget are deferred.\n"
											"The first eligible light always renders regardless of budget (starvation prevention).\n"
											"\n"
											"Reference points:\n"
											"  1-2 ms: Intellightent's original (1 outdoors, 2 indoors)\n"
											"  5 ms : default, comfortable for typical scenes (~5-8 lights at ~1 ms each)\n"
											"  16 ms: full 60 fps frame; shadows can saturate the frame here\n"
											"  32 ms: extreme, only useful for very high light counts on fast GPUs\n"
											"\n"
											"Higher = more shadow lights redraw per frame, fewer stale shadow maps,\n"
											"at the cost of frametime. The Budget verdict in the Active Casters\n"
											"section shows whether the current setting has headroom to spare."));
		} else {
			ImGui::Text(T(TKEY("budget_from_formula"), "Budget from formula: %.2f ms"), s_autoBudgetMs);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("budget_from_formula_tooltip"), "Edit the Redraw Budget formula in the Advanced section below."));
		}

		// Budget consumption visualisation lives in the Active Casters block
		// (DrawShadowSchedulerStats) alongside the verdict, so the bar, the
		// numeric reading and the actionable state appear in one place
		// instead of being split between two sections.

		// ---- Frame-target diagnostic (Formula mode only) ------------------
		// `frametarget` is an exprtk variable available to the Redraw Budget
		// formula -- in Formula mode the user needs to see what it evaluates
		// to in order to write/debug expressions that reference it. In Manual
		// mode the user's chosen RedrawBudgetMs has nothing to do with frame
		// timing, so this block would just be noise -- the new Budget verdict
		// (in the Active Casters block) covers the "headroom / saturated"
		// signal more actionably for both modes, and DrawShadowSummary covers
		// the rendered/dropped lights count without duplication.
		if (settings.BudgetMode == BudgetModeEnum::Formula) {
			const float currentFrameMs = *globals::game::deltaTime * 1000.0f;
			const float currentFPS = 1000.0f / std::max(currentFrameMs, 1.0f);
			const float targetMs = ComputeFrameTimePercentile90();
			const float targetFPS = targetMs > 0.0f ? 1000.0f / targetMs : 0.0f;
			const float rawHeadroom = targetMs - s_ftEMA;
			const float headroomMs = rawHeadroom - kFrameHeadroomSafetyMs;

			const char* state = T(TKEY("frame_state_steady"), "steady");
			if (rawHeadroom > kFrameHeadroomSafetyMs + kFrameHeadroomDeadZoneMs)
				state = T(TKEY("frame_state_growing"), "growing");
			else if (rawHeadroom < -kFrameHeadroomDeadZoneMs)
				state = T(TKEY("frame_state_throttling"), "throttling");

			ImGui::Text(T(TKEY("frame_diagnostic"), "Frame: %.1f FPS (%.1f ms) | frametarget: %.0f FPS (%.1f ms) | headroom: %+.1f ms | %s"),
				currentFPS, currentFrameMs, targetFPS, targetMs, headroomMs, state);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					T(TKEY("frame_diagnostic_tooltip"),
						"Live values of the formula variables exposed to the Redraw\n"
						"Budget formula. `frametarget` is the rolling 90th-percentile\n"
						"frame time, used as a self-measured ceiling -- not a vsync\n"
						"target. State indicator:\n"
						"  steady     -- within +/-%.1f ms of target\n"
						"  growing    -- frametime well below target; headroom available\n"
						"  throttling -- frametime over target; expressions returning\n"
						"                 nonzero values here will keep frametime high"),
					kFrameHeadroomDeadZoneMs);
		}
		{
			// Slider bound only, never clamp the stored setting: doing so wrote
			// MaxRedrawPerFrame to 1 permanently on first DrawSettings, before
			// the scheduler hook had run. Bound on the configured light count,
			// not the per-frame active count, so the slider range doesn't jitter.
			int maxRedraws = std::max(settings.ShadowLightCount, Settings::kMinMaxRedrawPerFrame);
			ImGui::SliderInt(T(TKEY("max_redraws_per_frame"), "Max Redraws Per Frame"), &settings.MaxRedrawPerFrame,
				Settings::kMinMaxRedrawPerFrame, maxRedraws);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					T(TKEY("max_redraws_per_frame_tooltip"),
						"Hard cap on how many shadow lights may re-render their shadow maps in one frame.\n"
						"Acts as a safety valve regardless of budget -- the budget controls time spent,\n"
						"this controls count. The sun directional light always counts as one redraw.\n"
						"Minimum is %d (lower values cause shadow flicker as redraw rotation outpaces TAA).\n"
						"Upper bound matches the Shadow Light Count setting (%d)."),
					Settings::kMinMaxRedrawPerFrame, maxRedraws);
		}

		// ---- Light conversion (requires restart for hooks) -----------------
		if (ImGui::TreeNode(T(TKEY("light_conversion"), "Light Conversion##LightConv"))) {
			ImGui::Checkbox(T(TKEY("convert_excess_to_normal"), "Convert Excess Lights to Normal"), &settings.ConvertExcessToNormal);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("convert_excess_to_normal_tooltip"),
											"Shadow lights that exceed the active shadow caster limit are demoted to\n"
											"normal (unshadowed) lights so they still contribute diffuse and specular\n"
											"lighting at no shadow-map cost. Lights that fail culling are dropped entirely.\n"
											"Requires a game restart to change."));
			Util::UI::DrawSettingDiff(s_bootSnapshot, settings, &Settings::ConvertExcessToNormal);

			// No texture-array cost -- converted lights flow through the cluster
			// pipeline as ordinary non-shadow lights. Match the ShadowLightCount
			// max so users can pair a large shadow pool with a matching converted
			// pool without the slider lying about the upper bound.
			ImGui::SliderInt(T(TKEY("converted_shadow_slots"), "Converted Shadow Slots"), &settings.ConvertedShadowSlots, 0, 127);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("converted_shadow_slots_tooltip"),
											"Extra pool slots for lights converted to normal (unshadowed) mode.\n"
											"Increase if Convert Excess Lights drops lights you expect to see."));

			ImGui::Checkbox(T(TKEY("promote_normal_to_shadow"), "Promote Normal Lights to Shadow Casters"), &settings.PromoteNormalToShadow);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("promote_normal_to_shadow_tooltip"),
											"Experimental: elevate high-scoring unshadowed lights to shadow casters\n"
											"when shadow slots are available.\n"
											"Requires a game restart to change."));
			Util::UI::DrawSettingDiff(s_bootSnapshot, settings, &Settings::PromoteNormalToShadow);

			ImGui::SeparatorText(T(TKEY("portal_strict_enforcement"), "Portal-Strict Enforcement"));
			// Three-way toggle plus master row. SCM forces the engine's
			// portal-strict flag on shadow casters at creation time, gated
			// per shadow type (FOV-derived). Defaults enforce on omni and
			// hemisphere, leave spotlights alone -- portal-strict on spots
			// drops culled-but-visible spots entirely (cone test rejects
			// spots whose origin is behind a portal even when the beam
			// sweeps into a visible room).
			{
				const bool allOn = settings.ForceEnablePortalStrictOmni &&
				                   settings.ForceEnablePortalStrictHemi &&
				                   settings.ForceEnablePortalStrictSpot;
				const bool allOff = !settings.ForceEnablePortalStrictOmni &&
				                    !settings.ForceEnablePortalStrictHemi &&
				                    !settings.ForceEnablePortalStrictSpot;
				bool master = allOn;
				bool indeterminate = !allOn && !allOff;
				if (indeterminate) {
					// Render the master checkbox as visually mixed via a
					// muted alpha so the row still functions as a "set all"
					// control without misrepresenting state.
					ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);
				}
				if (ImGui::Checkbox(T(TKEY("force_portal_strict_all"), "Force Enable Portal Strict (All)"), &master)) {
					settings.ForceEnablePortalStrictOmni = master;
					settings.ForceEnablePortalStrictHemi = master;
					settings.ForceEnablePortalStrictSpot = master;
				}
				if (indeterminate)
					ImGui::PopStyleVar();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("force_portal_strict_all_tooltip"),
												"Master toggle for the three per-type rows below.\n"
												"Checked when all three are enforced, unchecked when none are,\n"
												"and rendered translucent when mixed.\n"
												"Requires a game restart to change."));
			}

			ImGui::Indent();
			ImGui::Checkbox(T(TKEY("force_portal_strict_omni"), "Force Portal Strict on Omni Lights"), &settings.ForceEnablePortalStrictOmni);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("force_portal_strict_omni_tooltip"),
											"Force room-visibility (portal-strict) culling on omnidirectional\n"
											"(dual dome projection, aka dual-paraboloid) shadow casters.\n"
											"Recommended on -- tightens room visibility culling for full-sphere\n"
											"shadow lights without side effects.\n"
											"Requires a game restart to change."));
			Util::UI::DrawSettingDiff(s_bootSnapshot, settings, &Settings::ForceEnablePortalStrictOmni);
			ImGui::Checkbox(T(TKEY("force_portal_strict_hemi"), "Force Portal Strict on Hemisphere Lights"), &settings.ForceEnablePortalStrictHemi);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("force_portal_strict_hemi_tooltip"),
											"Force room-visibility (portal-strict) culling on hemisphere\n"
											"(single dome projection) shadow casters. Recommended on --\n"
											"behaves like the omni case under portal culling.\n"
											"Requires a game restart to change."));
			Util::UI::DrawSettingDiff(s_bootSnapshot, settings, &Settings::ForceEnablePortalStrictHemi);
			ImGui::Checkbox(T(TKEY("force_portal_strict_spot"), "Force Portal Strict on Spot Lights"), &settings.ForceEnablePortalStrictSpot);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("force_portal_strict_spot_tooltip"),
											"Force room-visibility (portal-strict) culling on perspective\n"
											"(frustum/spot) shadow casters. Off by default: the cone test\n"
											"rejects spots whose origin sits behind a portal even when their\n"
											"beam sweeps into a visible room, which drops culled-but-visible\n"
											"spots entirely.\n"
											"Enable only for debugging.\n"
											"Requires a game restart to change."));
			Util::UI::DrawSettingDiff(s_bootSnapshot, settings, &Settings::ForceEnablePortalStrictSpot);
			ImGui::Unindent();

			ImGui::TreePop();
		}

		// ---- Advanced (dynamic) -------------------------------------------
		if (ImGui::TreeNode(T(TKEY("advanced"), "Advanced##ShadowScheduling"))) {
			ImGui::Checkbox(T(TKEY("allow_immediate_draw_new_lights"), "Allow Immediate Draw for New Lights"), &settings.AllowDrawNewLight);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("allow_immediate_draw_new_lights_tooltip"),
											"Allow a light just added to the active pool to render its shadow map this frame.\n"
											"Prevents a one-frame shadow-map gap when new lights enter view."));

			// Atlas is boot-latched (the engine texture allocation depends on
			// it), so show restart state against the captured boot value.
			ImGui::BeginDisabled(s_installedShadowLightCount <= 4);
			ImGui::Checkbox(T(TKEY("shadow_atlas"), "Shadow Atlas (Restart Required)"), &settings.ShadowAtlas);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("shadow_atlas_tooltip"),
											"Store all extra shadow maps in one shared texture instead of one\n"
											"full-size map per light. Uses far less video memory with many\n"
											"shadow-casting lights. Takes effect after restarting the game."));
			if (settings.ShadowAtlas != s_bootAtlasEnabled)
				Util::Text::RestartNeeded("%s", T("common.restart_required", "Restart required"));
			if (settings.ShadowAtlas) {
				// Preview the STORED value (an out-of-set ini value must not
				// display as a preset it isn't); writes happen only on an
				// explicit selection.
				static constexpr uint32_t kAtlasResOptions[] = { 4096u, 8192u, kAtlasMaxResolution };
				char atlasResPreview[16];
				snprintf(atlasResPreview, sizeof(atlasResPreview), "%u", settings.AtlasResolution);
				if (ImGui::BeginCombo(T(TKEY("atlas_resolution"), "Atlas Resolution (Restart Required)"), atlasResPreview)) {
					for (uint32_t option : kAtlasResOptions) {
						char label[16];
						snprintf(label, sizeof(label), "%u", option);
						if (ImGui::Selectable(label, settings.AtlasResolution == option))
							settings.AtlasResolution = option;
					}
					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("atlas_resolution_tooltip"),
												"Size of the shared shadow texture. Bigger sizes keep more lights\n"
												"at full shadow detail but use more video memory. Takes effect\n"
												"after restarting the game."));
				// A full tile is one vanilla shadow map, so the atlas holds
				// (dim / tile)^2 of them; lights past that are the ones the
				// budget demotes. Surfacing it makes demotion legible instead
				// of looking arbitrary, and shows what raising either setting
				// actually trades.
				if (const uint32_t tile = AtlasBaseTile(); tile > 0) {
					const uint32_t perAxis = std::max(1u, AtlasDim() / tile);
					ImGui::Text(T(TKEY("atlas_full_capacity"), "  Fits ~%u lights at full %u px detail"),
						perAxis * perAxis, tile);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("atlas_full_capacity_tooltip"),
													"A full-detail tile is one vanilla shadow map, sized by the game's\n"
													"own shadow resolution setting. Lights beyond this count still cast\n"
													"shadows, at reduced detail, ordered by priority. Raise Atlas\n"
													"Resolution to keep more at full detail; raising the game's shadow\n"
													"resolution makes each tile sharper but fewer of them fit."));
				}
				// Compare through the same clamp+snap the atlas applies, or a
				// snapped dimension shows a restart banner no restart clears.
				if (AtlasActive() && AtlasSnapResolution(settings.AtlasResolution) != AtlasDim())
					Util::Text::RestartNeeded("%s", T("common.restart_required", "Restart required"));
			}
			ImGui::EndDisabled();

			ImGui::SeparatorText(T(TKEY("shadow_distance_header"), "Shadow Distance"));
			if (auto* prefColl = RE::INIPrefSettingCollection::GetSingleton()) {
				const bool wasMatching = settings.MatchShadowToLightFade;
				if (ImGui::Checkbox(T(TKEY("match_shadow_to_light_fade"), "Match Shadow Distance to Light Fade"), &settings.MatchShadowToLightFade) &&
					wasMatching && !settings.MatchShadowToLightFade)
					RefreshEngineShadowDistanceCache();  // toggled off: restore the manual distance now, don't wait for a reload
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", T(TKEY("match_shadow_to_light_fade_tooltip"),
												"Each frame, set the shadow-cull distance to the engine's light\n"
												"LOD fade-out distance, so a shadow exists exactly as long as its\n"
												"light is visible -- removes the on-approach pop without rendering\n"
												"shadows past where lights fade. Auto-adapts to interior-cell and\n"
												"weather overrides; overrides the sliders below while enabled."));

				// ---- Shadow Distance (live) -----------------------------------
				// Drives the engine's shadow-cull far plane. A light past this
				// distance is culled and demoted to a normal light, so its shadow
				// pops in as the player crosses the boundary. Raising it keeps
				// distant casters shadowed at the cost of more shadow renders.
				// Applies live; persisted on Save. Disabled while
				// MatchShadowToLightFade drives the distance for us.
				ImGui::BeginDisabled(settings.MatchShadowToLightFade);
				if (auto* iSetting = prefColl->GetSetting("fInteriorShadowDistance:Display")) {
					float v = iSetting->GetFloat();
					if (ImGui::SliderFloat(T(TKEY("interior_shadow_distance"), "Interior Shadow Distance"), &v, 1000.0f, 12000.0f, "%.0f")) {
						iSetting->SetFloat(v);
						s_shadowDistanceDirty = true;
						RefreshEngineShadowDistanceCache();  // apply live, no cell reload
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("interior_shadow_distance_tooltip"),
													"Distance (game units) past which interior light shadows are culled\n"
													"(fInteriorShadowDistance:Display, vanilla default 3000). Raise it so\n"
													"distant interior casters stay shadowed instead of popping in on\n"
													"approach -- costs more shadow renders. Applies live;\n"
													"persisted to SkyrimPrefs.ini on Save Settings."));
				}
				if (auto* eSetting = prefColl->GetSetting("fShadowDistance:Display")) {
					float v = eSetting->GetFloat();
					if (ImGui::SliderFloat(T(TKEY("exterior_shadow_distance"), "Exterior Shadow Distance"), &v, 2000.0f, 20000.0f, "%.0f")) {
						eSetting->SetFloat(v);
						s_shadowDistanceDirty = true;
						RefreshEngineShadowDistanceCache();  // apply live, no cell reload
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("exterior_shadow_distance_tooltip"),
													"Distance (game units) past which exterior shadows are culled\n"
													"(fShadowDistance:Display). Also drives the directional sun cascade\n"
													"range, so higher values soften distant outdoor shadow transitions\n"
													"at a GPU cost. Applies live; persisted on Save Settings."));
				}
				ImGui::EndDisabled();

				// Light fade distance -- not gated by the match toggle because it
				// drives the LIGHTS. With Match on it's the master "how far do lights
				// AND their shadows reach" control, since the coupling tracks it.
				if (auto* lodStart = GetDisplaySetting("fLightLODStartFade:Display")) {
					float v = lodStart->GetFloat();
					if (ImGui::SliderFloat(T(TKEY("light_fade_distance"), "Light Fade Distance"), &v, 1000.0f, 20000.0f, "%.0f")) {
						lodStart->SetFloat(v);
						// fLightLODMaxStartFade caps the start fade; lift it so the
						// slider isn't silently clamped (only raise, never lower).
						if (auto* lodMax = GetDisplaySetting("fLightLODMaxStartFade:Display"))
							if (lodMax->GetFloat() < v)
								lodMax->SetFloat(v);
						s_shadowDistanceDirty = true;
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("light_fade_distance_tooltip"),
													"Distance (game units) at which lights LOD-fade out\n"
													"(fLightLODStartFade; also lifts the fLightLODMaxStartFade cap).\n"
													"With 'Match Shadow Distance to Light Fade' on, this is the master\n"
													"control -- it sets how far BOTH lights and their shadows reach.\n"
													"Vanilla 3500. Global light-LOD setting: affects all lights, not\n"
													"just shadow casters. Persisted to SkyrimPrefs.ini on Save."));
				}
			}

			// ---- Importance scheduling curve ------------------------------
			ImGui::SeparatorText(T(TKEY("importance_scheduling"), "Importance Scheduling"));
			DrawImpactCullControls(settings);

			ImGui::SliderFloat(T(TKEY("max_interval_scale"), "Max Interval Scale"), &settings.ImportanceMaxScale, 0.5f, 5.0f, "%.2f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("max_interval_scale_tooltip"),
											"Interval multiplier for the LOWEST-priority lights.\n"
											"Higher values defer dim or distant lights more aggressively.\n"
											"Priority is the Score Formula's rank among active lights.\n"
											"Default: 2.0"));
			settings.ImportanceMaxScale = std::max(settings.ImportanceMaxScale, settings.ImportanceMinScale);

			ImGui::SliderFloat(T(TKEY("min_interval_scale"), "Min Interval Scale"), &settings.ImportanceMinScale, 0.01f, 1.0f, "%.3f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", T(TKEY("min_interval_scale_tooltip"),
											"Interval multiplier for the HIGHEST-priority lights.\n"
											"Lower values make top-ranked lights update shadows more often.\n"
											"The ratio Max/Min defines the scheduling dynamic range.\n"
											"Default: 0.05  (40x range at default Max=2.0)"));
			settings.ImportanceMinScale = std::min(settings.ImportanceMinScale, settings.ImportanceMaxScale);

			{
				float ratio = settings.ImportanceMaxScale / std::max(settings.ImportanceMinScale, 0.001f);
				ImGui::Text(T(TKEY("dynamic_range"), "Dynamic range: %.0fx  (unimportant lights wait %.0fx longer)"), ratio, ratio);
			}

			if (ImGui::Button(T(TKEY("reset_importance_defaults"), "Reset Importance Defaults"))) {
				settings.ImportanceMinScale = 0.05f;
				settings.ImportanceMaxScale = 2.0f;
			}

			// ---- Formula editor ------------------------------------------
			if (ImGui::TreeNode(T(TKEY("formula_editor"), "Formula Editor##Formulas"))) {
				// Build variable reference from the DRY table.
				if (ImGui::TreeNode(T(TKEY("available_variables"), "Available Variables##FormulaVars"))) {
					if (ImGui::BeginTable("##FormulaVarTable", 2,
							ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
								ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
							ImVec2(0, std::min(static_cast<float>(IM_ARRAYSIZE(kFormulaVars)) * 20.0f + 28.0f, 320.0f)))) {
						ImGui::TableSetupColumn(T(TKEY("col_variable"), "Variable"));
						ImGui::TableSetupColumn(T(TKEY("col_description"), "Description"));
						ImGui::TableHeadersRow();
						for (const auto& v : kFormulaVars) {
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextUnformatted(v.name);
							ImGui::TableSetColumnIndex(1);
							ImGui::TextUnformatted(v.description);
						}
						ImGui::EndTable();
					}
					ImGui::TreePop();
				}

				static char scoreBuf[512];
				static char scoreErr[256] = {};
				static char redrawIntervalBuf[512];
				static char redrawIntervalErr[256] = {};
				static char redrawBudgetBuf[512];
				static char redrawBudgetErr[256] = {};
				static bool formulaBufsInited = false;
				if (!formulaBufsInited) {
					snprintf(scoreBuf, sizeof(scoreBuf), "%s", settings.ScoreFormula.c_str());
					snprintf(redrawIntervalBuf, sizeof(redrawIntervalBuf), "%s", settings.RedrawIntervalFormula.c_str());
					snprintf(redrawBudgetBuf, sizeof(redrawBudgetBuf), "%s", settings.RedrawBudgetFormula.c_str());
					formulaBufsInited = true;
				}

				// Shipped defaults for the per-field Reset buttons, sourced from
				// the Settings member initializers so they can't drift.
				static const Settings kFormulaDefaults{};

				// Helper lambda: validate, apply live, revert buffer on error.
				auto applyFormula = [](const char* label, char* buf, size_t bufSize,
										std::string& settingStr, char* errBuf, size_t errBufSize,
										std::unique_ptr<FormulaHelper>& helper,
										const char* tooltip, const std::string& defaultStr) {
					auto reparse = [&]() {
						if (helper)
							helper->Reparse(settingStr);
						else {
							helper = std::make_unique<FormulaHelper>();
							helper->Parse(settingStr);
						}
					};
					ImGui::InputText(label, buf, bufSize);
					if (ImGui::IsItemDeactivatedAfterEdit()) {
						std::string err;
						if (FormulaHelper::Validate(buf, err)) {
							settingStr = buf;
							errBuf[0] = '\0';
							reparse();
						} else {
							snprintf(errBuf, errBufSize, T(TKEY("parse_error"), "Parse error: %s"), err.c_str());
							snprintf(buf, bufSize, "%s", settingStr.c_str());
						}
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", tooltip);
					ImGui::SameLine();
					ImGui::PushID(label);
					if (ImGui::SmallButton(T(TKEY("formula_reset"), "Reset")) && settingStr != defaultStr) {
						settingStr = defaultStr;
						snprintf(buf, bufSize, "%s", settingStr.c_str());
						errBuf[0] = '\0';
						reparse();
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", T(TKEY("formula_reset_tip"), "Restore the default expression."));
					ImGui::PopID();
					if (errBuf[0])
						ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", errBuf);
				};

				applyFormula(T(TKEY("formula_score"), "Score"), scoreBuf, sizeof(scoreBuf),
					settings.ScoreFormula, scoreErr, sizeof(scoreErr), s_formulaScore,
					T(TKEY("formula_score_tooltip"), "Light priority scoring formula. Higher score = more likely to get a shadow slot."),
					kFormulaDefaults.ScoreFormula);

				applyFormula(T(TKEY("formula_redraw_interval"), "Redraw Interval"), redrawIntervalBuf, sizeof(redrawIntervalBuf),
					settings.RedrawIntervalFormula, redrawIntervalErr, sizeof(redrawIntervalErr), s_formulaRedrawInterval,
					T(TKEY("formula_redraw_interval_tooltip"), "Per-light redraw interval formula. Higher = less frequent shadow map updates."),
					kFormulaDefaults.RedrawIntervalFormula);
				applyFormula(T(TKEY("formula_redraw_budget"), "Redraw Budget"), redrawBudgetBuf, sizeof(redrawBudgetBuf),
					settings.RedrawBudgetFormula, redrawBudgetErr, sizeof(redrawBudgetErr), s_formulaRedrawBudget,
					T(TKEY("formula_redraw_budget_tooltip"), "Per-frame redraw budget formula (ms). Empty = use the Redraw Budget (ms) slider value."),
					kFormulaDefaults.RedrawBudgetFormula);

				ImGui::TreePop();
			}

			ImGui::TreePop();
		}

		// Active casters table + scheduler stats are rendered by LightLimitFix
		// alongside its own quick-stats line, so the table area has full
		// testing context (cluster light count, shadow slot usage, etc.) in
		// one place. See LightLimitFix::DrawSettings.

		if (!settings.Enabled)
			ImGui::EndDisabled();

		if (s_externalConflict)
			ImGui::EndDisabled();
	}
}

#undef I18N_KEY_PREFIX

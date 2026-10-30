#include "ImGuiSurfaceSync.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>

namespace
{
	bool HasAnyMouseDown(const ImGuiIO& io) noexcept
	{
		for (bool down : io.MouseDown) {
			if (down)
				return true;
		}
		return false;
	}

	bool HasAnyMouseClick(const ImGuiIO& io) noexcept
	{
		for (bool clicked : io.MouseClicked) {
			if (clicked)
				return true;
		}
		return false;
	}

	bool DidContextStartInteraction(const ImGuiContext& context) noexcept
	{
		return context.ActiveIdIsJustActivated || HasAnyMouseClick(context.IO) || context.NavActivatePressedId != 0;
	}

	bool WasItemActivatedThisFrame(const ImGuiContext& context, ImGuiID id) noexcept
	{
		if (id == 0)
			return false;
		const std::array<ImGuiID, 5> activatedIds{
			context.ActiveId,
			context.ActiveIdPreviousFrame,
			context.NavActivateId,
			context.NavActivateDownId,
			context.NavActivatePressedId
		};
		return std::ranges::find(activatedIds, id) != activatedIds.end();
	}

	std::vector<ImGuiID> CollectOpenPopups(const ImGuiContext& context)
	{
		std::vector<ImGuiID> popups;
		popups.reserve(static_cast<std::size_t>(context.OpenPopupStack.Size));
		for (const ImGuiPopupData& popup : context.OpenPopupStack) {
			if (popup.PopupId != 0)
				popups.push_back(popup.PopupId);
		}
		return popups;
	}
}

std::uint64_t ImGuiSurfaceSync::MakeTreeKey(ImGuiID windowId, ImGuiID storageId) noexcept
{
	return (static_cast<std::uint64_t>(windowId) << 32) | static_cast<std::uint64_t>(storageId);
}

void ImGuiSurfaceSync::ApplyPendingWidgets(ImGuiContext* target, ContextSyncState& targetState)
{
	if (!target)
		return;

	ScopedImGuiContext guard(target);

	for (auto it = targetState.pendingTabs.begin(); it != targetState.pendingTabs.end();) {
		if (ImGuiTabBar* bar = target->TabBars.GetByKey(it->first)) {
			bar->NextSelectedTabId = it->second;
			targetState.tabSelections[it->first] = it->second;
			it = targetState.pendingTabs.erase(it);
		} else {
			++it;
		}
	}

	for (auto it = targetState.pendingTrees.begin(); it != targetState.pendingTrees.end();) {
		if (ImGuiWindow* window = ImGui::FindWindowByID(it->second.windowId)) {
			window->StateStorage.SetInt(it->second.storageId, it->second.value);
			targetState.treeStates[it->first] = it->second.value;
			it = targetState.pendingTrees.erase(it);
		} else {
			++it;
		}
	}
}

void ImGuiSurfaceSync::ApplyBeforeNewFrame(ImGuiContext* target)
{
	if (!target || target->WithinFrameScope)
		return;

	auto& targetState = m_contextState[target];
	if (!targetState.seeded) {
		for (const auto& [otherContext, otherState] : m_contextState) {
			if (otherContext == target || !otherState.seeded)
				continue;
			for (const auto& [barId, tabId] : otherState.tabSelections)
				targetState.pendingTabs.try_emplace(barId, tabId);
			break;
		}
		targetState.seeded = true;
	}

	if (targetState.clearIdleActiveId) {
		targetState.clearIdleActiveId = false;
		if (target->ActiveId != 0 && !HasAnyMouseDown(target->IO)) {
			ScopedImGuiContext guard(target);
			ImGui::ClearActiveID();
		}
	}

	ApplyPendingWidgets(target, targetState);
}

void ImGuiSurfaceSync::OnFrameCompleted(ImGuiContext* source, ImGuiContext* target)
{
	if (!source)
		return;

	auto& sourceState = m_contextState[source];
	sourceState.seeded = true;
	ContextSyncState* targetState = target ? &m_contextState[target] : nullptr;

	if (targetState && DidContextStartInteraction(*source))
		targetState->clearIdleActiveId = true;

	for (int i = 0; i < source->TabBars.GetMapSize(); ++i) {
		const ImGuiTabBar* bar = source->TabBars.TryGetMapData(i);
		if (!bar || bar->ID == 0 || bar->SelectedTabId == 0 || bar->CurrFrameVisible != source->FrameCount)
			continue;

		const auto prevIt = sourceState.tabSelections.find(bar->ID);
		const bool hadPrev = prevIt != sourceState.tabSelections.end();
		const bool changed = hadPrev && prevIt->second != bar->SelectedTabId;
		const bool blockedByPending = !hadPrev && sourceState.pendingTabs.contains(bar->ID);
		if (!blockedByPending)
			sourceState.tabSelections[bar->ID] = bar->SelectedTabId;

		if (!targetState)
			continue;

		if (changed) {
			targetState->pendingTabs[bar->ID] = bar->SelectedTabId;
			sourceState.pendingTabs.erase(bar->ID);
		} else if (!hadPrev && !blockedByPending) {
			const auto targetIt = targetState->tabSelections.find(bar->ID);
			if (targetIt != targetState->tabSelections.end() && targetIt->second != bar->SelectedTabId) {
				sourceState.pendingTabs[bar->ID] = targetIt->second;
			} else if (targetIt == targetState->tabSelections.end() && !targetState->pendingTabs.contains(bar->ID)) {
				targetState->pendingTabs[bar->ID] = bar->SelectedTabId;
			}
		}
	}

	for (ImGuiWindow* window : source->Windows) {
		if (!window || (!window->Active && !window->WasActive) || window->ID == 0)
			continue;

		for (const ImGuiStoragePair& entry : window->StateStorage.Data) {
			if (entry.key == 0 || (entry.val_i != 0 && entry.val_i != 1))
				continue;

			const std::uint64_t treeKey = MakeTreeKey(window->ID, entry.key);
			const auto prevIt = sourceState.treeStates.find(treeKey);
			const bool hadPrev = prevIt != sourceState.treeStates.end();
			const bool changed = hadPrev && prevIt->second != entry.val_i;
			const bool blockedByPending = !hadPrev && sourceState.pendingTrees.contains(treeKey);
			if (!blockedByPending)
				sourceState.treeStates[treeKey] = entry.val_i;

			if (!targetState)
				continue;

			if (changed) {
				targetState->pendingTrees[treeKey] = PendingTreeToggle{ window->ID, entry.key, entry.val_i };
				sourceState.pendingTrees.erase(treeKey);
			} else if (!hadPrev && !blockedByPending) {
				const auto targetIt = targetState->treeStates.find(treeKey);
				if (targetIt == targetState->treeStates.end()) {
					if (!targetState->pendingTrees.contains(treeKey))
						targetState->pendingTrees[treeKey] = PendingTreeToggle{ window->ID, entry.key, entry.val_i };
				} else if (targetIt->second != entry.val_i) {
					if (WasItemActivatedThisFrame(*source, entry.key) || DidContextStartInteraction(*source))
						targetState->pendingTrees[treeKey] = PendingTreeToggle{ window->ID, entry.key, entry.val_i };
					else
						sourceState.pendingTrees[treeKey] = PendingTreeToggle{ window->ID, entry.key, targetIt->second };
				}
			}
		}
	}

	std::vector<ImGuiID> currentPopups = CollectOpenPopups(*source);
	if (target && targetState && !target->WithinFrameScope) {
		for (ImGuiID oldPopupId : sourceState.openPopups) {
			if (std::ranges::find(currentPopups, oldPopupId) != currentPopups.end())
				continue;
			for (int i = 0; i < target->OpenPopupStack.Size; ++i) {
				if (target->OpenPopupStack[i].PopupId == oldPopupId) {
					ScopedImGuiContext guard(target);
					ImGui::ClosePopupToLevel(i, true);
					targetState->openPopups = CollectOpenPopups(*target);
					break;
				}
			}
		}
	}
	sourceState.openPopups = std::move(currentPopups);

	if (target && targetState && !target->WithinFrameScope)
		ApplyPendingWidgets(target, *targetState);
}

void ImGuiSurfaceSync::ForgetContext(ImGuiContext* context)
{
	if (context)
		m_contextState.erase(context);
}

void ImGuiSurfaceSync::Reset()
{
	m_contextState.clear();
}

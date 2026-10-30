#pragma once

#include <imgui.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ImGuiContext;

/** RAII guard that binds an ImGuiContext for the current scope and restores the prior context. */
class ScopedImGuiContext
{
public:
	explicit ScopedImGuiContext(ImGuiContext* target) noexcept :
		m_previous(ImGui::GetCurrentContext())
	{
		if (target && target != m_previous)
			ImGui::SetCurrentContext(target);
	}

	~ScopedImGuiContext()
	{
		if (ImGui::GetCurrentContext() != m_previous)
			ImGui::SetCurrentContext(m_previous);
	}

	ScopedImGuiContext(const ScopedImGuiContext&) = delete;
	ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
	ImGuiContext* m_previous = nullptr;
};

/** Synchronizes interactive widget state across independent desktop and VR HMD ImGui contexts. */
class ImGuiSurfaceSync
{
public:
	/** Applies queued tab, tree-node, and focus hand-off updates to @p target before ImGui::NewFrame(). */
	void ApplyBeforeNewFrame(ImGuiContext* target);

	/** Captures user-driven widget transitions from @p source after ImGui::Render() and queues them for @p target. */
	void OnFrameCompleted(ImGuiContext* source, ImGuiContext* target);

	/** Removes cached synchronization state for a destroyed context. */
	void ForgetContext(ImGuiContext* context);

	/** Clears all tracked synchronization state across all contexts. */
	void Reset();

private:
	struct PendingTreeToggle
	{
		ImGuiID windowId = 0;
		ImGuiID storageId = 0;
		int value = 0;
	};

	struct ContextSyncState
	{
		bool seeded = false;
		bool clearIdleActiveId = false;
		std::unordered_map<ImGuiID, ImGuiID> tabSelections;
		std::unordered_map<ImGuiID, ImGuiID> pendingTabs;
		std::unordered_map<std::uint64_t, int> treeStates;
		std::unordered_map<std::uint64_t, PendingTreeToggle> pendingTrees;
		std::vector<ImGuiID> openPopups;
	};

	static std::uint64_t MakeTreeKey(ImGuiID windowId, ImGuiID storageId) noexcept;
	void ApplyPendingWidgets(ImGuiContext* target, ContextSyncState& targetState);

	std::unordered_map<ImGuiContext*, ContextSyncState> m_contextState;
};

#pragma once

// Decisions for the ImGuiVRHelper combos the VR menu registers: which action, if any, a chord may do.
// Free of helper and engine types so the decision is unit-testable.
namespace VRHelperCombo
{
	/** @brief The combo edges the helper reported this frame. */
	struct Edges
	{
		bool open = false;
		bool close = false;
	};

	enum class Action
	{
		None,
		Show,
		Hide
	};

	/**
	 * @brief The performance-overlay change the open/close combos may make this frame.
	 * @param a_focused True when the helper routed menu focus to this client.
	 * @param a_pointerInPanel True when the wand is on this client's panel this frame.
	 *
	 * A chord fires wherever the wand points and the helper's raycast is global, so pointer state is
	 * only meaningful for the focused client.
	 */
	[[nodiscard]] inline Action DecideOverlayToggle(bool a_focused, bool a_pointerInPanel, Edges a_edges)
	{
		if (!a_focused || !a_pointerInPanel)
			return Action::None;
		if (a_edges.open)
			return Action::Show;
		if (a_edges.close)
			return Action::Hide;
		return Action::None;
	}
}

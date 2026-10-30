#pragma once

#include <functional>
#include <string_view>

#include "SceneSettingsManager.h"
#include "Utils/SettingsControl.h"

struct Feature;

/// Intercepts ImGui widget calls so a Scene Manager replica of a feature's DrawSettings can bind
/// each control to a scene entry. Detours are installed once and stay inert until a Scope is alive.
namespace SceneWidgetInterceptor
{
	/// What the armed replica is editing. Copied into the interceptor for the scope's lifetime.
	struct Context
	{
		Feature* feature = nullptr;
		SceneSettingsManager::SceneContextId contextId;
		/// The main menu's own DrawSettings: controls edit the baseline and show which layer wins.
		bool baseline = false;
	};

	/** @brief Installs the detours. Idempotent; call from the render thread before the first frame. */
	bool Install();

	/** @brief Whether Install attached every detour. */
	bool IsInstalled();

	/// True while a non-baseline Scope replicates a feature's DrawSettings for scene authoring, so
	/// features can hide UI with no scene meaning (debug views, buffer viewers).
	bool IsArmed();

	/// Empty while healthy; otherwise names the entry point whose attach failed.
	std::string_view GetInstallError();

	/// Arms interception for the duration of one replica's DrawSettings call.
	class Scope
	{
	public:
		explicit Scope(const Context& context);
		~Scope();

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Context previous;
		bool previousArmed;
	};

	/// A control that binds a stack temporary rather than its settings member, as the Util:: wrappers
	/// that rescale a value before handing it to ImGui do.
	using Proxy = SettingsControl::Proxy;

	/// Declares the proxy for the ImGui control drawn inside it, so a rescaled temporary still binds.
	using ProxyScope = SettingsControl::ProxyScope;

	/**
	 * @brief Binds a custom int control no detour sees, e.g. Util::SegmentedControl, as the ImGui ones are bound.
	 * @param a_draw Draws the control against the int it is handed and returns whether it changed it.
	 */
	bool InterceptInt(const char* a_label, int& a_value, const std::function<bool(int&)>& a_draw);
	/** @brief Captures a baseline edit after the outermost settings UI scope returns. */
	void RequestExternalCapture();
	/** @brief Whether the generator proved a uniquely labelled static visibility control is UI-only. */
	bool IsUiOnlyControl(const char* label, std::string_view kind);
	/** @brief Whether this frame's input can activate or edit a settings control. */
	bool InputMayEditSettings();

	/// The armed context, or nullptr when unarmed. For SceneWidgetBinding and CustomSceneControls.
	const Context* GetArmedContext();

	/// The proxy declared around the control being drawn, or nullptr. For SceneWidgetBinding only.
	const Proxy* GetArmedProxy();
}

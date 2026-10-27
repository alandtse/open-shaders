#pragma once

/**
 * Skips the engine's per-frame attach walk for dynamic lights whose state has not changed.
 *
 * The engine re-enables every dynamic point light each frame, and each call re-walks the scene to rebuild that light's
 * attached-geometry list. Light Limit Fix lights world surfaces from its clustered pipeline and reads only a light's own room and
 * portal data, so an unchanged light's walk reproduces the previous frame.
 */
namespace LightAttach
{
	/** @brief True once the gate is installed, which needs a runtime whose per-frame dynamic light call was verified. */
	[[nodiscard]] bool IsSupported();

	/** @brief Replaces the per-frame dynamic light enable call so unchanged lights skip their attach walk. */
	void Install();

	/** @brief Drops every tracked light so each is walked on its next visit; call when the scene is torn down and light addresses may be reused. */
	void ResetTracking();

	/** @brief Whether the audit also lists every non-shadow light's rooms, portals and position. Runtime only, never saved. */
	[[nodiscard]] bool GetAuditDetail();
	void SetAuditDetail(bool a_enabled);

	/** @brief Walk and skip totals since launch plus digests of each light's rooms, portals and attached geometry, merged into the Light Limit Fix diagnostics. */
	json GetDiagnostics();
}

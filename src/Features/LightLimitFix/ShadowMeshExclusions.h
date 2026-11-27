#pragma once

#include <nlohmann/json_fwd.hpp>

namespace RE
{
	class BSRenderPass;
}

namespace ShadowMeshExclusions
{
	/** @brief Loads the union of the mesh lists in the exclusion directory. */
	void Reload();
	/** @brief Shows the file editor and the world mesh picker. */
	void DrawSettings();
	/** @brief Tests the current point-light shadow draw against the loaded NIFs. */
	bool ShouldSkip(const RE::BSRenderPass* pass);
	/** @brief Consumes an edit that requires cached point-light shadows to be redrawn. */
	bool ConsumeShadowRefresh();
	/** @brief Returns the loaded files, mesh paths and number of excluded shadow draws. */
	nlohmann::json Inspect();
}

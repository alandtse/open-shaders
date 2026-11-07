#pragma once

namespace RE
{
	class BSRenderPass;
}

namespace VanillaBinding
{
	/** @brief Binds shared pixel and compute buffers at the runtime's supported slots. */
	void BindSharedConstantBuffers();
	/** @brief Binds current scene depth for custom pixel shaders. */
	void BindSceneDepth();
	/** @brief Binds shared buffers and non-conflicting depth for sun glare draws. */
	void BindSkyResources(const RE::BSRenderPass* a_pass);
	/** @brief Supplies vanilla resources required by the current custom draw. */
	void BindDrawResources();
}

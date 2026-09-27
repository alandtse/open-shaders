#pragma once

/**
 * @brief Refuses new alpha GeometryGroup slots before the engine's global array overflows.
 *
 * BSBatchRenderer::StartGroupingAlphas bump-allocates from a fixed array with no capacity check;
 * past the end it reads adjacent .rdata as a bogus `this`, and the counter's use as the element
 * count in SortAlphaGeometryGroups' qsort corrupts memory. Returning null is a path callers
 * already handle.
 */
struct AlphaGeometryGroupCeilingFix : EngineFix
{
	std::string GetName() override { return "Alpha GeometryGroup Ceiling Fix"; }
	const char* GetEngineFixesName() const override { return "BatchRendererAlphaGeometryGroupOverflow"; }

	void Install() override;

	/** @brief High-water live count since load; well under capacity means no scene neared the array. */
	static inline std::atomic<uint32_t> peak{ 0 };
	/** @brief Allocations refused at the ceiling. */
	static inline std::atomic<uint64_t> drops{ 0 };

	struct BSBatchRenderer_StartGroupingAlphas
	{
		static void* thunk(RE::BSBatchRenderer* a_this, void* a_bound, RE::NiCamera* a_camera, bool a_sortByClosestPoint);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

#pragma once

#include "EngineFix.h"
#include "Utils/GameSetting.h"

#include <array>
#include <limits>

// This overrides the shadow cascade rasterizers to fix issues with peter panning and self shadowing.
// Flat keeps the v1.5.2 shadowmap rasterizer table behavior; VR always bypasses that global table swap.
struct ShadowmapRasterizerFix : EngineFix
{
	std::string GetName() override { return "Shadowmap Cascade Rasterizer Fix"; }
	void Install() override;

	using RasterStateArray = ID3D11RasterizerState* [2][3][12][2];

	struct ShadowMapRasterizerDescriptor
	{
		int rasterDepthBias;
		float rasterDepthBiasClamp;
		float rasterSlopeScaleBias;
	};

	static constexpr std::uint32_t maxCascades = 3;
	static constexpr std::uint32_t invalidCascade = std::numeric_limits<std::uint32_t>::max();
	static void InstallD3DHooks(ID3D11DeviceContext* context);
	static void InitializeRasterStates();
	static void CloneRasterStates(const RasterStateArray& inputArray, std::uint32_t cascade, const std::array<ShadowMapRasterizerDescriptor, maxCascades>& descriptors);
	static void ReleaseClonedRasterStates();
	static void RebuildBiasedRasterStateLookup();
	static ID3D11RasterizerState* GetBiasedRasterState(ID3D11RasterizerState* state);
	static bool IsVROuterCascadeCasterBiasEnabled();

	static inline std::uint32_t numCascades = 0;
	static inline std::uint32_t currentCascade = 0;
	static inline std::uint32_t activeCascade = invalidCascade;
	static inline bool initialized = false;
	static inline bool d3dHooksInstalled = false;

	static inline RasterStateArray* gRasterStates = nullptr;
	static inline RasterStateArray backupGameRasterStates = {};
	static inline RasterStateArray shadowmapRasterStates[maxCascades] = {};

	static void GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor desc);

	static constexpr int flatFirstCascadeDepthBias = 160;
	static constexpr float flatFirstCascadeDepthBiasClamp = 0.015f;
	static constexpr float flatFirstCascadeSlopeScaleBias = 3.2f;

	static constexpr int flatSecondCascadeDepthBias = 100;
	static constexpr float flatSecondCascadeDepthBiasClamp = 0.015f;
	static constexpr float flatSecondCascadeSlopeScaleBias = 3.8f;

	static constexpr int flatThirdCascadeDepthBias = 100;
	static constexpr float flatThirdCascadeDepthBiasClamp = 0.015f;
	static constexpr float flatThirdCascadeSlopeScaleBias = 3.8f;

	static constexpr std::array<ShadowMapRasterizerDescriptor, maxCascades> flatCascadeDescriptors = {
		ShadowMapRasterizerDescriptor{ flatFirstCascadeDepthBias, flatFirstCascadeDepthBiasClamp, flatFirstCascadeSlopeScaleBias },
		ShadowMapRasterizerDescriptor{ flatSecondCascadeDepthBias, flatSecondCascadeDepthBiasClamp, flatSecondCascadeSlopeScaleBias },
		ShadowMapRasterizerDescriptor{ flatThirdCascadeDepthBias, flatThirdCascadeDepthBiasClamp, flatThirdCascadeSlopeScaleBias }
	};

	// Keep close-range casters unshifted; even small first-cascade bias can make nearby meshes lose contact shadows.
	static constexpr int vrNearCascadeDepthBias = 0;
	static constexpr float vrNearCascadeDepthBiasClamp = 0.0f;
	static constexpr float vrNearCascadeSlopeScaleBias = 0.0f;

	// Developer option only: a tiny outer-cascade caster offset for distant directional shadow acne.
	static constexpr int vrOuterCascadeDepthBias = 8;
	static constexpr float vrOuterCascadeDepthBiasClamp = 0.0001f;
	static constexpr float vrOuterCascadeSlopeScaleBias = 0.05f;

	static constexpr std::array<ShadowMapRasterizerDescriptor, maxCascades> vrCascadeDescriptors = {
		ShadowMapRasterizerDescriptor{ vrNearCascadeDepthBias, vrNearCascadeDepthBiasClamp, vrNearCascadeSlopeScaleBias },
		ShadowMapRasterizerDescriptor{ vrOuterCascadeDepthBias, vrOuterCascadeDepthBiasClamp, vrOuterCascadeSlopeScaleBias },
		ShadowMapRasterizerDescriptor{ vrOuterCascadeDepthBias, vrOuterCascadeDepthBiasClamp, vrOuterCascadeSlopeScaleBias }
	};

	struct ScopedCascadeBias
	{
		explicit ScopedCascadeBias(std::uint32_t cascade);
		~ScopedCascadeBias();

		std::uint32_t previousCascade;
	};

	struct BSShadowDirectionalLight_RenderShadowmaps_RenderCascade
	{
		static void thunk(RE::BSShadowDirectionalLight* light, void* arg1, void* arg2, std::uint32_t flags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_RSSetState
	{
		static void thunk(ID3D11DeviceContext* context, ID3D11RasterizerState* state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	std::map<std::string, Util::GameSetting> Settings{
		{ "iNumSplits:Display", { "Number of Shadow Map Cascades (INI) ",
									"Controls the number of shadow map cascades used for directional lighting. "
									"Higher values provide better shadow quality but use more GPU resources. "
									"Maximum of 3 cascades supported. ",
									REL::Relocate<uintptr_t>(0, 0, 0x1ed6350), 2, 1, 3 } },
	};
};

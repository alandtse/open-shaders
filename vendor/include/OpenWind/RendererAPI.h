#pragma once

#include "OpenWind/Authoring.h"
#include "OpenWind/RendererTypes.h"

namespace OpenWind
{
	/** Field components the renderer can evaluate alongside the baseline wind field. */
	enum class RendererCapability : uint32_t
	{
		FineWind = 1u << 0,
		TerrainShelter = 1u << 1,
		ObstacleShelter = 1u << 2
	};

	/** Immutable DDS pixels borrowed only for the duration of the upload callback. */
	struct TerrainHeightmapView
	{
		uint32_t width{}, height{}, pitch{}, format{};
		float3 minimum{}, maximum{};
		const void* pixels{};
	};

	/** A compatible renderer supplies typed mesh operations; Open Wind owns command handling. */
	struct RendererHost
	{
		uint32_t structSize = sizeof(RendererHost);
		uint32_t revision = kRendererRevision;
		uint32_t frameSize = sizeof(RendererFrame);
		void* context{};
		const AuthoringHost* authoring{};
		uint32_t capabilities{};
	};

	/** Render-thread lifecycle and field transport, separate from the public sampling contract. */
	struct RendererAPI
	{
		uint32_t structSize = sizeof(RendererAPI);
		uint32_t revision = kRendererRevision;
		uint32_t frameSize = sizeof(RendererFrame);
		bool (*Bind)(const RendererHost* host) noexcept {};
		bool (*Update)(RendererFrame* frame) noexcept {};
		bool (*CopyFrame)(RendererFrame* frame) noexcept {};
		void (*DataLoaded)() noexcept {};
		void (*ResetScene)(bool opening) noexcept {};
		bool (*SetSettings)(const WindSettings* settings, uint32_t size) noexcept {};
		bool (*SetControls)(const WindRuntimeState* controls, uint32_t size) noexcept {};
		/** Visits this world's terrain texture without transferring allocation ownership. */
		bool (*VisitTerrainHeightmap)(uint32_t worldspaceId, void* context,
			bool (*visitor)(void*, const TerrainHeightmapView*) noexcept) noexcept {};
	};

	/** Returns the current renderer interface regardless of the requested revision. */
	using RequestRendererAPI = const RendererAPI* (*)(uint32_t revision) noexcept;
}

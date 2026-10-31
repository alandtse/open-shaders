#pragma once

#include <cstdint>

namespace OpenWind
{
	/** Tree response values exchanged without renderer geometry or allocator ownership. */
	struct TreeAuthoringValues
	{
		float bend{};
		float leafAmbient{};
		float upperBendRange{};
		float maximumDisplacementPercent{};
		float trunkGustInfluence{};
		float leafGustInfluence{};
		float transientWindInfluence{};
		float leafTransientWindInfluence{};
		float leafTransientFlutterMaximum{};
		float transientMaximumBendMultiplier{};
	};

	/** Rule snapshot; mesh is borrowed only for the duration of the visitor call. */
	struct AuthoringRule
	{
		uint64_t index{};
		const char* mesh{};
		bool unsaved{};
		TreeAuthoringValues tree{};
		float grassBend{};
		float grassFlutter{};
		bool bendEnabled = true;
	};

	/** Save result; strings are borrowed only for the duration of the visitor call. */
	struct AuthoringSaveResult
	{
		bool success{};
		uint64_t savedRuleCount{};
		const char* path{};
		const char* error{};
	};

	/** Renderer-owned mesh operations; unsupported callbacks may be null. Calls are synchronous on the menu thread. */
	struct AuthoringHost
	{
		bool (*VisitRules)(void*, bool grass, void* visitor, bool (*visit)(void*, const AuthoringRule*) noexcept) noexcept {};
		bool (*VisitPaths)(void*, bool grass, bool unconfigured, void* visitor, bool (*visit)(void*, const char*) noexcept) noexcept {};
		bool (*GetUniversal)(void*, bool* enabled, TreeAuthoringValues*) noexcept {};
		bool (*SaveRules)(void*, bool grass, void* visitor, bool (*visit)(void*, const AuthoringSaveResult*) noexcept) noexcept {};
		bool (*RevertRules)(void*, bool grass) noexcept {};
		bool (*AddGrassRule)(void*, const char* mesh) noexcept {};
		bool (*SetGrassRule)(void*, uint64_t index, float bend, float flutter) noexcept {};
		bool (*SetTreeRule)(void*, const char* mesh, const TreeAuthoringValues*) noexcept {};
		/** A null values pointer restores the renderer's default universal response values. */
		bool (*SetUniversal)(void*, bool enabled, const TreeAuthoringValues*) noexcept {};
		/** Sets the existing per-mesh bend exclusion without altering response values. */
		bool (*SetTreeBendEnabled)(void*, const char* mesh, bool enabled) noexcept {};
	};

	/** Consumes UTF-8 JSON during the call; no response buffer retry can repeat a mutation. */
	using AuthoringResponse = void (*)(void* context, const char* response) noexcept;
}

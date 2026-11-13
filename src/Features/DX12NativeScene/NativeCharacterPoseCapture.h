#pragma once

#include "RE/Skyrim.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace DX12Native
{
	/** @brief Owns Skyrim's current VS skin-palette bytes for one observed part.
     * Captured only inside the renderer callback; never hands the engine's
     * boneMatrices pointer or bone nodes to an independent worker.
     */
	struct CharacterPoseCapture
	{
		uint32_t actorFormID = 0, partOrdinal = 0, capturedFrame = 0;
		uint32_t skinFrameID = 0, boneCount = 0, registerCount = 0;
		uint64_t sourceToken = 0;
		std::vector<std::array<float, 4>> registers;
	};

	inline std::optional<CharacterPoseCapture> CaptureCharacterShaderPose(
		RE::BSGeometry& geometry, uint32_t actorFormID,
		uint32_t partOrdinal, uint64_t sourceToken, uint32_t frame)
	{
		const auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get();
		if (!skin || !skin->boneMatrices || !skin->numMatrices ||
			skin->numMatrices > 80 ||
			skin->numRegisters != skin->numMatrices * 3u ||
			skin->numRegisters > 240u)
			return std::nullopt;

		CharacterPoseCapture result;
		result.actorFormID = actorFormID;
		result.partOrdinal = partOrdinal;
		result.sourceToken = sourceToken;
		result.capturedFrame = frame;
		result.skinFrameID = skin->frameID;
		result.boneCount = skin->numMatrices;
		result.registerCount = skin->numRegisters;
		result.registers.resize(result.registerCount);
		// A matrix is three float4 VS registers in the existing Skyrim
		// Skinned.hlsli contract. Treat the bytes as raw input; transformation
		// space and pivot parity are not assumed verified.
		std::memcpy(result.registers.data(), skin->boneMatrices,
			result.registers.size() * sizeof(result.registers[0]));
		for (const auto& v : result.registers)
			for (float component : v)
				if (!std::isfinite(component))
					return std::nullopt;
		return result;
	}
}

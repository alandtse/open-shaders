#pragma once

#include "Tuning.h"

#include <d3d12.h>
#include <filesystem>
#include <memory>

namespace NR
{
	struct FrameParameters
	{
		float jitterX = 0, jitterY = 0, frameTimeMs = 0;
		DirectX::SimpleMath::Matrix worldToView, viewToClip;
		bool reset = true;
		bool created = false;
		uint32_t result = 0;
	};

	/** @brief Owns the cached NGX ABI and one persistent Feature 18 handle per eye. */
	class Runtime
	{
	public:
		Runtime();
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;
		/** @brief Loads the supported NR runtime and resolves its function table once. */
		void Initialize(ID3D12Device* device, const std::filesystem::path& directory);
		/** @brief Releases temporal instances after the caller has retired GPU work. */
		void ResetFeatures();
		/** @brief Creates or evaluates a full-resolution display-referred proxy for one OS eye. */
		bool Evaluate(ID3D12GraphicsCommandList* commands, uint32_t eye,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
			uint32_t width, uint32_t height, uint32_t guideWidth, uint32_t guideHeight, FrameParameters& frame, const Tuning& tuning);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}

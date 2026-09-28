#pragma once

#include "Tuning.h"
#include "Utils/StringUtils.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <d3d11.h>
#include <d3d12.h>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace NR
{
	/** @brief True while the process is exiting: NGX teardown is skipped and nothing may log. */
	inline std::atomic<bool> processTerminating{ false };

	/** @brief Prefix that marks a failure as happening while the runtime starts up. */
	inline constexpr const char* kInitializationPrefix = "initialization failed: ";

	/** @brief Major version of the nvngx_dlssnr.dll builds this pass accepts. */
	inline constexpr uint32_t kRequiredRuntimeMajor = 310;
	/** @brief Minor version of the nvngx_dlssnr.dll builds this pass accepts. */
	inline constexpr uint32_t kRequiredRuntimeMinor = 8;

	/**
	 * @brief Marks process teardown. Declare it as the LAST member of the object that owns the
	 *        runtime: members are destroyed in reverse order, so it flips before any NGX teardown.
	 */
	struct TerminationSentinel
	{
		~TerminationSentinel() { processTerminating.store(true, std::memory_order_relaxed); }
	};

	/** @brief Reason a runtime version cannot be used, or empty when it is the accepted 310.8.x. */
	template <class V>
	std::string UnsupportedRuntimeReason(const std::optional<V>& version, std::string_view directory)
	{
		if (!version)
			return std::format("nvngx_dlssnr.dll in {} has no version information", directory);
		if (version->major() != kRequiredRuntimeMajor || version->minor() != kRequiredRuntimeMinor)
			return std::format("unsupported runtime version {} (needs {}.{})", version->string("."), kRequiredRuntimeMajor, kRequiredRuntimeMinor);
		return {};
	}

	// SHA-256 of the nvngx_dlssnr.dll builds (310.8 DVS Production, 310.8.0.0 test) whose channel
	// order was verified in game. A build outside this list is refused, not rendered on trust.
	inline constexpr std::array<std::string_view, 2> kValidatedRuntimeSha256{
		"8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206",
		"E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E",
	};

	/** @brief True when a SHA-256 hex digest names a validated build; hex case is ignored. */
	inline bool IsValidatedRuntimeHash(std::string_view digest)
	{
		for (const auto validated : kValidatedRuntimeSha256) {
			if (Util::IEquals(validated, digest))
				return true;
		}
		return false;
	}

	/** @brief True when an image path sits under <systemDirectory>\DriverStore\, i.e. NVIDIA's own core. */
	inline bool IsUnderDriverStore(std::wstring_view image, std::wstring_view systemDirectory)
	{
		const std::wstring prefix = std::wstring(systemDirectory) + L"\\DriverStore\\";
		if (image.size() < prefix.size())
			return false;
		return CompareStringOrdinal(image.data(), static_cast<int>(prefix.size()),
				   prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
	}

	/** @brief Per-eye render width; the NR pass and Upscale() must derive it identically. */
	inline uint32_t EyeRenderWidth(uint32_t renderWidth, uint32_t eyes)
	{
		return eyes ? renderWidth / eyes : renderWidth;
	}

	/** @brief True when a colour target can host NR's proxy for this per-eye render size. */
	inline bool IsSupportedOutput(const D3D11_TEXTURE2D_DESC& desc, uint32_t width, uint32_t height, uint32_t eyes)
	{
		const bool formatSupported = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		                             desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R11G11B10_FLOAT;
		return width && height && desc.Width >= width * eyes && desc.Height >= height &&
		       desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && formatSupported;
	}

	/** @brief Active texel region of a guide resource supplied to Feature 18. */
	struct GuideRegion
	{
		uint32_t baseX = 0, baseY = 0, width = 0, height = 0;
	};

	/** @brief Independent guide regions and motion conversion to NR input pixels. */
	struct GuideParameters
	{
		GuideRegion depth, motion;
		float motionScaleX = 1.0f, motionScaleY = 1.0f;
	};

	struct FrameParameters
	{
		float jitterX = 0, jitterY = 0, frameTimeMs = 0;
		DirectX::SimpleMath::Matrix worldToView, viewToClip;
		bool feedCameraData = false;
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
		/** @brief Version of the accepted nvngx_dlssnr.dll; empty until Initialize succeeds. */
		[[nodiscard]] std::string Version() const;
		/** @brief Creates or evaluates a full-resolution display-referred proxy for one OS eye. */
		bool Evaluate(ID3D12GraphicsCommandList* commands, uint32_t eye,
			ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
			uint32_t width, uint32_t height, const GuideParameters& guides,
			FrameParameters& frame, const Tuning& tuning);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}

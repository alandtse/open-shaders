#pragma once

#include "Buffer.h"
#include "Utils/LazyShader.h"

/**
 * @brief Min- or max-reduction depth mip pyramid, optionally built separately per stereo eye.
 *
 * A texel at level N is exactly the reduction of everything beneath it, so a consumer can test an
 * object of any on-screen size against a fixed number of texels. One object owns one reduction op;
 * a consumer that needs both holds two. A stereo consumer that wants a min and a max chain per eye
 * builds both objects with eyeSeparated set and reads GetSRV(eye) from each:
 * @code
 * hiZMin.Build(device, ctx, { depthSRV, HiZPyramid::Reduction::Min, true, true, true });
 * hiZMax.Build(device, ctx, { depthSRV, HiZPyramid::Reduction::Max, true, true, true });
 * @endcode
 */
class HiZPyramid
{
public:
	/** @brief Which value a parent texel takes from its children. */
	enum class Reduction
	{
		Max,  ///< Farthest child depth; a test against it can only under-report occlusion.
		Min   ///< Nearest child depth.
	};

	/** @brief One frame's build request. */
	struct BuildDesc
	{
		/** @brief Source depth. A null SRV invalidates the pyramid for this frame. */
		ID3D11ShaderResourceView* sourceDepth = nullptr;
		/** @brief Reduction every level of the chain stores. */
		Reduction reduction = Reduction::Max;
		/**
		 * @brief Build one chain per stereo eye, each reduced only from its own half of the packed
		 * side-by-side source, instead of one chain over the whole source.
		 */
		bool eyeSeparated = false;
		/** @brief True when sourceDepth is a UNORM-class view, such as the game's R24_UNORM_X8_TYPELESS depth copy. */
		bool sourceUnormTyped = false;
		/** @brief True when sourceDepth is still bound as a depth target, so the base dispatch must unbind it first. */
		bool sourceBoundAsRenderTarget = false;
	};

	/** @brief Extents of the last successful Build, for the caller's own logging and constants. */
	struct Stats
	{
		/** Source extent that was reduced, in texels; one eye's extent when the chains are eye-separated. */
		uint32_t sourceWidth = 0;
		uint32_t sourceHeight = 0;
		/** Allocated base-level extent, in texels, padded to the reduction chain's tile granularity. */
		uint32_t textureWidth = 0;
		uint32_t textureHeight = 0;
	};

	/** @brief Source texels one base-level texel covers, on both axes. */
	static constexpr uint32_t kDownsampleFactor = 4;

	/**
	 * @brief Rebuilds every chain from desc.sourceDepth.
	 * @param device Device used to create the per-mip views when the size changes.
	 * @param ctx Immediate context the reduction dispatches are issued on.
	 * @param desc This frame's source, reduction and layout.
	 * @return True when the pyramid is valid for this frame.
	 */
	bool Build(ID3D11Device* device, ID3D11DeviceContext* ctx, const BuildDesc& desc);

	/** @brief Marks the pyramid unusable for this frame without releasing anything. */
	void Invalidate() { valid = false; }

	/**
	 * @brief Returns one eye's full-chain SRV, or nullptr when the pyramid is not valid this frame.
	 * @param eye 0 is the source's left half, 1 its right half. Only 0 exists unless BuildDesc::eyeSeparated was set.
	 */
	ID3D11ShaderResourceView* GetSRV(uint32_t eye = 0) const { return valid && eye < eyeCount ? textures[eye]->srv.get() : nullptr; }

	/** @brief Returns true when Build succeeded for the current frame. */
	bool IsValid() const { return valid; }
	/** @brief Returns the number of independent chains: 2 when eye-separated, 1 otherwise. */
	uint32_t GetEyeCount() const { return eyeCount; }
	/** @brief Returns the base level width in texels, per chain. */
	uint32_t GetWidth() const { return width; }
	/** @brief Returns the base level height in texels. */
	uint32_t GetHeight() const { return height; }
	/** @brief Returns the number of trustworthy mip levels; 1 when the reduction chain is absent. */
	uint32_t GetMipCount() const { return spdCS ? mipCount : 1u; }
	/** @brief Returns how many nominal screen pixels one base-level texel covers, scaled by dynamic resolution. */
	float GetTexelPixels() const { return texelPixels; }
	/** @brief Returns the extents of the last successful Build. */
	const Stats& GetStats() const { return stats; }

	/** @brief Creates the parameter constant buffer. Called from the feature's SetupResources. */
	void SetupResources();
	/** @brief Releases the cached compute shaders so they recompile on next use. */
	void ClearShaderCache();

private:
	/** @brief Must match Common/HiZBaseCS.hlsl's cbuffer HiZParams. */
	struct alignas(16) BaseParams
	{
		uint32_t sourceWidth;
		uint32_t sourceHeight;
		uint32_t baseWidth;
		uint32_t baseHeight;
		uint32_t windowOriginX;
		uint32_t windowWidth;
		uint32_t _pad0[2];  // explicit so alignas(16) does not warn (C4324)
	};
	STATIC_ASSERT_ALIGNAS_16(BaseParams);

	/** @brief Must match Common/SPD.hlsl's cbuffer SpdParams. */
	struct alignas(16) SPDParams
	{
		uint32_t sourceWidth;
		uint32_t sourceHeight;
		uint32_t outputMips;
		uint32_t totalGroups;
	};
	STATIC_ASSERT_ALIGNAS_16(SPDParams);

	/**
	 * @brief (Re)creates the per-eye pyramid textures and their per-mip views for a new size.
	 * @param count Number of independent chains to allocate.
	 */
	bool CreateTextures(ID3D11Device* device, uint32_t dstW, uint32_t dstH, uint32_t count);

	std::array<std::unique_ptr<Texture2D>, 2> textures;
	std::array<std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>>, 2> mipUAVs;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> mip0SRVs;
	std::unique_ptr<ConstantBuffer> paramsCB;

	Util::LazyShader<ID3D11ComputeShader> baseCS;
	Util::LazyShader<ID3D11ComputeShader> spdCS;

	Stats stats;
	Reduction reduction = Reduction::Max;
	uint32_t eyeCount = 1;
	uint32_t allocatedEyes = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	float texelPixels = (float)kDownsampleFactor;
	uint32_t paddedWidth = 0;
	uint32_t paddedHeight = 0;
	uint32_t mipCount = 1;
	bool valid = false;

	// SPD reduces a 64x64 tile wholly in LDS, so a base padded to that granularity halves exactly for
	// six levels. The seventh would floor an odd mip, dropping a row and leaving it unreduced.
	static constexpr uint32_t kExactMips = 6;
	static constexpr uint32_t tileSize = 64;
};

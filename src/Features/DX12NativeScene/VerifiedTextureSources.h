#pragma once

#include "Features/NativePBR/NativePBRCompiler.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace DX12Native
{
	/** @brief Provenance from SHA-256 verified DDS data actually published in a DX12 texture page. */
	struct VerifiedTextureSource
	{
		std::string digest;
		uint32_t width = 0, height = 0, mips = 0;
		bool ambiguous = false;
	};

	enum class LiveTextureSourceStatus : uint8_t
	{
		Available,
		Missing,
		Ambiguous,
		Incompatible
	};

	/** @brief CPU-only index of resident offline sources; never asserts identity with an active DX11 resource. */
	class VerifiedTextureSources
	{
	public:
		bool Observe(std::string_view a_path, std::string_view a_digest, uint32_t a_width, uint32_t a_height, uint32_t a_mips)
		{
			const auto path = NativePBR::CanonicalTexturePath(std::string(a_path));
			if (!path || a_digest.size() != 64 || !a_width || !a_height || !a_mips ||
				!std::all_of(a_digest.begin(), a_digest.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
				return false;
			std::string digest(a_digest);
			std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) { return char(std::tolower(c)); });
			auto [it, inserted] = sources.try_emplace(*path, VerifiedTextureSource{ digest, a_width, a_height, a_mips });
			if (!inserted && (it->second.digest != digest || it->second.width != a_width ||
								 it->second.height != a_height || it->second.mips != a_mips))
				it->second.ambiguous = true;
			// Conflicting normalized aliases remain ambiguous for this published scene.
			return !it->second.ambiguous;
		}

		LiveTextureSourceStatus Assess(const NativePBR::Recipe& a_recipe) const
		{
			constexpr std::array roles{ NativePBR::TextureRole::BaseColor, NativePBR::TextureRole::Normal,
				NativePBR::TextureRole::Rmaos, NativePBR::TextureRole::Emission };
			for (const auto role : roles) {
				const auto& request = a_recipe.textures[static_cast<size_t>(role)];
				if (request.canonicalPath.empty()) {
					if (request.required || !request.assetPath.empty())
						return LiveTextureSourceStatus::Missing;
					continue;
				}
				const auto it = sources.find(request.canonicalPath);
				if (it == sources.end())
					return LiveTextureSourceStatus::Missing;
				const auto& source = it->second;
				if (source.ambiguous)
					return LiveTextureSourceStatus::Ambiguous;
				const auto& observed = request.resource;
				// Only compare properties whose values were captured from a live D3D11 texture.
				// DXGI formats are intentionally not equated across typeless/sRGB views.
				if (observed.texture2D &&
					((observed.width && source.width != observed.width) ||
						(observed.height && source.height != observed.height) ||
						(observed.mipLevels && source.mips != observed.mipLevels) ||
						(observed.arraySize && observed.arraySize != 1) ||
						(observed.sampleCount && observed.sampleCount != 1)))
					return LiveTextureSourceStatus::Incompatible;
			}
			return LiveTextureSourceStatus::Available;
		}

		size_t Size() const { return sources.size(); }
		/** @brief Carries verified provenance only for paths retained in the next material generation. */
		VerifiedTextureSources Select(const std::unordered_set<std::string>& a_paths) const
		{
			VerifiedTextureSources result;
			for (const auto& path : a_paths)
				if (auto found = sources.find(path); found != sources.end())
					result.sources.emplace(*found);
			return result;
		}

	private:
		std::unordered_map<std::string, VerifiedTextureSource> sources;
	};
}

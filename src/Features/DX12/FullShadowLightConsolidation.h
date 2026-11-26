#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <vector>

namespace DX12Native
{
	inline constexpr uint32_t kDefaultRuntimeUnificationRadius = 64;
	inline constexpr uint32_t kDefaultPreparedExclusionRadius = 128;
	inline constexpr uint32_t kFullShadowRadiusMax = 512;
	inline constexpr uint32_t kFullShadowRadiusStep = 4;

	/** @brief Quantize the full-shadow spatial controls in game units. */
	inline uint32_t SanitizeFullShadowRadius(uint32_t radius)
	{
		return (std::min(radius, kFullShadowRadiusMax) + kFullShadowRadiusStep / 2) /
		       kFullShadowRadiusStep * kFullShadowRadiusStep;
	}

	template <class Light>
	bool SamePreparedShadowLight(const Light& a, const Light& b)
	{
		return a.referenceID == b.referenceID && a.flags == b.flags &&
		       a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
		       a.color.x == b.color.x && a.color.y == b.color.y && a.color.z == b.color.z &&
		       a.radius == b.radius && a.fade == b.fade && a.emitterRadius == b.emitterRadius &&
		       a.falloffExponent == b.falloffExponent;
	}

	namespace FullShadowDetail
	{
		using Cell = std::array<int64_t, 3>;

		template <class Position>
		Cell CellOf(const Position& p, uint32_t radius)
		{
			const auto axis = [radius](float v) {
				constexpr double limit = double(std::numeric_limits<int64_t>::max() / 2);
				return int64_t(std::clamp(std::floor(double(v) / radius), -limit, limit));
			};
			return { axis(p.x), axis(p.y), axis(p.z) };
		}

		template <class Position>
		bool Within(const Position& a, const Position& b, uint32_t radius)
		{
			const double x = double(a.x) - b.x, y = double(a.y) - b.y, z = double(a.z) - b.z;
			return x * x + y * y + z * z <= double(radius) * radius;
		}

		template <class Light>
		class SpatialIndex
		{
		public:
			explicit SpatialIndex(uint32_t a_radius) : radius(a_radius) {}

			void Add(const Light& light, size_t index)
			{
				buckets[CellOf(light.position, radius)].push_back(index);
			}

			template <class Visitor>
			void Visit(const Light& light, Visitor&& visitor) const
			{
				const auto cell = CellOf(light.position, radius);
				for (int z = -1; z <= 1; ++z)
					for (int y = -1; y <= 1; ++y)
						for (int x = -1; x <= 1; ++x) {
							const auto it = buckets.find({ cell[0] + x, cell[1] + y, cell[2] + z });
							if (it != buckets.end())
								for (const auto index : it->second)
									if (visitor(index))
										return;
						}
			}

		private:
			uint32_t radius;
			std::map<Cell, std::vector<size_t>> buckets;
		};

		template <class Light>
		bool FinitePosition(const Light& light)
		{
			return std::isfinite(light.position.x) && std::isfinite(light.position.y) && std::isfinite(light.position.z);
		}
	}

	/** @brief Preserve prepared authority; merge remaining same-owner runtime emitters around deterministic anchors. */
	template <class Light>
	std::vector<Light> ConsolidateFullShadowLights(std::span<const Light> prepared,
		std::span<const Light> runtime, uint32_t unifyRadius, uint32_t excludeRadius)
	{
		using namespace FullShadowDetail;
		std::vector<Light> result(prepared.begin(), prepared.end());
		result.reserve(prepared.size() + runtime.size());
		SpatialIndex<Light> preparedIndex(excludeRadius);
		if (excludeRadius)
			for (size_t i = 0; i < prepared.size(); ++i)
				if (FinitePosition(prepared[i]))
					preparedIndex.Add(prepared[i], i);

		std::vector<Light> candidates;
		candidates.reserve(runtime.size());
		for (const auto& light : runtime) {
			if (!FinitePosition(light))
				continue;
			bool excluded = false;
			if (excludeRadius)
				preparedIndex.Visit(light, [&](size_t i) {
					excluded = Within(light.position, prepared[i].position, excludeRadius);
					return excluded;
				});
			if (!excluded)
				candidates.push_back(light);
		}
		std::ranges::sort(candidates, {}, &Light::runtimeIdentity);
		if (!unifyRadius) {
			result.insert(result.end(), candidates.begin(), candidates.end());
			return result;
		}

		SpatialIndex<Light> runtimeIndex(unifyRadius);
		for (size_t i = 0; i < candidates.size(); ++i)
			if (candidates[i].referenceID)
				runtimeIndex.Add(candidates[i], i);

		std::vector<bool> consumed(candidates.size(), false);
		std::vector<size_t> members;
		for (size_t i = 0; i < candidates.size(); ++i) {
			if (consumed[i])
				continue;
			const auto& anchor = candidates[i];
			auto merged = anchor;
			consumed[i] = true;
			members.clear();
			members.push_back(i);
			if (anchor.referenceID)
				runtimeIndex.Visit(anchor, [&](size_t j) {
					if (!consumed[j] && candidates[j].referenceID == anchor.referenceID &&
						Within(anchor.position, candidates[j].position, unifyRadius)) {
						consumed[j] = true;
						members.push_back(j);
					}
					return false;
				});
			if (members.size() > 1) {
				std::ranges::sort(members);
				double red = 0, green = 0, blue = 0, weightSum = 0;
				double x = 0, y = 0, z = 0, plainX = 0, plainY = 0, plainZ = 0;
				for (const auto j : members) {
					const auto& source = candidates[j];
					const double fade = std::max(0.0f, source.fade);
					const double r = std::max(0.0f, source.color.x) * fade;
					const double g = std::max(0.0f, source.color.y) * fade;
					const double b = std::max(0.0f, source.color.z) * fade;
					constexpr std::array<double, 3> luminance{ .2126, .7152, .0722 };
					const double weight = luminance[0] * r + luminance[1] * g + luminance[2] * b;
					red += r; green += g; blue += b;
					weightSum += weight;
					x += weight * source.position.x;
					y += weight * source.position.y;
					z += weight * source.position.z;
					plainX += source.position.x;
					plainY += source.position.y;
					plainZ += source.position.z;
					merged.radius = std::max(merged.radius, source.radius);
					merged.emitterRadius = std::max(merged.emitterRadius, source.emitterRadius);
				}
				const auto toFloat = [](double value) {
					return float(std::min(value, double(std::numeric_limits<float>::max())));
				};
				merged.color.x = toFloat(red);
				merged.color.y = toFloat(green);
				merged.color.z = toFloat(blue);
				merged.fade = 1.0f;
				const double count = double(members.size());
				merged.position.x = float(weightSum > 0 ? x / weightSum : plainX / count);
				merged.position.y = float(weightSum > 0 ? y / weightSum : plainY / count);
				merged.position.z = float(weightSum > 0 ? z / weightSum : plainZ / count);
				merged.unifiedCount = uint32_t(members.size());
			}
			result.push_back(merged);
		}
		return result;
	}
}

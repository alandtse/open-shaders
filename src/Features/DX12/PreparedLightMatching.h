#pragma once

#include "DX12Resources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace DX12
{
	inline constexpr uint32_t kMaximumShadowLights = 64;

	struct RuntimeLight
	{
		uint32_t referenceID = 0, baseID = 0, shadowSlot = std::numeric_limits<uint32_t>::max();
		float3 position{}, color{};
		float radius = 0, fade = 1, emitterRadius = 0, falloffExponent = 1;
	};

	struct LightTuning
	{
		bool inheritRuntime = true;
		float matchDistance = 64.0f;
		float intensity = 1.0f, reach = 1.0f, emitterSize = 1.0f;
	};

	struct ResolvedLight
	{
		PreparedLight light;
		uint32_t runtimeShadowSlot = std::numeric_limits<uint32_t>::max();
		bool matched = false;
	};

	struct ResolvedLights
	{
		std::array<ResolvedLight, kMaximumShadowLights> values{};
		uint32_t count = 0;
	};

	/** @brief Matches prepared light identities to runtime observations without adding unmatched runtime lights. */
	inline ResolvedLights MatchPreparedLights(std::span<const PreparedLight> prepared,
		std::span<const RuntimeLight> runtime, LightTuning tuning, const float3& camera)
	{
		ResolvedLights output;
		if (prepared.empty())
			return output;
		const auto valid = [](float value) { return std::isfinite(value); };
		const auto finitePoint = [&](const float3& point) {
			return valid(point.x) && valid(point.y) && valid(point.z);
		};
		if (!finitePoint(camera))
			return output;
		const auto bounded = [&](float value, float fallback, float low, float high) {
			return std::clamp(valid(value) ? value : fallback, low, high);
		};
		tuning.matchDistance = bounded(tuning.matchDistance, 64.0f, 1.0f, 256.0f);
		tuning.intensity = bounded(tuning.intensity, 1.0f, 0.0f, 8.0f);
		tuning.reach = bounded(tuning.reach, 1.0f, 0.1f, 4.0f);
		tuning.emitterSize = bounded(tuning.emitterSize, 1.0f, 0.1f, 8.0f);
		const auto squaredDistance = [](const float3& a, const float3& b) {
			const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
			return x * x + y * y + z * z;
		};
		std::vector<const PreparedLight*> candidates;
		candidates.reserve(prepared.size());
		for (const auto& light : prepared)
			if (light.referenceID && finitePoint(light.position) && finitePoint(light.color) &&
				valid(light.radius) && light.radius > 0 && valid(light.fade) &&
				valid(light.emitterRadius) && valid(light.falloffExponent))
				candidates.push_back(&light);
		const auto nearer = [&](const auto* a, const auto* b) {
			const auto da = squaredDistance(a->position, camera), db = squaredDistance(b->position, camera);
			return da == db ? a->referenceID < b->referenceID : da < db;
		};
		if (candidates.size() > kMaximumShadowLights) {
			std::nth_element(candidates.begin(), candidates.begin() + kMaximumShadowLights, candidates.end(), nearer);
			candidates.resize(kMaximumShadowLights);
		}
		std::ranges::sort(candidates, nearer);
		std::vector<bool> claimed(runtime.size(), false);
		for (const auto* candidate : candidates) {
			if (output.count == kMaximumShadowLights)
				break;
			ResolvedLight resolved{};
			resolved.light = *candidate;
			size_t match = runtime.size();
			float best = tuning.matchDistance * tuning.matchDistance;
			for (size_t i = 0; i < runtime.size(); ++i) {
				if (claimed[i] || !finitePoint(runtime[i].position) || !valid(runtime[i].radius) || runtime[i].radius <= 0)
					continue;
				const float distance = squaredDistance(candidate->position, runtime[i].position);
				const bool identityMatch = runtime[i].referenceID && runtime[i].referenceID == candidate->referenceID;
				const bool baseMatch = runtime[i].baseID && runtime[i].baseID == candidate->baseID;
				if (!identityMatch && (!baseMatch || distance > best))
					continue;
				if (identityMatch || distance < best) {
					match = i;
					best = identityMatch ? -1.0f : distance;
					if (identityMatch)
						break;
				}
			}
			if (match != runtime.size()) {
				claimed[match] = true;
				resolved.matched = true;
				resolved.runtimeShadowSlot = runtime[match].shadowSlot;
				if (tuning.inheritRuntime) {
					resolved.light.position = runtime[match].position;
					if (finitePoint(runtime[match].color))
						resolved.light.color = runtime[match].color;
					resolved.light.radius = runtime[match].radius;
					if (valid(runtime[match].fade))
						resolved.light.fade = runtime[match].fade;
					if (valid(runtime[match].emitterRadius))
						resolved.light.emitterRadius = runtime[match].emitterRadius;
					if (valid(runtime[match].falloffExponent))
						resolved.light.falloffExponent = runtime[match].falloffExponent;
				}
			}
			resolved.light.radius = std::clamp(resolved.light.radius * tuning.reach, 0.01f, 65536.0f);
			resolved.light.fade = std::clamp(resolved.light.fade * tuning.intensity, 0.0f, 64.0f);
			resolved.light.emitterRadius = std::clamp(resolved.light.emitterRadius * tuning.emitterSize, 0.0f, 512.0f);
			output.values[output.count++] = resolved;
		}
		return output;
	}
}

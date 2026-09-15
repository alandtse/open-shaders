#pragma once

#include "Features/Wind/WindMath.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace WindImpactDedup
{
	/**
	 * @brief Records a_position as a new recent impact if it isn't a duplicate of one already
	 * tracked in a_recentImpacts, evicting entries older than a_duplicateWindow first and
	 * rejecting the impact if the recent burst or same/cross-projectile radius checks fail.
	 * @return true if a_recentImpacts was updated with the new impact.
	 */
	template <class RecentImpact>
	bool AcceptImpact(std::vector<RecentImpact>& a_recentImpacts, std::uintptr_t a_projectileIdentity,
		const RE::NiPoint3& a_position, float a_duplicateWindow, float a_crossImpactWindow,
		float a_burstWindow, std::size_t a_maximumSourcesPerBurst, std::size_t a_maximumRecentImpacts)
	{
		const auto now = std::chrono::steady_clock::now();
		const auto secondsSince = [&](const RecentImpact& a_recent) {
			return std::chrono::duration<float>(now - a_recent.time).count();
		};

		std::erase_if(a_recentImpacts, [&](const RecentImpact& a_recent) {
			return secondsSince(a_recent) > a_duplicateWindow;
		});
		if (std::ranges::count_if(a_recentImpacts, [&](const RecentImpact& a_recent) {
				return secondsSince(a_recent) <= a_burstWindow;
			}) >= static_cast<std::ptrdiff_t>(a_maximumSourcesPerBurst))
			return false;

		for (const auto& recent : a_recentImpacts) {
			const bool sameProjectile = recent.projectile == a_projectileIdentity;
			if (!sameProjectile && secondsSince(recent) > a_crossImpactWindow)
				continue;
			const float deduplicationRadius = sameProjectile ? 256.0f : 96.0f;
			if (WindMath::SquaredDistance(recent.position, a_position) <= deduplicationRadius * deduplicationRadius)
				return false;
		}

		a_recentImpacts.push_back({ a_projectileIdentity, a_position, now });
		if (a_recentImpacts.size() > a_maximumRecentImpacts)
			a_recentImpacts.erase(a_recentImpacts.begin());
		return true;
	}
}

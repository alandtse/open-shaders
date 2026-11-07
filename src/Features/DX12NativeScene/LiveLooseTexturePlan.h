#pragma once

#include "TruePBR/VerifiedBridge.h"
#include <array>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
    /** @brief Bounded CPU-side plan; this does not read loose files or publish any resource. */
    struct LiveLooseTexturePlan
    {
        std::vector<std::string> paths;
        size_t alreadyPresent = 0, deferred = 0, invalid = 0;
    };

    /** @brief Finds only DDS requests used by uniquely matched prepared instances. */
    inline LiveLooseTexturePlan PlanLiveLooseTextures(const NativePBR::LiveMatchResult& a_matches,
        const std::unordered_set<std::string>& a_present, size_t a_maxTextures = 8)
    {
        LiveLooseTexturePlan plan;
        std::unordered_set<std::string> visited;
        constexpr std::array roles{ NativePBR::TextureRole::BaseColor, NativePBR::TextureRole::Normal,
            NativePBR::TextureRole::Rmaos, NativePBR::TextureRole::Emission };
        for (const auto& match : a_matches.matches) {
            if (!match || !match->recipe) continue;
            for (const auto role : roles) {
                const auto& request = match->recipe->textures[static_cast<size_t>(role)];
                if (request.canonicalPath.empty()) {
                    if (request.required || !request.assetPath.empty()) ++plan.invalid;
                    continue;
                }
                if (!request.canonicalPath.ends_with(".dds")) { ++plan.invalid; continue; }
                if (!visited.insert(request.canonicalPath).second) continue;
                if (a_present.contains(request.canonicalPath)) { ++plan.alreadyPresent; continue; }
                if (plan.paths.size() >= a_maxTextures) { ++plan.deferred; continue; }
                plan.paths.push_back(request.canonicalPath);
            }
        }
        return plan;
    }
}

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DX12Native
{
    /** @brief Static geometry captured from live Skyrim CPU arrays.
     *
     * No BSGeometry, raw engine pointers, COM objects or D3D11 contexts survive
     * this boundary. GPU-only and animated meshes are not captured.
     */
    struct RuntimeGeometryPacket
    {
        std::string referenceKey, shapeName;
        std::array<float, 12> transform{};
        std::array<float, 4> bound{};
        uint32_t vertexCount = 0, vertexStride = 0;
        uint32_t indexCount = 0;
        bool hasNormal = false, hasUv = false, hasTangent = false;
        uint32_t normalOffset = 0, uvOffset = 0, tangentOffset = 0;
        uint64_t captureFrame = 0;
        std::vector<uint8_t> vertices;
        std::vector<uint16_t> indices;
        uint64_t Bytes() const { return vertices.size() + uint64_t(indices.size()) * sizeof(uint16_t); }
    };

    struct RuntimeGeometryInput
    {
        std::string referenceKey, shapeName;
        std::array<float, 12> transform{};
        std::array<float, 4> bound{};
        uint32_t vertexCount = 0, vertexStride = 0, triangleCount = 0;
        bool skinned = false, dynamic = false, alpha = false;
        bool hasNormal = false, hasUv = false, hasTangent = false;
        uint32_t normalOffset = 0, uvOffset = 0, tangentOffset = 0;
        uint64_t frame = 0;
        std::span<const uint8_t> vertices;
        std::span<const uint16_t> indices;
    };

    /** @brief Validate before touching borrowed engine CPU arrays. */
    inline bool ValidRuntimeGeometryInput(const RuntimeGeometryInput& input, size_t maxBytes = 512 * 1024)
    {
        if (input.skinned || input.dynamic || input.alpha ||
            input.referenceKey.empty() || input.shapeName.empty() ||
            input.referenceKey.size() > 256 || input.shapeName.size() > 1024 ||
            !input.vertexCount || input.vertexCount > 65535 ||
            !input.triangleCount || input.vertexStride < 16 || input.vertexStride > 256 ||
            input.triangleCount > 65535 || !maxBytes ||
            !std::ranges::all_of(input.transform, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(input.bound, [](float v) { return std::isfinite(v); }) ||
            input.bound[3] <= 0) return false;
        const size_t vertexBytes = size_t(input.vertexCount) * input.vertexStride;
        const size_t indexCount = size_t(input.triangleCount) * 3;
        if (vertexBytes > maxBytes || indexCount * sizeof(uint16_t) > maxBytes - vertexBytes ||
            input.vertices.size() != vertexBytes || input.indices.size() != indexCount)
            return false;
        if (input.hasNormal && (input.normalOffset + 4 > input.vertexStride)) return false;
        if (input.hasUv && (input.uvOffset + 4 > input.vertexStride)) return false;
        if (input.hasTangent && (input.tangentOffset + 4 > input.vertexStride)) return false;
        for (uint32_t i = 0; i < input.vertexCount; ++i) {
            const auto* src = input.vertices.data() + size_t(i) * input.vertexStride;
            float xyz[3];
            std::memcpy(xyz, src, sizeof(xyz));
            if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) return false;
        }
        for (const auto index : input.indices) if (index >= input.vertexCount) return false;
        return true;
    }

    inline std::shared_ptr<const RuntimeGeometryPacket> CopyRuntimeGeometry(
        const RuntimeGeometryInput& input, size_t maxBytes = 512 * 1024)
    {
        if (!ValidRuntimeGeometryInput(input, maxBytes)) return {};
        auto packet = std::make_shared<RuntimeGeometryPacket>();
        packet->referenceKey = input.referenceKey;
        packet->shapeName = input.shapeName;
        packet->transform = input.transform;
        packet->bound = input.bound;
        packet->vertexCount = input.vertexCount;
        packet->vertexStride = input.vertexStride;
        packet->indexCount = uint32_t(input.indices.size());
        packet->normalOffset = input.normalOffset;
        packet->uvOffset = input.uvOffset;
        packet->tangentOffset = input.tangentOffset;
        packet->hasNormal = input.hasNormal;
        packet->hasUv = input.hasUv;
        packet->hasTangent = input.hasTangent;
        packet->captureFrame = input.frame;
        packet->vertices.assign(input.vertices.begin(), input.vertices.end());
        packet->indices.assign(input.indices.begin(), input.indices.end());
        return packet;
    }

    /** @brief Identities already present in the successfully published production TLAS.
     *  A runtime capture is unnecessary only when that precise reference/shape
     *  already has a built acceleration instance; missing or unresolved items
     *  remain eligible for runtime capture.
     */
    struct PreparedRuntimeCoverage
    {
        uint64_t sceneRevision = 0;
        std::unordered_set<std::string> completed;
        bool Contains(const std::string& reference, const std::string& shape) const
        {
            return completed.contains(reference + '\0' + shape);
        }
    };

    /** @brief Bounded immutable capture mailbox, scoped to the current scene.
     *
     * Exported packets own their storage and can safely outlive the render hook.
     * A model's cache key requires a unique reference/shape association.
     */
    class RuntimeGeometryMailbox
    {
    public:
        explicit RuntimeGeometryMailbox(size_t maxTotalBytes = 64ull * 1024 * 1024,
            size_t maxItems = 8192) : byteLimit(maxTotalBytes), itemLimit(maxItems) {}

        bool Contains(const std::string& reference, const std::string& shape) const
        {
            std::scoped_lock lock(mutex);
            return entries.contains(Key(reference, shape));
        }
        /** @brief Cheap admission before validating or copying borrowed renderer arrays.
         * Submit still enforces the limit under the same lock when the packet arrives. */
        bool CanCapture(const std::string& reference, const std::string& shape,
            uint64_t candidateBytes = 0) const
        {
            std::scoped_lock lock(mutex);
            return !entries.contains(Key(reference, shape)) && entries.size() < itemLimit &&
                candidateBytes <= byteLimit && bytes <= byteLimit - candidateBytes;
        }
        bool Submit(std::shared_ptr<const RuntimeGeometryPacket> packet)
        {
            if (!packet || packet->referenceKey.empty() || packet->shapeName.empty()) return false;
            std::scoped_lock lock(mutex);
            const auto key = Key(packet->referenceKey, packet->shapeName);
            if (entries.contains(key) || entries.size() >= itemLimit ||
                bytes > byteLimit || packet->Bytes() > byteLimit - bytes) return false;
            bytes += packet->Bytes();
            entries.emplace(key, std::move(packet));
            ++revision;
            return true;
        }
        /** @brief Discard captures that the published production TLAS already covers. */
        size_t RemoveCovered(const PreparedRuntimeCoverage& coverage)
        {
            std::scoped_lock lock(mutex);
            size_t removed = 0;
            for (auto item = entries.begin(); item != entries.end();) {
                if (!coverage.completed.contains(item->first)) { ++item; continue; }
                bytes -= item->second->Bytes();
                item = entries.erase(item);
                ++removed;
            }
            if (removed) ++revision;
            return removed;
        }
        std::vector<std::shared_ptr<const RuntimeGeometryPacket>> Export(size_t start, size_t limit) const
        {
            std::scoped_lock lock(mutex);
            std::vector<std::shared_ptr<const RuntimeGeometryPacket>> output;
            output.reserve(std::min(limit, entries.size()));
            // Stable export order makes background preparation deterministic.
            std::vector<std::string> keys;
            keys.reserve(entries.size());
            for (const auto& [key, value] : entries) keys.push_back(key);
            std::sort(keys.begin(), keys.end());
            for (size_t i = start; i < keys.size() && output.size() < limit; ++i)
                output.push_back(entries.at(keys[i]));
            return output;
        }
        void Reset()
        {
            std::scoped_lock lock(mutex);
            entries.clear();
            bytes = 0;
            ++revision;
        }
        size_t Count() const { std::scoped_lock lock(mutex); return entries.size(); }
        uint64_t Bytes() const { std::scoped_lock lock(mutex); return bytes; }
        uint64_t Revision() const { std::scoped_lock lock(mutex); return revision; }

    private:
        static std::string Key(const std::string& reference, const std::string& shape)
        {
            return reference + '\0' + shape;
        }
        mutable std::mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<const RuntimeGeometryPacket>> entries;
        size_t byteLimit = 0, itemLimit = 0;
        uint64_t revision = 0, bytes = 0;
    };
}

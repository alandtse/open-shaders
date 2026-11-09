#pragma once

#include "RuntimeGeometryCapture.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace DX12Native
{
    struct EncodedRuntimeGeometry
    {
        // Compatible with the RSGM/RSGA layouts accepted by PreparedScene.
        // These are generated directly from owned runtime mesh bytes, not
        // loaded from an offline atlas or written to its directory.
        std::vector<uint8_t> geometry;
        std::vector<uint8_t> attributes;
        uint32_t attributeStride = 0;
        bool tangentFrame = false;
    };

    inline float GeometryHalfToFloat(uint16_t bits)
    {
        const float sign = (bits & 0x8000u) ? -1.0f : 1.0f;
        const uint32_t exponent = (bits >> 10) & 31u;
        const uint32_t fraction = bits & 1023u;
        if (exponent == 31u) return std::numeric_limits<float>::quiet_NaN();
        if (exponent == 0u) return sign * std::ldexp(float(fraction), -24);
        return sign * std::ldexp(float(1024u + fraction), int(exponent) - 25);
    }
    inline std::array<float, 3> GeometryNormalize(std::array<float, 3> value)
    {
        const float len = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
        if (!std::isfinite(len) || len <= 1e-7f) return { 0, 0, 0 };
        for (auto& v : value) v /= len;
        return value;
    }
    inline std::array<float, 3> GeometryPackedNormal(const uint8_t* src)
    {
        return GeometryNormalize({ float(src[0]) / 127.5f - 1.0f,
            float(src[1]) / 127.5f - 1.0f, float(src[2]) / 127.5f - 1.0f });
    }
    inline void AppendGeometryBytes(std::vector<uint8_t>& target, const void* src, size_t len)
    {
        const auto* bytes = static_cast<const uint8_t*>(src);
        target.insert(target.end(), bytes, bytes + len);
    }
    inline void AppendGeometryU32(std::vector<uint8_t>& target, uint32_t value)
    {
        AppendGeometryBytes(target, &value, sizeof(value));
    }

    /** @brief Off-render-thread decode to the existing native geometry/shading ABI.
     *
     * Reject non-finite/malformed packets instead of guessing. Missing normals
     * or UVs still permit intersection geometry, but prohibit PBR shading
     * until a verified attribute stream is acquired.
     */
    inline std::optional<EncodedRuntimeGeometry> EncodeRuntimeGeometry(const RuntimeGeometryPacket& packet)
    {
        const uint64_t vertexBytes = uint64_t(packet.vertexCount) * packet.vertexStride;
        if (!packet.vertexCount || !packet.indexCount || packet.vertexStride < 16 ||
            packet.indexCount % 3 || packet.vertices.size() != vertexBytes ||
            packet.indices.size() != packet.indexCount || packet.Bytes() > 8ull * 1024 * 1024)
            return std::nullopt;
        EncodedRuntimeGeometry out;
        out.geometry.reserve(16 + size_t(packet.vertexCount) * 12 + size_t(packet.indexCount) * 4);
        AppendGeometryU32(out.geometry, 0x4d475352u);
        AppendGeometryU32(out.geometry, 1);
        AppendGeometryU32(out.geometry, packet.vertexCount);
        AppendGeometryU32(out.geometry, packet.indexCount);
        for (uint32_t i = 0; i < packet.vertexCount; ++i) {
            float xyz[3];
            std::memcpy(xyz, packet.vertices.data() + size_t(i) * packet.vertexStride, sizeof(xyz));
            if (!std::ranges::all_of(xyz, [](float value) { return std::isfinite(value); }))
                return std::nullopt;
            AppendGeometryBytes(out.geometry, xyz, sizeof(xyz));
        }
        for (const uint16_t index : packet.indices) {
            if (index >= packet.vertexCount) return std::nullopt;
            AppendGeometryU32(out.geometry, uint32_t(index));
        }
        if (!packet.hasNormal || !packet.hasUv ||
            packet.normalOffset + 4 > packet.vertexStride ||
            packet.uvOffset + 4 > packet.vertexStride)
            return out;
        // Tangent-space import requires all vertices to have independent,
        // non-degenerate normal and tangent directions.
        bool hasTangents = packet.hasTangent && packet.tangentOffset + 4 <= packet.vertexStride;
        if (hasTangents) {
            for (uint32_t i = 0; i < packet.vertexCount; ++i) {
                const auto* src = packet.vertices.data() + size_t(i) * packet.vertexStride;
                const auto n = GeometryPackedNormal(src + packet.normalOffset);
                const auto t = GeometryPackedNormal(src + packet.tangentOffset);
                const float crossLength =
                    std::hypot(n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                        n[0] * t[1] - n[1] * t[0]);
                if (!std::isfinite(crossLength) || crossLength <= 1e-3f) { hasTangents = false; break; }
            }
        }
        out.attributeStride = hasTangents ? 60 : 36;
        out.tangentFrame = hasTangents;
        out.attributes.reserve(16 + size_t(packet.vertexCount) * out.attributeStride);
        AppendGeometryU32(out.attributes, 0x41475352u);
        AppendGeometryU32(out.attributes, hasTangents ? 2u : 1u);
        AppendGeometryU32(out.attributes, packet.vertexCount);
        AppendGeometryU32(out.attributes, out.attributeStride);
        for (uint32_t i = 0; i < packet.vertexCount; ++i) {
            const auto* src = packet.vertices.data() + size_t(i) * packet.vertexStride;
            const auto normal = GeometryPackedNormal(src + packet.normalOffset);
            if (normal == std::array<float, 3>{ 0, 0, 0 }) return std::nullopt;
            uint16_t halfUV[2]{};
            std::memcpy(halfUV, src + packet.uvOffset, sizeof(halfUV));
            const std::array<float, 2> uv{ GeometryHalfToFloat(halfUV[0]), GeometryHalfToFloat(halfUV[1]) };
            if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return std::nullopt;
            const std::array<float, 4> color{ 1, 1, 1, 1 };
            AppendGeometryBytes(out.attributes, normal.data(), 12);
            AppendGeometryBytes(out.attributes, uv.data(), 8);
            AppendGeometryBytes(out.attributes, color.data(), 16);
            if (hasTangents) {
                const auto tangent = GeometryPackedNormal(src + packet.tangentOffset);
                auto bitangent = GeometryNormalize({
                    normal[1] * tangent[2] - normal[2] * tangent[1],
                    normal[2] * tangent[0] - normal[0] * tangent[2],
                    normal[0] * tangent[1] - normal[1] * tangent[0] });
                const float handedness = src[packet.tangentOffset + 3] >= 128u ? 1.0f : -1.0f;
                for (auto& v : bitangent) v *= handedness;
                AppendGeometryBytes(out.attributes, tangent.data(), 12);
                AppendGeometryBytes(out.attributes, bitangent.data(), 12);
            }
        }
        return out;
    }
}

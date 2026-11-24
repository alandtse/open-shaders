#pragma once

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace DX12Native
{
    /** @brief Admits a geometry-derived snow receiver without asserting that snow is present. */
    inline bool ReadPreparedSnowProjection(const nlohmann::json& a_row, uint32_t a_indexCount, bool a_opaque)
    {
        const auto found = a_row.find("projectedSnow");
        if (found == a_row.end())
            return false;
        if (!found->is_object())
            throw std::runtime_error("Prepared snow projection descriptor invalid");
        const auto& snow = *found;
        const auto basis = snow.at("basis").get<std::string>();
        const auto candidate = snow.at("candidate").get<bool>();
        const auto projected = snow.at("projectedArea").get<double>();
        const auto surface = snow.at("surfaceArea").get<double>();
        const auto up = snow.at("upwardTriangles").get<uint32_t>();
        const auto total = snow.at("totalTriangles").get<uint32_t>();
        if (!std::isfinite(projected) || !std::isfinite(surface) || projected < 0 ||
            surface < projected || total > a_indexCount / 3 || up > total ||
            candidate != (projected > 0 && up > 0) ||
            (basis != "world-up-face-projection-v1" &&
                !(basis.starts_with("unavailable:") && !candidate && projected == 0 && surface == 0 && total == 0)))
            throw std::runtime_error("Prepared snow projection metadata inconsistent");
        return a_opaque && candidate;
    }
}

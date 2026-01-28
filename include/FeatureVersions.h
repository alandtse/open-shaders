#pragma once
// This file overrides any generated copy in the build tree.
// Make sure your CMake includes ${CMAKE_SOURCE_DIR}/include before ${CMAKE_BINARY_DIR}.

#include <map>
#include <string_view>

namespace FeatureVersions
{
    using namespace std::literals::string_view_literals;

    inline const std::map<std::string_view, REL::Version> FEATURE_MINIMAL_VERSIONS{
        {"CloudShadows"sv,         {1,2,0}},
        {"DynamicCubemaps"sv,      {2,2,2}},
        {"ExtendedMaterials"sv,    {1,1,0}},
        {"ExtendedTranslucency"sv, {1,0,0}},
        {"GrassCollision"sv,       {3,0,2}}, // ←  CS 1.4.7 requires
        {"GrassLighting"sv,        {2,0,0}},
        {"HairSpecular"sv,         {1,0,3}},  // ←  CS 1.4.7 requires 
        {"ImageBasedLighting"sv,   {1,0,1}},
        {"InteriorSun"sv,          {1,0,0}},
        {"InverseSquareLighting"sv,{1,1,0}},
        {"LODBlending"sv,          {1,0,0}},
        {"LightLimitFix"sv,        {3,0,0}},
        {"LinearLighting"sv,       {1,0,0}},  // ←  CS 1.4.7
        {"PerformanceOverlay"sv,   {1,0,0}},
        {"RenderDoc"sv,            {1,0,0}},
        {"ScreenSpaceGI"sv,        {4,0,1}},  // CS 1.4.7 
        {"ScreenSpaceShadows"sv,   {2,0,0}}, // CS 1.4.6 
        {"SkySync"sv,              {1,0,0}},
        {"Skylighting"sv,          {1,2,3}}, // CS 1.4.7
        {"SubsurfaceScattering"sv, {3,0,1}},  //CS 1.4.7
        {"TerrainBlending"sv,      {1,0,1}},
        {"TerrainHelper"sv,        {1,0,0}},
        {"TerrainShadows"sv,       {1,0,0}},
        {"TerrainVariation"sv,     {1,0,1}},
        {"Upscaling"sv,            {1,1,2}},  // ←  CS 1.4.7
        {"VR"sv,                   {1,0,1}},
        {"VolumetricLighting"sv,   {1,0,0}},
        {"WaterEffects"sv,         {1,0,1}},
        {"WeatherPicker"sv,        {1,0,0}},
        {"WeatherEditor"sv,        {1,0,0}},  // ←  CS 1.4.7
        {"WetnessEffects"sv,       {3,0,0}},
    };
}

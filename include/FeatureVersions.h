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
        {"DynamicCubemaps"sv,      {2,2,3}},
        {"ExtendedMaterials"sv,    {1,1,0}},
        {"ExtendedTranslucency"sv, {1,0,0}},
        {"GrassCollision"sv,       {3,0,2}}, 
        {"GrassLighting"sv,        {2,0,0}},
        {"HairSpecular"sv,         {1,0,3}},  
        {"ImageBasedLighting"sv,   {1,0,1}},
        {"InteriorSun"sv,          {1,0,0}},
        {"InverseSquareLighting"sv,{1,1,0}},
        {"LODBlending"sv,          {1,0,0}},
        {"LightLimitFix"sv,        {3,1,6}},
        {"LinearLighting"sv,       {1,0,1}},  
        {"PerformanceOverlay"sv,   {1,0,0}},
        {"RenderDoc"sv,            {1,0,0}},
        {"ScreenSpaceGI"sv,        {4,1,7}},  
        {"ScreenSpaceShadows"sv,   {2,1,5}}, 
        {"SkySync"sv,              {1,0,0}},
        {"Skylighting"sv,          {1,3,5}}, 
        {"SubsurfaceScattering"sv, {3,1,5}},  
        {"TerrainBlending"sv,      {1,1,5}},
        {"TerrainHelper"sv,        {1,0,0}},
        {"TerrainShadows"sv,       {1,0,1}},
        {"TerrainVariation"sv,     {1,0,1}},
        {"UnifiedWater"sv,         {1,0,0}},
        {"Upscaling"sv,            {1,3,5}},  
        {"VR"sv,                   {1,1,5}},
        {"VolumetricLighting"sv,   {1,1,5}},
        {"WaterEffects"sv,         {1,1,0}},
        {"WeatherPicker"sv,        {1,0,0}},
        {"WeatherEditor"sv,        {1,0,0}},  
        {"WetnessEffects"sv,       {3,0,0}},
        {"Wetterness"sv,           {1,0,0}},
    };
}

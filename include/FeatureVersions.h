#pragma once
// This file overrides any generated copy in the build tree.

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
        {"GrassCollision"sv,       {2,0,0}}, // lowered minimal
        {"GrassLighting"sv,        {2,0,0}},
        {"HairSpecular"sv,         {1,0,1}},
        {"ImageBasedLighting"sv,   {1,0,1}},
        {"InteriorSun"sv,          {1,0,0}},
        {"InverseSquareLighting"sv,{1,1,0}},
        {"LODBlending"sv,          {1,0,0}},
        {"LightLimitFix"sv,        {3,0,0}},
        {"PerformanceOverlay"sv,   {1,0,0}},
        {"RenderDoc"sv,            {1,0,0}},
        {"ScreenSpaceGI"sv,        {3,0,0}}, 
        {"ScreenSpaceShadows"sv,   {1,2,1}}, // lower minimal for best SSS in VR 
        {"SkySync"sv,              {1,0,0}},
        {"Skylighting"sv,          {1,2,2}}, 
        {"SubsurfaceScattering"sv, {3,0,0}},
        {"TerrainBlending"sv,      {1,0,1}},
        {"TerrainHelper"sv,        {1,0,0}},
        {"TerrainShadows"sv,       {1,0,0}},
        {"TerrainVariation"sv,     {1,0,1}},
        {"Upscaling"sv,            {1,1,1}},
        {"VR"sv,                   {1,0,1}},
        {"VolumetricLighting"sv,   {1,0,0}},
        {"WaterEffects"sv,         {1,0,1}},
        {"WeatherPicker"sv,        {1,0,0}},
        {"WetnessEffects"sv,       {3,0,0}},
    };
}

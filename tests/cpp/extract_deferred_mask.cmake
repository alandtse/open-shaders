function(extract_deferred_mask_region source start end output)
    string(FIND "${source}" "${start}" region_start)
    if(region_start EQUAL -1)
        message(FATAL_ERROR "Deferred mask test cannot find ${start}")
    endif()
    string(SUBSTRING "${source}" ${region_start} -1 remaining)
    string(FIND "${remaining}" "${end}" region_end)
    if(region_end EQUAL -1)
        message(FATAL_ERROR "Deferred mask test cannot find ${end}")
    endif()
    string(SUBSTRING "${remaining}" 0 ${region_end} extracted)
    file(WRITE "${deferred_mask_test_directory}/${output}" "${extracted}")
endfunction()

get_filename_component(deferred_mask_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${deferred_mask_source_root}/src/Deferred.cpp"
    "${deferred_mask_source_root}/src/Hooks.cpp"
    "${deferred_mask_source_root}/src/Utils/D3D.cpp"
    "${deferred_mask_source_root}/src/ShaderCache.h"
    "${deferred_mask_source_root}/src/ShaderCache.cpp")
set(deferred_mask_test_directory "${CMAKE_CURRENT_BINARY_DIR}/deferred_mask")
file(MAKE_DIRECTORY "${deferred_mask_test_directory}")
file(READ "${deferred_mask_source_root}/src/Deferred.cpp" deferred_source)
extract_deferred_mask_region("${deferred_source}" "static void ReleaseRenderTargetSlot"
    "void Deferred::SetupResources" "deferred_target_under_test.h")
extract_deferred_mask_region("${deferred_source}" "bool Deferred::MaterialCategoriesRequested"
    "void Deferred::StartDeferred" "deferred_mask_under_test.h")
file(READ "${deferred_mask_source_root}/src/Utils/D3D.cpp" naming_source)
extract_deferred_mask_region("${naming_source}" "	GUID WKPDID_D3DDebugObjectNameT"
    "	bool GetTexture2DDesc" "deferred_names_under_test.h")
file(READ "${deferred_mask_source_root}/src/ShaderCache.h" shader_cache_header)
extract_deferred_mask_region("${shader_cache_header}" "		static constexpr uint32_t GetMaterialCategoryFlag("
    "
		RE::BSGraphics::PixelShader* GetPixelShader(" "deferred_flag_under_test.h")
file(READ "${deferred_mask_source_root}/src/Hooks.cpp" hooks_source)
extract_deferred_mask_region("${hooks_source}" "			if (!state->settingCustomShader && a_pixelShader && state->currentShader &&"
    "			*globals::game::currentPixelShader = a_pixelShader;" "deferred_fallback_under_test.h")
file(READ "${deferred_mask_source_root}/src/ShaderCache.cpp" shader_cache_source)
extract_deferred_mask_region("${shader_cache_source}" "	void ShaderCache::PrewarmDeferredPixelShaders("
    "	RE::BSGraphics::PixelShader* ShaderCache::GetPixelShader(" "deferred_prewarm_under_test.h")

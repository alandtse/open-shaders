# Reject AIO archives missing essential native DX12 binaries or runtime payloads.
if(NOT DEFINED AIO_STAGE_DIR OR NOT IS_DIRECTORY "${AIO_STAGE_DIR}")
    message(FATAL_ERROR "AIO verification requires an existing AIO_STAGE_DIR")
endif()
set(_required
    "SKSE/Plugins/CommunityShaders.dll"
    "SKSE/Plugins/CommunityShaders/DX12Resources/bin/DX12ResourcesPrep.exe"
    "Shaders/Features/DX12.ini"
    "Shaders/Features/DX12Resources.ini"
    "Shaders/Features/DX12NativeScene.ini"
    "Shaders/DX12NativeScene/ShadowOffload.hlsl6"
    "Shaders/DX12NativeScene/ShadowOffload.dxil"
    "Shaders/DX12NativeScene/ShadowRTQuality.dxil"
    "Shaders/DX12NativeScene/ShadowRTPerformance.dxil"
    "Shaders/NativeComposite/CompositeOffload.hlsl6"
    "Shaders/NativeComposite/CompositeOffload.dxil"
    "Shaders/NativeComposite/CompositeRTQuality.dxil"
    "Shaders/NativeComposite/CompositeRTPerformance.dxil"
    "Shaders/Upscaling/FidelityFX/amd_fidelityfx_framegeneration_dx12.dll"
    "Shaders/Upscaling/FidelityFX/amd_fidelityfx_loader_dx12.dll"
    "Shaders/Upscaling/FidelityFX/amd_fidelityfx_upscaler_dx12.dll"
    "Shaders/Upscaling/Streamline/nvngx_dlss.dll"
    "Shaders/Upscaling/Streamline/sl.common.dll"
    "Shaders/Upscaling/Streamline/sl.dlss.dll"
    "Shaders/Upscaling/Streamline/sl.interposer.dll"
    "Shaders/Upscaling/Streamline/sl.pcl.dll"
    "Shaders/Upscaling/Streamline/sl.reflex.dll"
    "Shaders/Upscaling/StreamlineDX12/nvngx_dlss.dll"
    "Shaders/Upscaling/StreamlineDX12/nvngx_dlssg.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.common.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.dlss.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.dlss_g.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.interposer.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.pcl.dll"
    "Shaders/Upscaling/StreamlineDX12/sl.reflex.dll"
)
foreach(_path IN LISTS _required)
    set(_full "${AIO_STAGE_DIR}/${_path}")
    if(NOT EXISTS "${_full}" OR IS_DIRECTORY "${_full}")
        message(FATAL_ERROR "AIO payload missing: ${_path}")
    endif()
    file(SIZE "${_full}" _size)
    if(_size EQUAL 0)
        message(FATAL_ERROR "AIO payload is empty: ${_path}")
    endif()
endforeach()
list(LENGTH _required _count)
message(
    STATUS
    "Verified ${_count} required AIO payload files (including native DXIL)"
)

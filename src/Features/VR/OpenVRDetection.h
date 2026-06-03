#pragma once
#include <cstdint>
#include <string>

namespace VRDetection
{
	enum class RuntimeType
	{
		Unknown,
		SteamVR,
		OpenComposite
	};

	struct OpenVRDetectionResult
	{
		bool isAvailable = false;
		bool isCompatible = false;

		// Interface probing results
		bool hasOverlayInterface = false;
		bool hasSystemInterface = false;
		bool hasCompositorInterface = false;

		// File-based info
		std::string dllPath;
		std::string version;
		uint64_t fileSize = 0;
		std::string modificationTime;

		// Detection metadata
		RuntimeType runtimeType = RuntimeType::Unknown;
		bool probingSucceeded = false;
	};

	// Whether OpenComposite is running its own DLSS/FSR/DLAA upscaling (so a
	// feature like Upscaling can stand down). settingName/configPath name the
	// trigger for UI/logs.
	struct OpenCompositeUpscalingState
	{
		bool active = false;
		std::string settingName;
		std::string configPath;
	};

	// Runtime interface probing via VR_IsInterfaceVersionValid
	bool ProbeRuntimeInterfaces(OpenVRDetectionResult& result);

	// Gather DLL metadata (path, version, size, timestamp)
	void GatherDLLInfo(OpenVRDetectionResult& result);

	// Detect runtime type (SteamVR vs OpenComposite)
	RuntimeType DetectRuntimeType(const std::string& dllPath, const std::string& version, uint64_t fileSize);

	// Full detection via interface probing
	OpenVRDetectionResult Detect();

	// Probe opencomposite.ini / opencomposite_ext.ini (by the loaded
	// openvr_api.dll and in the CWD) for an enabled upscaler. Inactive when none
	// found. Does not check whether VR is active — callers gate on that.
	OpenCompositeUpscalingState DetectOpenCompositeUpscaling();

	const char* RuntimeTypeToString(RuntimeType type);
}

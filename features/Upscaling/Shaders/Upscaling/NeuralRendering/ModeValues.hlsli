#ifndef __NR_MODE_VALUES_HLSLI__
#define __NR_MODE_VALUES_HLSLI__

// The numbers Diagnostics.h's enums and MaterialMap.h's Mode declare, which their static_asserts pin.
namespace NR
{
	static const uint kConversionRaw = 0;
	static const uint kConversionLinearToSRGB = 1;
	static const uint kConversionSRGBToLinear = 2;
	static const uint kConversionLinearToGamma22 = 3;
	static const uint kConversionGamma22ToLinear = 4;
	static const uint kConversionSRGBToGamma22 = 5;
	static const uint kConversionSkyrimGammaToGamma22 = 6;
	static const uint kConversionLinearToSkyrimGamma = 7;
	static const uint kConversionLinearToLinear = 8;
	static const uint kConversionProduction = 9;

	static const uint kExposureProduction = 0;
	static const uint kExposureIgnore = 1;
	static const uint kExposureForceOne = 2;
	static const uint kExposureGame = 3;
	static const uint kExposureManual = 4;
	static const uint kExposureDeExposeReExpose = 5;
	static const uint kExposurePassOnly = 6;
	static const uint kExposureDoNotPass = 7;

	static const uint kCompositeProduction = 0;
	static const uint kCompositeReplacement = 1;
	static const uint kCompositeMaskedLerp = 2;
	static const uint kCompositeHalfMaskedLerp = 3;
	static const uint kCompositePreserveLuminance = 4;
	static const uint kCompositePreserveRatio = 5;
	static const uint kCompositeResidual = 6;
	static const uint kCompositeRatio = 7;

	static const uint kMaskAutomatic = 0;
	static const uint kMaskForceZero = 1;
	static const uint kMaskForceOne = 2;

	static const uint kVisualNone = 0;
	static const uint kVisualInput = 1;
	static const uint kVisualOutput = 2;
	static const uint kVisualDifference = 3;
	static const uint kVisualRatio = 4;
	static const uint kVisualOriginal = 5;
	static const uint kVisualPostComposite = 6;
	static const uint kVisualLuminanceDifference = 7;
	static const uint kVisualChromaDifference = 8;
	static const uint kVisualMask = 9;
	static const uint kVisualExposure = 10;
	static const uint kVisualSplitOriginalOutput = 11;
	static const uint kVisualSplitOriginalComposite = 12;
	static const uint kVisualSplitInputOutput = 13;
	static const uint kVisualSplitPrePost = 14;
	static const uint kVisualLogRatio = 15;
	static const uint kVisualToneDelta = 16;
	static const uint kVisualToneLow = 17;
	static const uint kVisualToneHigh = 18;
	static const uint kVisualToneLowGain = 19;
	static const uint kVisualFinalLuminanceRatio = 20;
	static const uint kVisualCategory = 21;

	static const uint kMaterialMapCategory = 0;
	static const uint kMaterialMapStrength = 1;
}

#endif  // __NR_MODE_VALUES_HLSLI__

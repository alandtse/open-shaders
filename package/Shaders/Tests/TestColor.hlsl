// HLSL Unit Tests for Common/Color.hlsli
#include "/Shaders/Common/Color.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags color, luminance
[numthreads(1, 1, 1)] void TestRGBToLuminance() {
	// Test 1: White should produce luminance of 1.0
	float3 white = float3(1.0, 1.0, 1.0);
	float whiteLum = Color::RGBToLuminance(white);
	ASSERT(IsTrue, abs(whiteLum - 1.0f) < 0.001f);

	// Test 2: Black should produce luminance of 0.0
	float3 black = float3(0.0, 0.0, 0.0);
	float blackLum = Color::RGBToLuminance(black);
	ASSERT(AreEqual, blackLum, 0.0f);

	// Test 3: Green contributes most to luminance (Rec. 709: R=0.2126, G=0.7152, B=0.0722)
	float3 red = float3(1.0, 0.0, 0.0);
	float3 green = float3(0.0, 1.0, 0.0);
	float3 blue = float3(0.0, 0.0, 1.0);

	float redLum = Color::RGBToLuminance(red);
	float greenLum = Color::RGBToLuminance(green);
	float blueLum = Color::RGBToLuminance(blue);

	// Verify ordering: green > red > blue
	ASSERT(IsTrue, greenLum > redLum);
	ASSERT(IsTrue, greenLum > blueLum);
	ASSERT(IsTrue, redLum > blueLum);

	// Test 4: Verify expected coefficients (approximate)
	ASSERT(IsTrue, abs(redLum - 0.2126f) < 0.01f);
	ASSERT(IsTrue, abs(greenLum - 0.7152f) < 0.01f);
	ASSERT(IsTrue, abs(blueLum - 0.0722f) < 0.01f);

	// Test 5: Sum of components should equal whole
	float3 mixed = float3(0.3f, 0.5f, 0.2f);
	float mixedLum = Color::RGBToLuminance(mixed);
	ASSERT(IsTrue, mixedLum >= 0.0f);
	ASSERT(IsTrue, mixedLum <= 1.0f);
}

	/// @tags color, colorspace
	[numthreads(1, 1, 1)] void TestRGBYCoCgRoundtrip()
{
	// Test various colors roundtrip correctly
	float3 testColors[5] = {
		float3(0.5, 0.5, 0.5),  // Gray
		float3(1.0, 0.0, 0.0),  // Red
		float3(0.0, 1.0, 0.0),  // Green
		float3(0.0, 0.0, 1.0),  // Blue
		float3(0.8, 0.3, 0.5)   // Random color
	};

	for (int i = 0; i < 5; i++) {
		float3 original = testColors[i];
		float3 ycocg = Color::RGBToYCoCg(original);
		float3 roundtrip = Color::YCoCgToRGB(ycocg);

		// Check each component is close enough (within small epsilon)
		ASSERT(IsTrue, abs(roundtrip.r - original.r) < 0.001f);
		ASSERT(IsTrue, abs(roundtrip.g - original.g) < 0.001f);
		ASSERT(IsTrue, abs(roundtrip.b - original.b) < 0.001f);
	}
}

/// @tags color
[numthreads(1, 1, 1)] void TestSaturation() {
	float3 testColor = float3(0.8, 0.3, 0.5);

	// Test desaturation (0.0) produces gray
	float3 desaturated = Color::Saturation(testColor, 0.0);
	float gray = desaturated.r;
	ASSERT(IsTrue, abs(desaturated.r - gray) < 0.001f);
	ASSERT(IsTrue, abs(desaturated.g - gray) < 0.001f);
	ASSERT(IsTrue, abs(desaturated.b - gray) < 0.001f);

	// Test full saturation (1.0) preserves color
	float3 fullSat = Color::Saturation(testColor, 1.0);
	ASSERT(IsTrue, abs(fullSat.r - testColor.r) < 0.001f);
	ASSERT(IsTrue, abs(fullSat.g - testColor.g) < 0.001f);
	ASSERT(IsTrue, abs(fullSat.b - testColor.b) < 0.001f);

	// Test over-saturation doesn't produce negative values
	float3 overSat = Color::Saturation(testColor, 2.0);
	ASSERT(IsTrue, overSat.r >= 0.0f);
	ASSERT(IsTrue, overSat.g >= 0.0f);
	ASSERT(IsTrue, overSat.b >= 0.0f);
}

	/// @tags color, gamma, colorspace
	[numthreads(1, 1, 1)] void TestGammaConversionRoundtrip()
{
	float3 testColors[3] = {
		float3(0.5, 0.5, 0.5),
		float3(0.2, 0.7, 0.3),
		float3(0.9, 0.1, 0.6)
	};

	for (int i = 0; i < 3; i++) {
		float3 original = testColors[i];

		// Test Gamma -> Linear -> Gamma
		float3 linearColor = Color::SkyrimGammaToLinear(original);
		float3 backToGamma = Color::LinearToSkyrimGamma(linearColor);

		ASSERT(IsTrue, abs(backToGamma.r - original.r) < 0.01f);
		ASSERT(IsTrue, abs(backToGamma.g - original.g) < 0.01f);
		ASSERT(IsTrue, abs(backToGamma.b - original.b) < 0.01f);

		// Test TrueLinear roundtrip
		float3 trueLinearColor = Color::SrgbToLinear(original);
		float3 backToGamma2 = Color::LinearToSrgb(trueLinearColor);

		ASSERT(IsTrue, abs(backToGamma2.r - original.r) < 0.01f);
		ASSERT(IsTrue, abs(backToGamma2.g - original.g) < 0.01f);
		ASSERT(IsTrue, abs(backToGamma2.b - original.b) < 0.01f);
	}
}

/// @tags color, colorspace, acescg, gamma
[numthreads(1, 1, 1)] void TestAP1AuthoredTransferRoundtrip() {
	const float3 testColors[4] = {
		float3(1.0f, 0.0f, 0.0f),
		float3(0.0f, 1.0f, 0.0f),
		float3(0.0f, 0.0f, 1.0f),
		float3(4.0f, 0.2f, 1.5f)
	};

	for (int i = 0; i < 4; i++) {
		const float3 linearSrgb = AP1TosRGB(testColors[i]);
		const float3 authored = Color::EncodeSRGB(linearSrgb);
		const float3 decoded = Color::DecodeSRGB(authored);
		const float3 roundtrip = sRGBToAP1(decoded);

		ASSERT(IsTrue, all(abs(roundtrip - testColors[i]) < 0.0001f));
		ASSERT(IsTrue, all(authored * linearSrgb >= 0.0f));
	}
}

	/// @tags color, luminance
	[numthreads(1, 1, 1)] void TestRGBToLuminanceVariants()
{
	float3 testColor = float3(0.6, 0.4, 0.3);

	float lum1 = Color::RGBToLuminance(testColor);
	float lum2 = Color::RGBToLuminanceAlternative(testColor);
	float lum3 = Color::RGBToLuminance2(testColor);

	ASSERT(IsTrue, lum1 >= 0.0f && lum1 <= 1.0f);
	ASSERT(IsTrue, lum2 >= 0.0f && lum2 <= 1.0f);
	ASSERT(IsTrue, lum3 >= 0.0f && lum3 <= 1.0f);

	ASSERT(IsTrue, abs(lum1 - lum2) < 0.2f);
	ASSERT(IsTrue, abs(lum1 - lum3) < 0.2f);
}

/// @tags color, lighting
[numthreads(1, 1, 1)] void TestDiffuseCalibrationEndpoints() {
	const float gammas[4] = { 0.1f, 1.0f, 2.2f, 4.0f };
	const float whites[3] = { 0.001f, 0.5f, 1.0f };
	const float midpoints[3] = { 0.0001f, 0.5f, 0.999f };
	const float tolerance = 0.00001f;
	for (int g = 0; g < 4; g++) {
		for (int w = 0; w < 3; w++) {
			for (int m = 0; m < 3; m++) {
				float reflectance = min(midpoints[m], whites[w] * 0.999f);
				float midpoint = pow(0.5f, gammas[g]);
				float curve = reflectance * (1.0f - midpoint) / (midpoint * (whites[w] - reflectance));
				float3 result = Color::CalibrateDiffuse(float3(0.0f, 0.5f, 1.0f), gammas[g], curve, whites[w]);
				ASSERT(IsTrue, all(isfinite(result)));
				ASSERT(IsTrue, abs(result.r) < tolerance);
				ASSERT(IsTrue, abs(result.g - reflectance) < tolerance);
				ASSERT(IsTrue, abs(result.b - whites[w]) < tolerance);
			}
		}
	}
}

	/// @tags color, lighting
	[numthreads(1, 1, 1)] void TestDiffuseCalibrationBounds()
{
	const float curve = 3.59479342f;
	const float tolerance = 0.00001f;
	float previous = 0.0f;
	for (int i = 0; i <= 64; i++) {
		float encoded = float(i) / 64.0f;
		float3 result = Color::CalibrateDiffuse(encoded.xxx, 2.2f, curve, 1.0f);
		ASSERT(IsTrue, all(result >= 0.0f) && all(result <= 1.0f + tolerance));
		ASSERT(IsTrue, result.r >= previous - tolerance);
		ASSERT(IsTrue, all(abs(result - result.r) < tolerance));
		previous = result.r;
	}
	float3 outOfRange = Color::CalibrateDiffuse(float3(-0.1f, 1.0f, 2.0f), 2.2f, curve, 1.0f);
	ASSERT(IsTrue, all(abs(outOfRange - float3(0.0f, 1.0f, 1.0f)) < tolerance));
	float3 colored = Color::CalibrateDiffuse(float3(0.2f, 0.5f, 0.8f), 2.2f, curve, 1.0f);
	ASSERT(IsTrue, all(abs(colored - float3(0.096926f, 0.5f, 0.850113f)) < tolerance));
}

/// @tags color, lighting
[numthreads(1, 1, 1)] void TestDiffuseSaturationCalibration() {
	const float3 colors[6] = {
		float3(0.2f, 0.5f, 0.8f), float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
		float3(0.0f, 0.0f, 1.0f), 0.5f.xxx, 1.0f.xxx
	};
	const float saturations[4] = { 0.0f, 0.25f, 0.75f, 1.0f };
	const float curve = 3.59479342f;
	const float tolerance = 0.00001f;
	for (int c = 0; c < 6; c++) {
		float3 reference = Color::CalibrateDiffuse(colors[c], 2.2f, curve, 1.0f);
		float luminance = Color::RGBToLuminance(reference, Color::kRec709LuminanceWeights);
		for (int s = 0; s < 4; s++) {
			float3 result = Color::CalibrateDiffuse(colors[c], 2.2f, curve, 1.0f, saturations[s]);
			ASSERT(IsTrue, abs(Color::RGBToLuminance(result, Color::kRec709LuminanceWeights) - luminance) < tolerance);
			ASSERT(IsTrue, all(result >= 0.0f) && all(result <= 1.0f + tolerance));
			ASSERT(IsTrue, all(abs((result - luminance) - saturations[s] * (reference - luminance)) < tolerance));
			if (saturations[s] == 1.0f)
				ASSERT(IsTrue, all(asuint(result) == asuint(reference)));
			if (saturations[s] == 0.0f)
				ASSERT(IsTrue, all(abs(result - luminance) < tolerance));
			if (c >= 4)
				ASSERT(IsTrue, all(abs(result - reference) < tolerance));
		}
	}
}

	/// @tags color, lighting, gamma
	[numthreads(1, 1, 1)] void TestConversionSaturation()
{
	const float3 colors[4] = {
		float3(4.0f, 0.2f, 1.5f), float3(-0.05f, 0.6f, 0.2f), 0.5f.xxx, 3.0f.xxx
	};
	const float saturations[4] = { 0.0f, 0.25f, 0.75f, 1.0f };
	const float tolerance = 0.00001f;
	for (int c = 0; c < 4; c++) {
		float luminance = Color::RGBToLuminance(colors[c], Color::kRec709LuminanceWeights);
		for (int s = 0; s < 4; s++) {
			float3 result = Color::ApplyConversionSaturation(colors[c], saturations[s]);
			ASSERT(IsTrue, abs(Color::RGBToLuminance(result, Color::kRec709LuminanceWeights) - luminance) < tolerance);
			ASSERT(IsTrue, all(result >= min(colors[c].r, min(colors[c].g, colors[c].b)) - tolerance));
			ASSERT(IsTrue, all(result <= max(colors[c].r, max(colors[c].g, colors[c].b)) + tolerance));
			if (saturations[s] == 1.0f)
				ASSERT(IsTrue, all(asuint(result) == asuint(colors[c])));
			if (saturations[s] == 0.0f)
				ASSERT(IsTrue, all(abs(result - luminance) < tolerance));
			if (c >= 2)
				ASSERT(IsTrue, all(abs(result - colors[c]) < tolerance));
			float3 transported = Color::DecodeSRGB(Color::EncodeSRGB(result));
			ASSERT(IsTrue, all(abs(transported - result) < tolerance));
			float3 gamutRoundtrip = AP1TosRGB(sRGBToAP1(result));
			ASSERT(IsTrue, all(abs(gamutRoundtrip - result) < tolerance));
		}
	}
}

/// @tags color, lighting
[numthreads(1, 1, 1)] void TestDiffuseAndLight() {
	float3 color = float3(0.5, 0.3, 0.7);

	float3 diffuse = Color::Diffuse(color);
	float3 light = Color::Light(color);

	ASSERT(IsTrue, diffuse.r >= 0.0f && diffuse.g >= 0.0f && diffuse.b >= 0.0f);
	ASSERT(IsTrue, light.r >= 0.0f && light.g >= 0.0f && light.b >= 0.0f);

	float3 black = float3(0.0, 0.0, 0.0);
	float3 diffuseBlack = Color::Diffuse(black);
	ASSERT(IsTrue, diffuseBlack.r >= 0.0f && diffuseBlack.g >= 0.0f && diffuseBlack.b >= 0.0f);
}

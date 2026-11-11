#include "/Shaders/Common/NeuralRenderingMaterial.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestMaterialSamplesStayInsideTheirEye() {
	const uint2 size = uint2(7, 5);
	ASSERT(IsTrue, all(NeuralRenderingMaterial::SourcePixel(int2(-4, -4), size, 0) == int2(0, 0)));
	ASSERT(IsTrue, all(NeuralRenderingMaterial::SourcePixel(int2(10, 8), size, 0) == int2(6, 4)));
	ASSERT(IsTrue, all(NeuralRenderingMaterial::SourcePixel(int2(-4, -4), size, 7) == int2(7, 0)));
	ASSERT(IsTrue, all(NeuralRenderingMaterial::SourcePixel(int2(10, 8), size, 7) == int2(13, 4)));
	ASSERT(IsTrue, all(NeuralRenderingMaterial::SourcePixel(int2(3, 2), size, 7) == int2(10, 2)));
}

	/// @tags neural-rendering
	[numthreads(1, 1, 1)] void TestMaterialSelectionMatchesR8Protection()
{
	ASSERT(AreEqual, NeuralRenderingMaterial::Selection(0.0), 1.0);
	ASSERT(AreEqual, NeuralRenderingMaterial::Selection(1.0), 0.0);
	ASSERT(AreEqual, NeuralRenderingMaterial::Selection(-1.0), 1.0);
	ASSERT(AreEqual, NeuralRenderingMaterial::Selection(2.0), 0.0);
	for (uint value = 0; value <= 255; ++value) {
		const float protection = float(value) / 255.0;
		ASSERT(IsTrue, abs(NeuralRenderingMaterial::Selection(protection) - (1.0 - protection)) < 1e-6);
	}
}

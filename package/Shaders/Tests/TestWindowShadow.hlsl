#include "/Shaders/Common/WindowShadow.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

/// @tags lighting, shadows
[numthreads(1, 1, 1)] void TestWindowShadowOverlappingPanes() {
	float front = WindowShadow::Encode(0.25, false);
	float back = WindowShadow::Encode(0.75, true);
	float entry = max(front, back);
	ASSERT(IsTrue, WindowShadow::Open(entry));
	ASSERT(IsFalse, WindowShadow::ReachesPane(0.1, entry));
	ASSERT(IsFalse, WindowShadow::ReachesPane(0.5, entry));
	ASSERT(IsTrue, WindowShadow::ReachesPane(0.875, entry));
	ASSERT(IsTrue, entry == max(back, front));
	ASSERT(IsFalse, WindowShadow::Open(max(WindowShadow::Encode(0.25, true), WindowShadow::Encode(0.75, false))));
}

	/// @tags lighting, shadows
	[numthreads(1, 1, 1)] void TestWindowShadowFootprintAndDepthRange()
{
	ASSERT(IsFalse, WindowShadow::Open(0.0));
	ASSERT(IsTrue, WindowShadow::ReachesPane(0.0, 0.0));
	ASSERT(IsTrue, WindowShadow::Open(WindowShadow::Encode(0.0, true)));
	ASSERT(IsFalse, WindowShadow::Open(WindowShadow::Encode(1.0, false)));
	ASSERT(IsTrue, WindowShadow::ReachesPane(0.6173, WindowShadow::Encode(0.6173, true)));
	ASSERT(IsFalse, WindowShadow::ReachesPane(0.999, WindowShadow::Encode(1.0, true)));
	float entry = max(WindowShadow::Encode(-0.4, true), WindowShadow::Encode(-0.1, false));
	ASSERT(IsFalse, WindowShadow::Open(entry));
	ASSERT(IsTrue, WindowShadow::ReachesPane(-0.1, entry));
	ASSERT(IsTrue, WindowShadow::ReachesPane(0.0, entry));
	ASSERT(IsFalse, WindowShadow::ReachesPane(-0.2, entry));
}

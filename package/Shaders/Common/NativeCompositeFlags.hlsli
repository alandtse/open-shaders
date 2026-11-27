#ifndef NATIVE_COMPOSITE_FLAGS_HLSLI
#define NATIVE_COMPOSITE_FLAGS_HLSLI

// Bits of the composite Flags constant; DX12NativeComposite.cpp writes the same values.
static const uint kCompositeLegacyTransfer = 1u;
static const uint kCompositeDebugView = 2u;

// Debug view colour for a scene receiver with no matching mask sample.
static const float4 kCompositeDebugUnmatched = float4(1.0f, 0.0f, 1.0f, 1.0f);

#endif

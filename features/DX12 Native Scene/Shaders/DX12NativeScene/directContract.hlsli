#ifndef REDSTONE_DIRECT_CONTRACT_HLSLI
#define REDSTONE_DIRECT_CONTRACT_HLSLI
#include "Common/PointLightFlags.hlsli"
#include "ReceiverRoom.hlsli"

static const uint DirectDiffuseView = 10;
static const uint DirectSpecularView = 11;
static const uint DirectCombinedView = 12;
static const float DirectRadianceTag = 4;
static const uint MaximumDirectLights = 64;  // Must match DX12Native::kMaximumNativePointLights.
static const uint LightSnapshotTexels = 8;
static const float UnavailableTag = 5;
static const uint MissingReceiver = 1;
static const uint MissedGeometry = 2;
static const uint UnsupportedMaterial = 3;
static const uint MissingLight = 4;
static const uint MismatchedDepth = 5;
static const uint InvalidSurface = 6;
static const uint MissingRoom = 7;
static const uint ExcludedRoom = 8;
static const uint LightReady = 0;
static const uint LightNoTable = 1;
static const uint LightInvalidTable = 2;
static const uint LightUnresolvedReference = 3;
static const uint LightNoMatch = 4;
static const uint LightAmbiguous = 5;
static const uint LightUnsupported = 6;
static const uint LightInvalidParameters = 7;
static const uint LightInvalidRadiance = 8;

float4 Unavailable(uint reason)
{
	return float4(reason, 0, 0, UnavailableTag);
}

float4 UnavailableRoom(float receiverCode)
{
	return float4(MissingRoom, receiverCode, 0, UnavailableTag);
}

float4 UnavailableLight(float4 status)
{
	return float4(MissingLight, status.y, status.z, UnavailableTag);
}
#endif

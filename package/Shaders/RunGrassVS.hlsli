
#ifdef GRASS_COLLISION
#	include "GrassCollision\\GrassCollision.hlsli"
#endif  // GRASS_COLLISION

cbuffer cb7 : register(b7)
{
	float4 cb7[1];
}

cbuffer cb8 : register(b8)
{
	float4 cb8[240];
}

// Calculate wind displacement for a grass vertex
float3 CalculateWindDisplacement(VS_INPUT input, float windTimer)
{
	float windAngle = 0.4 * ((input.InstanceData1.x + input.InstanceData1.y) * -0.0078125 + windTimer);
	float windAngleSin, windAngleCos;
	sincos(windAngle, windAngleSin, windAngleCos);

	float windTmp3 = 0.2 * cos(Math::PI * windAngleCos);
	float windTmp1 = sin(Math::PI * windAngleSin);
	float windTmp2 = sin(Math::TAU * windAngleSin);
	float windPower = WindVector.z * (((windTmp1 + windTmp2) * 0.3 + windTmp3) *
										 (0.5 * (input.Color.w * input.Color.w)));

	return float3(WindVector.xy, 0) * windPower;
}

#ifdef GRASS_LIGHTING
float4 GetMSPosition(VS_INPUT input, float3x3 world3x3)
#else
float4 GetMSPosition(VS_INPUT input)
#endif
{
	float3 inputPosition = input.Position.xyz * (input.InstanceData4.yyy * ScaleMask.xyz + float3(1, 1, 1));

#ifdef GRASS_LIGHTING
	float3 transformedPosition = mul(world3x3, inputPosition);
	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + transformedPosition;
#else
	float3 instancePosition;
	instancePosition.z = dot(
		float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w), inputPosition);
	instancePosition.x = dot(input.InstanceData2.xyz, inputPosition);
	instancePosition.y = dot(input.InstanceData3.xyz, inputPosition);

	float4 msPosition;
	msPosition.xyz = input.InstanceData1.xyz + instancePosition;
#endif
	msPosition.w = 1;

	return msPosition;
}

#ifdef GRASS_LIGHTING
VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif  // VR
	);
	float3x3 world3x3 = float3x3(input.InstanceData2.xyz, input.InstanceData3.xyz, float3(input.InstanceData4.x, input.InstanceData2.w, input.InstanceData3.w));

	float4 msPosition = GetMSPosition(input, world3x3);

	float3 windDisplacement = CalculateWindDisplacement(input, WindTimer);
	float3 previousWindDisplacement = CalculateWindDisplacement(input, PreviousWindTimer);

#	ifdef GRASS_COLLISION
	float3 displacement, previousDisplacement;
	GrassCollision::GetDisplacedPosition(input, msPosition.xyz, displacement, previousDisplacement);
	msPosition.xyz += displacement;
#	endif  // GRASS_COLLISION

	msPosition.xyz += windDisplacement;

	float4 projSpacePosition = mul(WorldViewProj[eyeIndex], msPosition);
#	if !defined(VR)
	vsout.HPosition = projSpacePosition;
#	endif  // !VR

#	if defined(RENDER_DEPTH)
	vsout.Depth = projSpacePosition.zw;
#	endif  // RENDER_DEPTH

	float perInstanceFade = dot(cb8[(asuint(cb7[0].x) >> 2)].xyzw, Math::IdentityMatrix[(asint(cb7[0].x) & 3)].xyzw);

#	if defined(VR)
	float distanceFade = 1 - saturate((length(mul(World[0], msPosition).xyz) - AlphaParam1) / AlphaParam2);
#	else
	float distanceFade = 1 - saturate((length(projSpacePosition.xyz) - AlphaParam1) / AlphaParam2);
#	endif

	// Note: input.Color.w is used for wind speed
	vsout.Color.xyz = input.Color.xyz;
	vsout.Color.w = distanceFade * perInstanceFade;
	vsout.VertexMult = input.InstanceData1.w;

	vsout.TexCoord.xy = input.TexCoord.xy;
	vsout.TexCoord.z = FogNearColor.w;

	vsout.ViewSpacePosition = mul(WorldView[eyeIndex], msPosition).xyz;
	vsout.WorldPosition = mul(World[eyeIndex], msPosition);

	float4 previousMsPosition = GetMSPosition(input, world3x3);

#	ifdef GRASS_COLLISION
	previousMsPosition.xyz += previousDisplacement;
#	endif  // GRASS_COLLISION

	previousMsPosition.xyz += previousWindDisplacement;

	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], previousMsPosition);
#	if defined(VR)
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(projSpacePosition, eyeIndex);
	vsout.HPosition = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // !VR

	// Vertex normal needs to be transformed to world-space for lighting calculations.
	vsout.VertexNormal.xyz = mul(world3x3, input.Normal.xyz * 2.0 - 1.0);
	vsout.VertexNormal.w = input.Color.w;

	return vsout;
}
#else
VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = Stereo::GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif  // VR
	);

	float4 msPosition = GetMSPosition(input);

	float3 windDisplacement = CalculateWindDisplacement(input, WindTimer);
	float3 previousWindDisplacement = CalculateWindDisplacement(input, PreviousWindTimer);

#	ifdef GRASS_COLLISION
	float3 displacement, previousDisplacement;
	GrassCollision::GetDisplacedPosition(input, msPosition.xyz, displacement, previousDisplacement);
	msPosition.xyz += displacement;
#	endif  // GRASS_COLLISION

	msPosition.xyz += windDisplacement;

	float4 projSpacePosition = mul(WorldViewProj[eyeIndex], msPosition);
#	if !defined(VR)
	vsout.HPosition = projSpacePosition;
#	endif  // !VR	vsout.HPosition = projSpacePosition;

#	if defined(RENDER_DEPTH)
	vsout.Depth = projSpacePosition.zw;
#	endif  // RENDER_DEPTH

	float3 instanceNormal = float3(input.InstanceData2.z, input.InstanceData3.zw);
	float dirLightAngle = dot(DirLightDirection.xyz, instanceNormal);
	float3 diffuseMultiplier = input.InstanceData1.www * input.Color.xyz;

	float perInstanceFade = dot(cb8[(asuint(cb7[0].x) >> 2)].xyzw, Math::IdentityMatrix[(asint(cb7[0].x) & 3)].xyzw);

#	if defined(VR)
	float distanceFade = 1 - saturate((length(mul(World[0], msPosition).xyz) - AlphaParam1) / AlphaParam2);
#	else
	float distanceFade = 1 - saturate((length(projSpacePosition.xyz) - AlphaParam1) / AlphaParam2);
#	endif

	vsout.Color.xyz = input.Color.xyz;
	vsout.Color.w = distanceFade * perInstanceFade;
	vsout.VertexMult = input.InstanceData1.w;

	vsout.TexCoord.xy = input.TexCoord.xy;
	vsout.TexCoord.z = FogNearColor.w;

	vsout.AmbientColor.xyz = input.InstanceData1.www * (AmbientColor.xyz * input.Color.xyz);
	vsout.AmbientColor.w = ShadowClampValue;

	vsout.ViewSpacePosition = mul(WorldView[eyeIndex], msPosition).xyz;
	vsout.WorldPosition = mul(World[eyeIndex], msPosition);

	float4 previousMsPosition = GetMSPosition(input);
#	if defined(VR)
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(projSpacePosition, eyeIndex);
	vsout.HPosition = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // !VR

#	ifdef GRASS_COLLISION
	previousMsPosition.xyz += previousDisplacement;
#	endif  // GRASS_COLLISION

	previousMsPosition.xyz += previousWindDisplacement;

	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], previousMsPosition);

	return vsout;
}

#endif

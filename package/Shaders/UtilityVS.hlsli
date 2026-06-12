cbuffer PerTechnique : register(b0)
{
#if !defined(VR)
	float4 HighDetailRange[1] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c1);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#else
	float4 HighDetailRange[2] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c2);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c1);
	float4 EyePos[1] : packoffset(c5);
	float4 WaterParams : packoffset(c6);
	float4 TreeParams : packoffset(c7);
#else
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c1);
	float4 EyePos[2] : packoffset(c9);
	float4 WaterParams : packoffset(c11);
	float4 TreeParams : packoffset(c12);
#endif  // VR
};

float2 SmoothSaturate(float2 value)
{
	return value * value * (3 - 2 * value);
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = Stereo::GetEyeIndexVS(
#if defined(VR)
		input.InstanceID
#endif
	);

#if (defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) || SHADOWFILTER == 2
	vsout.PositionCS.xy = input.PositionMS.xy;
#	if defined(RENDER_SHADOWMASKDPB) || defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB)
	vsout.PositionCS.z = ShadowFadeParam.z;
#	else
	vsout.PositionCS.z = HighDetailRange[eyeIndex].x;
#	endif
	vsout.PositionCS.w = 1;
#elif defined(STENCIL_ABOVE_WATER)
	vsout.PositionCS.y = WaterParams.x * 2 + input.PositionMS.y;
	vsout.PositionCS.xzw = input.PositionMS.xzw;
#else

	precise float4 positionMS = float4(input.PositionMS.xyz, 1.0);
	float4 positionCS = float4(0, 0, 0, 0);

	float3 normalMS = float3(1, 1, 1);
#	if defined(NORMALS)
	normalMS = input.Normal.xyz * 2 - 1;
#	endif

#	if defined(VC) && defined(NORMALS) && defined(TREE_ANIM)
	float2 treeTmp1 = SmoothSaturate(abs(2 * frac(float2(0.1, 0.25) * (TreeParams.w * TreeParams.y * TreeParams.x) + dot(input.PositionMS.xyz, 1.0.xxx) + 0.5) - 1));
	float normalMult = (treeTmp1.x + 0.1 * treeTmp1.y) * (input.Color.w * TreeParams.z);
	positionMS.xyz += normalMS.xyz * normalMult;
#	endif

#	if defined(LOD_LANDSCAPE)
	positionMS = LodLandscape::AdjustLodLandscapeVertexPositionMS(positionMS, World[eyeIndex], HighDetailRange[eyeIndex]);
#	endif

#	if defined(SKINNED)
	precise int4 boneIndices = 765.01.xxxx * input.BoneIndices.xyzw;

	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, boneIndices, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, input.BoneWeights);
	precise float4 positionWS = float4(mul(positionMS, transpose(worldMatrix)), 1);

	positionCS = mul(FrameBuffer::CameraViewProj[eyeIndex], positionWS);
#	else
	precise float4x4 modelViewProj = mul(FrameBuffer::CameraViewProj[eyeIndex], World[eyeIndex]);
	positionCS = mul(modelViewProj, positionMS);
#	endif

#	if defined(RENDER_SHADOWMAP) && defined(RENDER_SHADOWMAP_CLAMPED)
	positionCS.z = max(0, positionCS.z);
#	endif

#	if defined(LOD_LANDSCAPE)
	vsout.PositionCS = LodLandscape::AdjustLodLandscapeVertexPositionCS(positionCS);
#	elif defined(RENDER_SHADOWMAP_PB)
	float3 positionCSPerspective = positionCS.xyz / positionCS.w;
	float3 shadowDirection = normalize(normalize(positionCSPerspective) + float3(0, 0, ParabolaParam.y));
	vsout.PositionCS.xy = shadowDirection.xy / shadowDirection.z;
	vsout.PositionCS.z = ParabolaParam.x * length(positionCSPerspective);
	vsout.PositionCS.w = positionCS.w;
#	else
	vsout.PositionCS = positionCS;
#	endif

#	if defined(RENDER_NORMAL)
	float3 normalVS = float3(1, 1, 1);
#		if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, boneIndices, input.BoneWeights);
	normalMS = normalize(mul(normalMS, transpose(boneRSMatrix)));
	normalVS = mul(FrameBuffer::CameraView[eyeIndex], float4(normalMS, 0)).xyz;
#		else
	normalVS = mul(mul(FrameBuffer::CameraView[eyeIndex], World[eyeIndex]), float4(normalMS, 0)).xyz;
#		endif
#		if defined(RENDER_NORMAL_CLAMP)
	normalVS = max(min(normalVS, 0.1), -0.1);
#		endif
	vsout.Normal.xyz = normalVS;

#		if defined(VC)
	vsout.Normal.w = input.Color.w;
#		else
	vsout.Normal.w = 1;
#		endif

#	endif

#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 texCoord = float4(0, 0, 1, 1);
	texCoord.xy = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

#		if defined(RENDER_NORMAL)
	texCoord.z = max(1, 0.0013333333 * positionCS.z + 0.8);

	float falloff = 1;
#			if defined(RENDER_NORMAL_FALLOFF)
#				if defined(SKINNED)
	falloff = dot(normalMS, normalize(EyePos[eyeIndex].xyz - positionWS.xyz));
#				else
	falloff = dot(normalMS, normalize(EyePos[eyeIndex].xyz - positionMS.xyz));
#				endif
#			endif
	texCoord.w = EyePos[eyeIndex].w * falloff;
#		endif

	vsout.TexCoord0 = texCoord;
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	vsout.TexCoord1.x = ParabolaParam.x * length(positionCSPerspective);
	vsout.TexCoord1.y = positionCS.w;
	precise float parabolaParam = ParabolaParam.y * positionCS.z;
	vsout.TexCoord1.z = parabolaParam * 0.5 + 0.5;
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 texCoord1 = float4(0, 0, 0, 0);
	texCoord1.xy = positionCS.zw;
	texCoord1.zw = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

	vsout.TexCoord1 = texCoord1;
#	elif defined(ADDITIONAL_ALPHA_MASK)
	vsout.TexCoord1 = positionCS.zw;
#	elif defined(DEBUG_SHADOWSPLIT)
	vsout.Depth = positionCS.z;
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.x = 1 - pow(saturate(dot(positionCS.xyz, positionCS.xyz) / ShadowFadeParam.x), 8);

#		if defined(SKINNED)
	vsout.PositionMS.xyz = positionWS.xyz;
#		else
	vsout.PositionMS.xyz = positionMS.xyz;
#		endif
	vsout.PositionMS.w = positionCS.z;
#	endif

#	if (defined(ALPHA_TEST) && defined(VC)) || defined(LOCALMAP_FOGOFWAR)
#		if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.y = input.Color.w;
#		elif !defined(TREE_ANIM)
	vsout.Alpha.x = input.Color.w;
#		endif
#	endif

#endif

#if defined(OFFSET_DEPTH)
	vsout.PositionCS.z += 5.0;
#endif

#ifdef VR
	vsout.EyeIndex = eyeIndex;
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.PositionCS, eyeIndex);
	vsout.PositionCS = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#endif  // VR
	return vsout;
}

cbuffer VS_PerFrame : register(b12)
{
#if !defined(VR)
	row_major float4x4 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#	if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
#		if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[1] : packoffset(c41);
#		endif  // MOTIONVECTORS_NORMALS
#	endif      // SKINNED
#else
	row_major float4x4 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#	if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
#		if defined(MOTIONVECTORS_NORMALS)
	float3 PreviousBonesPivot[2] : packoffset(c82);
#		endif  // MOTIONVECTORS_NORMALS
#	endif      // SKINNED
#endif          // VR
};

cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
	float4 SoftMateralVSParams : packoffset(c1);
	float4 FalloffData : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 MatProj[3] : packoffset(c6);
	float4 EyePosition[1] : packoffset(c12);
	float4 PosAdjust[1] : packoffset(c13);
	float4 TexcoordOffsetMembrane : packoffset(c14);
#else
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 MatProj[3] : packoffset(c12);
	float4 EyePosition[2] : packoffset(c21);
	float4 PosAdjust[2] : packoffset(c23);
	float4 TexcoordOffsetMembrane : packoffset(c25);
#endif  // VR
}

cbuffer IndexedTexcoordBuffer : register(b11)
{
	float4 IndexedTexCoord[128] : packoffset(c0);
}

#if defined(PROJECTED_UV)
float GetProjectedU(float3 worldPosition, float4 texCoordOffset)
{
	float projUvTmp = min(abs(worldPosition.x), abs(worldPosition.y)) *
	                  (1 / max(abs(worldPosition.x), abs(worldPosition.y)));
	float projUvTmpSqr = projUvTmp * projUvTmp;
	float projUvTmp2 =
		projUvTmpSqr *
			(projUvTmpSqr *
					(projUvTmpSqr * (projUvTmpSqr * 0.0208350997 + -0.0851330012) + 0.180141002) +
				-0.330299497) +
		0.999866009;
	float projUvTmp5;
	if (abs(worldPosition.x) > abs(worldPosition.y)) {
		projUvTmp5 = projUvTmp * projUvTmp2 * -2 + Math::HALF_PI;
	} else {
		projUvTmp5 = 0;
	}
	float projUvTmp6 = projUvTmp * projUvTmp2 + projUvTmp5;
	float projUvTmp7;
	if (worldPosition.y < -worldPosition.y) {
		projUvTmp7 = -Math::PI;
	} else {
		projUvTmp7 = 0;
	}
	float projUvTmp4 = projUvTmp6 + projUvTmp7;
	float minCoord = min(worldPosition.x, worldPosition.y);
	float maxCoord = max(worldPosition.x, worldPosition.y);
	if (minCoord < -minCoord && maxCoord >= -maxCoord) {
		projUvTmp4 = -projUvTmp4;
	}
	return abs(0.318309158 * projUvTmp4) * texCoordOffset.w + texCoordOffset.y;
}

float GetProjectedV(float3 worldPosition, uint a_eyeIndex = 0)
{
	return (-PosAdjust[a_eyeIndex].x + (PosAdjust[a_eyeIndex].z + worldPosition.z)) / PosAdjust[a_eyeIndex].y;
}
#endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	uint eyeIndex = Stereo::GetEyeIndexVS(
#if defined(VR)
		input.InstanceID
#endif  // VR
	);
	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	precise row_major float4x4 world4x4 = float4x4(World[eyeIndex][0], World[eyeIndex][1], World[eyeIndex][2], float4(0, 0, 0, 1));
	precise float3x3 world3x3 =
		transpose(float3x3(transpose(World[eyeIndex])[0], transpose(World[eyeIndex])[1], transpose(World[eyeIndex])[2]));

#if defined(SKY_OBJECT)
	float4x4 viewProj = float4x4(ViewProj[eyeIndex][0], ViewProj[eyeIndex][1], ViewProj[eyeIndex][3], ViewProj[eyeIndex][3]);
#else
	row_major float4x4 viewProj = ViewProj[eyeIndex];
#endif

#if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;
#	if defined(MOTIONVECTORS_NORMALS)
	float3x4 previousBoneTransformMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot[eyeIndex], input.BoneWeights);
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousBoneTransformMatrix)), 1);
#	endif
	float3x4 boneTransformMatrix =
		Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot[eyeIndex], input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(boneTransformMatrix)), 1);
	float4 viewPos = mul(viewProj, worldPosition);
#else
	precise float4 worldPosition = float4(mul(World[eyeIndex], inputPosition), 1);
	precise float4 previousWorldPosition = float4(mul(PreviousWorld[eyeIndex], inputPosition), 1);
	precise row_major float4x4 modelView = mul(viewProj, world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#endif

	vsout.Position = viewPos;

#if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
#endif

#if defined(NORMALS) || defined(MOTIONVECTORS_NORMALS)
	float3 normal = input.Normal.xyz * 2 - 1;

#	if defined(SKINNED)
	float3 worldNormal = normalize(mul(normal, boneRSMatrixTr));
#	else
	float3 worldNormal = normalize(mul(world3x3, normal));
#	endif
#endif

#if defined(VC)
	vsout.Color = input.Color;
#endif

#if !defined(MOTIONVECTORS_NORMALS)
	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;
#endif

	float4 texCoord = float4(0, 0, 1, 0);

#if defined(MEMBRANE)
	float4 texCoordOffset = TexcoordOffsetMembrane;
#else
	float4 texCoordOffset = TexcoordOffset;
#endif

#if defined(TEXCOORD_INDEX)
#	if defined(NORMALS)
	uint index = input.TexCoord0.z;
#	else
	uint index = input.Position.w;
#	endif
#endif

#if defined(PROJECTED_UV)
#	if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.x = dot(MatProj[0].xyz, inputPosition.xyz);
#	else
	texCoord.x = GetProjectedU(worldPosition.xyz, texCoordOffset);
#	endif
#else
#	if defined(TEXTURE)
	float u = input.TexCoord0.x;
#		if defined(TEXCOORD_INDEX)
	u = IndexedTexCoord[index].y * u + IndexedTexCoord[index].x;
#		endif
	texCoord.x = u * texCoordOffset.z + texCoordOffset.x;
#	endif
#endif
#if defined(PROJECTED_UV)
#	if defined(NORMALS) && !defined(MEMBRANE)
	texCoord.y = dot(MatProj[1].xyz, inputPosition.xyz);
#	else
	texCoord.y = GetProjectedV(worldPosition.xyz, eyeIndex);
#	endif
#else
#	if defined(TEXTURE)
	float v = input.TexCoord0.y;
#		if defined(TEXCOORD_INDEX)
	v = IndexedTexCoord[index].w * v + IndexedTexCoord[index].z;
#		endif
	texCoord.y = v * texCoordOffset.w + texCoordOffset.y;
#	endif
#endif
#if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.w = input.TexCoord0.y;
#elif defined(SOFT)
	texCoord.w = viewPos.w / SoftMateralVSParams.x;
#elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.w = input.TexCoord0.y;
#endif
#if defined(PROJECTED_UV) && !defined(NORMALS)
	texCoord.z = input.TexCoord0.x;
#elif defined(FALLOFF)
	float3 inverseWorldDirection = normalize(-worldPosition.xyz);
	float WdotN = dot(worldNormal, inverseWorldDirection);
	float falloff = saturate((-FalloffData.x + abs(WdotN)) / (FalloffData.y - FalloffData.x));
	float falloffParam = (falloff * falloff) * (3 - falloff * 2);
	texCoord.z = lerp(FalloffData.z, FalloffData.w, falloffParam);
#elif defined(MEMBRANE) && (!defined(NORMALS) || defined(ALPHA_TEST))
	texCoord.z = input.TexCoord0.x;
#endif
	vsout.TexCoord0 = texCoord;

	float3 eyePosition = 0.0.xxx;
#if defined(MEMBRANE) && defined(TEXTURE) && !defined(SKINNED)
	eyePosition = EyePosition[eyeIndex].xyz;
#endif

	float3 viewPosition = inputPosition.xyz;
#if defined(SKINNED)
	viewPosition = worldPosition.xyz;
#endif

#if defined(MEMBRANE)
#	if defined(SKINNED)
#		if defined(NORMALS)
	vsout.TBN0.xyz = worldNormal;
#		else
	float3x3 tbnTr = float3x3(normalize(boneRSMatrixTr[0]), normalize(boneRSMatrixTr[1]),
		normalize(boneRSMatrixTr[2]));
#			if defined(MOTIONVECTORS_NORMALS)
	tbnTr[2] = worldNormal;
#			endif
	float3x3 tbn = transpose(tbnTr);
	vsout.TBN0.xyz = tbn[0];
	vsout.TBN1.xyz = tbn[1];
	vsout.TBN2.xyz = tbn[2];
#		endif
#	endif

	vsout.ViewVector.xyz = normalize(eyePosition - viewPosition);
	vsout.ViewVector.w = 1;
#endif

#if !defined(SKINNED) && defined(NORMALS) && !(defined(MOTIONVECTORS_NORMALS) && defined(MEMBRANE) && !defined(SKINNED) && defined(NORMALS))
#	if defined(MEMBRANE)
	vsout.TBN0.xyz = normal;
#	elif defined(PROJECTED_UV)
	vsout.TBN0.xyz = input.Normal.xyz;
#	endif
#endif

#if defined(MOTIONVECTORS_NORMALS) && !(defined(MEMBRANE) && defined(SKINNED) && defined(NORMALS))
#	if defined(SKINNED) && !defined(MEMBRANE)
	float3 screenSpaceNormal = normal;
#	elif defined(FALLOFF) || (defined(SKINNED) && defined(MEMBRANE))
	float3 screenSpaceNormal = worldNormal;
#	else
	float4x4 modelScreen = mul(ScreenProj[eyeIndex], world4x4);
	float3 screenSpaceNormal = normalize(mul(modelScreen, float4(normal, 0))).xyz;
#	endif

	vsout.ScreenSpaceNormal = screenSpaceNormal;
#endif

#if defined(LIGHTING)
	vsout.MSPosition = viewPosition;
#endif

	vsout.FogAlpha.x = FogNearColor.w;

#if defined(PROJECTED_UV) && defined(NORMALS) && !defined(MEMBRANE)
	vsout.TBN1.xyz = MatProj[2].xyz;
#endif

	vsout.WorldPosition = worldPosition;
#if defined(MOTIONVECTORS_NORMALS)
	vsout.PreviousWorldPosition = previousWorldPosition;
#endif

#ifdef VR
	vsout.EyeIndex = eyeIndex;
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#endif  // VR
	return vsout;
}

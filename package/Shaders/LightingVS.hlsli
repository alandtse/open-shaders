
cbuffer PerTechnique : register(b0)
{
#if !defined(VR)
	float4 HighDetailRange[1] : packoffset(c0);  // loaded cells center in xy, size in zw
	float4 FogParam : packoffset(c1);
	float4 FogNearColor : packoffset(c2);
	float4 FogFarColor : packoffset(c3);
#else
	float4 HighDetailRange[2] : packoffset(c0);  // loaded cells center in xy, size in zw
	float4 FogParam : packoffset(c2);
	float4 FogNearColor : packoffset(c3);
	float4 FogFarColor : packoffset(c4);
#endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 LeftEyeCenter : packoffset(c0);
	float4 RightEyeCenter : packoffset(c1);
	float4 TexcoordOffset : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(VR)
	row_major float3x4 World[1] : packoffset(c0);
	row_major float3x4 PreviousWorld[1] : packoffset(c3);
	float4 EyePosition[1] : packoffset(c6);
	float4 LandBlendParams : packoffset(c7);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c8);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c9);
	row_major float3x4 TextureProj[1] : packoffset(c10);
	float IndexScale : packoffset(c13);
	float4 WorldMapOverlayParameters : packoffset(c14);
#else   // VR has 49 vs 30 entries
	row_major float3x4 World[2] : packoffset(c0);
	row_major float3x4 PreviousWorld[2] : packoffset(c6);
	float4 EyePosition[2] : packoffset(c12);
	float4 LandBlendParams : packoffset(c14);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c15);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c16);
	row_major float3x4 TextureProj[2] : packoffset(c17);
	float IndexScale : packoffset(c23);
	float4 WorldMapOverlayParameters : packoffset(c24);
#endif  // VR
};

cbuffer VS_PerFrame : register(b12)
{
#if !defined(VR)
	row_major float3x3 ScreenProj[1] : packoffset(c0);
	row_major float4x4 ViewProj[1] : packoffset(c8);
#	if defined(SKINNED)
	float3 BonesPivot[1] : packoffset(c40);
	float3 PreviousBonesPivot[1] : packoffset(c41);
#	endif  // SKINNED
#else
	row_major float3x3 ScreenProj[2] : packoffset(c0);
	row_major float4x4 ViewProj[2] : packoffset(c16);
#	if defined(SKINNED)
	float3 BonesPivot[2] : packoffset(c80);
	float3 PreviousBonesPivot[2] : packoffset(c82);
#	endif  // SKINNED
#endif      // VR
};

#if defined(TREE_ANIM)
float2 GetTreeShiftVector(float4 position, float4 color)
{
	precise float4 tmp1 = (TreeParams.w * TreeParams.y).xxxx * WindTimers.xxyy;
	precise float4 tmp2 = float4(0.1, 0.25, 0.1, 0.25) * tmp1 + dot(position.xyz, 1.0.xxx).xxxx;
	precise float4 tmp3 = abs(-1.0.xxxx + 2.0.xxxx * frac(0.5.xxxx + tmp2.xyzw));
	precise float4 tmp4 = (tmp3 * tmp3) * (3.0.xxxx - 2.0.xxxx * tmp3);
	return (tmp4.xz + 0.1.xx * tmp4.yw) * (TreeParams.z * color.w).xx;
}
#endif  // TREE_ANIM

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

	uint eyeIndex = Stereo::GetEyeIndexVS(
#if defined(VR)
		input.InstanceID
#endif
	);
#if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	inputPosition = LodLandscape::AdjustLodLandscapeVertexPositionMS(inputPosition, float4x4(World[eyeIndex], float4(0, 0, 0, 1)), HighDetailRange[eyeIndex]);
#endif  // defined(LODLANDNOISE) || defined(LODLANDSCAPE)                                                                   \

	precise float4 previousInputPosition = inputPosition;

#if defined(TREE_ANIM)
	precise float2 treeShiftVector = GetTreeShiftVector(input.Position, input.Color);
	float3 normal = -1.0.xxx + 2.0.xxx * input.Normal.xyz;

	inputPosition.xyz += normal.xyz * treeShiftVector.x;
	previousInputPosition.xyz += normal.xyz * treeShiftVector.y;
#endif

#if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;

	float3x4 previousWorldMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot[eyeIndex], input.BoneWeights);
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousWorldMatrix)), 1);

	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot[eyeIndex], input.BoneWeights);
	precise float4 worldPosition = float4(mul(inputPosition, transpose(worldMatrix)), 1);

	float4 viewPos = mul(ViewProj[eyeIndex], worldPosition);
#else   // !SKINNED
	precise float4 previousWorldPosition = float4(mul(PreviousWorld[eyeIndex], inputPosition), 1);
	precise float4 worldPosition = float4(mul(World[eyeIndex], inputPosition), 1);
	precise float4x4 world4x4 = float4x4(World[eyeIndex][0], World[eyeIndex][1], World[eyeIndex][2], float4(0, 0, 0, 1));
	precise float4x4 modelView = mul(ViewProj[eyeIndex], world4x4);
	float4 viewPos = mul(modelView, inputPosition);
#endif  // SKINNED

	vsout.Position = viewPos;

#if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	vsout.Position.z += min(1, 1e-4 * max(0, viewPos.z - 70000)) * 0.5;
#endif

	float2 uv = input.TexCoord0.xy * TexcoordOffset.zw + TexcoordOffset.xy;
#if defined(LANDSCAPE)
	vsout.TexCoord0.zw = (uv * 0.010416667.xx + LandBlendParams.xy) * float2(1, -1) + float2(0, 1);
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexCoord0.z = mul(TextureProj[eyeIndex][0], inputPosition);
	vsout.TexCoord0.w = mul(TextureProj[eyeIndex][1], inputPosition);
#endif
	vsout.TexCoord0.xy = uv;

#if defined(WORLD_MAP)
	vsout.InputPosition.xyz = WorldMapOverlayParameters.xyz + worldPosition.xyz;
#endif

#if defined(SKINNED)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
#endif

#if !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(
		float3(input.Position.w, input.Normal.w * 2 - 1, input.Bitangent.w * 2 - 1),
		input.Bitangent.xyz * 2.0.xxx + -1.0.xxx,
		input.Normal.xyz * 2.0.xxx + -1.0.xxx);
	float3x3 tbnTr = transpose(tbn);

#	if defined(SKINNED)
	float3x3 worldTbnTr = transpose(mul(transpose(tbnTr), transpose(boneRSMatrix)));
	float3x3 worldTbnTrTr = transpose(worldTbnTr);
	worldTbnTrTr[0] = normalize(worldTbnTrTr[0]);
	worldTbnTrTr[1] = normalize(worldTbnTrTr[1]);
	worldTbnTrTr[2] = normalize(worldTbnTrTr[2]);
	worldTbnTr = transpose(worldTbnTrTr);
	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#	else
	vsout.TBN0.xyz = mul(tbn, World[eyeIndex][0].xyz);
	vsout.TBN1.xyz = mul(tbn, World[eyeIndex][1].xyz);
	vsout.TBN2.xyz = mul(tbn, World[eyeIndex][2].xyz);
	float3x3 tempTbnTr = transpose(float3x3(vsout.TBN0.xyz, vsout.TBN1.xyz, vsout.TBN2.xyz));
	tempTbnTr[0] = normalize(tempTbnTr[0]);
	tempTbnTr[1] = normalize(tempTbnTr[1]);
	tempTbnTr[2] = normalize(tempTbnTr[2]);
	tempTbnTr = transpose(tempTbnTr);
	vsout.TBN0.xyz = tempTbnTr[0];
	vsout.TBN1.xyz = tempTbnTr[1];
	vsout.TBN2.xyz = tempTbnTr[2];
#	endif
#elif defined(SKINNED)
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
	float3x3 worldTbnTr = transpose(float3x3(normalize(boneRSMatrixTr[0]),
		normalize(boneRSMatrixTr[1]), normalize(boneRSMatrixTr[2])));

	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#endif

#if defined(LANDSCAPE)
	vsout.LandBlendWeights1 = input.LandBlendWeights1;

	float2 gridOffset = LandBlendParams.zw - input.Position.xy;
	vsout.LandBlendWeights2.w = 1 - saturate(0.000375600968 * (9625.59961 - length(gridOffset)));
	vsout.LandBlendWeights2.xyz = input.LandBlendWeights2.xyz;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3x3 texProjWorld3x3 = float3x3(World[eyeIndex][0].xyz, World[eyeIndex][1].xyz, World[eyeIndex][2].xyz);
	vsout.TexProj = mul(texProjWorld3x3, TextureProj[eyeIndex][2].xyz);
#endif

#if defined(EYE)
	precise float4 modelEyeCenter = float4(LeftEyeCenter.xyz + input.EyeParameter.xxx * (RightEyeCenter.xyz - LeftEyeCenter.xyz), 1);
	vsout.EyeNormal.xyz = normalize(worldPosition.xyz - mul(modelEyeCenter, transpose(worldMatrix)));
#endif  // EYE

	vsout.WorldPosition = worldPosition;
	vsout.PreviousWorldPosition = previousWorldPosition;

#if defined(VC)
	vsout.Color = input.Color;
#else
	vsout.Color = 1.0.xxxx;
#endif  // VC

	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(FogNearColor.xyz, FogFarColor.xyz, fogColorParam);
	vsout.FogParam.w = fogColorParam;

#if defined(VR)
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.Position, eyeIndex);
	vsout.Position = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#endif  // VR

	vsout.ModelPosition = input.Position.xyz;

	return vsout;
}

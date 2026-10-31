#ifndef __SNOW_DEFORMATION_UPDATE_PARAMS_HLSLI__
#define __SNOW_DEFORMATION_UPDATE_PARAMS_HLSLI__

#define MAX_STAMPS 96
#define TILE_TEXELS 64
#define FIELD_TEXELS 2048
#define BLANKET_TEXELS 128
#define BLANKET_CELL 64

cbuffer UpdateParams : register(b0)
{
	float4 Control;
	float4 Shape;
	float4 Options;
	float4 EyeOffset;
	float4 Probe;
	int4 OriginCell;
	int4 BlanketCell;
	float4 BlanketShape;
	float4 BlanketFade;
	float4 Coarse;
	float4 Stamps[MAX_STAMPS];
	float4 StampShape[MAX_STAMPS];
	float4 StampMotion[MAX_STAMPS];
	float4 StampSurface[MAX_STAMPS];
	float4 StampBounds[MAX_STAMPS];
};

#endif

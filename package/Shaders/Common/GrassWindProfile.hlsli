#ifndef __GRASS_WIND_PROFILE_DEPENDENCY_HLSL__
#define __GRASS_WIND_PROFILE_DEPENDENCY_HLSL__

namespace GrassWindProfile
{
	cbuffer MeshProfile : register(b9)
	{
		float BendSensitivity;
		float FlutterSensitivity;
		float FlutterTipConcentration;
		float TipFlexibility;
	};
}

#endif

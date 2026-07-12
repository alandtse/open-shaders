#pragma once

namespace RE
{
	class ImageSpaceEffectDepthOfField;
	class ImageSpaceEffectParam;
}

struct ID3D11DeviceContext;

namespace UnderwaterDepthOfField
{
	void InstallHooks();
	void InstallD3DHooks(ID3D11DeviceContext* a_context);
	void RecordShaderConstants(const RE::ImageSpaceEffectDepthOfField* a_effect, RE::ImageSpaceEffectParam* a_param);
	void BeginRender();
	void EndRender();
}

#pragma once

#include <d3d11.h>
#include <winrt/base.h>

namespace VRFeatures
{
	class VRRadialDensityMask
	{
	public:
		static VRRadialDensityMask* GetSingleton();

		void Initialize();
		void Apply(ID3D11DeviceContext* a_context);
		void ApplyForRenderTarget(ID3D11DeviceContext* a_context, uint32_t rtWidth, uint32_t rtHeight);
		void Cleanup();

		void SetEnabled(bool a_enabled);
		void UpdateMask();                                                    // Regenerate mask with current settings
		void UpdateMaskForSize(uint32_t renderWidth, uint32_t renderHeight);  // For dynamic resolution
		bool IsEnabled() const { return enabled; }

		struct Settings
		{
			float innerRadius = 0.5f;
			float outerRadius = 0.8f;
		} settings;

	private:
		VRRadialDensityMask() = default;
		~VRRadialDensityMask() = default;
		VRRadialDensityMask(const VRRadialDensityMask&) = delete;
		VRRadialDensityMask& operator=(const VRRadialDensityMask&) = delete;

		bool enabled = false;
		bool initialized = false;
		uint32_t currentWidth = 0;
		uint32_t currentHeight = 0;

		winrt::com_ptr<ID3D11ComputeShader> maskGenerationCS;
		winrt::com_ptr<ID3D11VertexShader> applyVS;
		winrt::com_ptr<ID3D11PixelShader> applyPS;
		winrt::com_ptr<ID3D11Texture2D> maskTexture;
		winrt::com_ptr<ID3D11ShaderResourceView> maskSRV;
		winrt::com_ptr<ID3D11UnorderedAccessView> maskUAV;
		winrt::com_ptr<ID3D11DepthStencilState> applyStencilState;
		winrt::com_ptr<ID3D11Buffer> paramCB;

		struct CBData
		{
			float CenterLeft[2];   // Center of left eye in pixels
			float CenterRight[2];  // Center of right eye in pixels
			float InnerRadiusSq;   // Squared radius for full quality
			float OuterRadiusSq;   // Squared radius for reduced quality
			float HalfWidth;       // Boundary between left/right eye
			float Pad;
		};

		void GenerateMask();
		void GenerateMaskForSize(uint32_t width, uint32_t height);
	};
}

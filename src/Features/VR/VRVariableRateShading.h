#pragma once

#include <d3d11.h>
#include <nvapi.h>
#include <nvapi_lite_d3dext.h>
#include <wrl/client.h>

namespace VRFeatures
{
	class VRVariableRateShading
	{
	public:
		static VRVariableRateShading* GetSingleton();

		bool Initialize();
		void Apply(ID3D11DeviceContext* a_context);
		void ApplyForRenderTarget(ID3D11DeviceContext* a_context, uint32_t rtWidth, uint32_t rtHeight);
		void Disable(ID3D11DeviceContext* a_context);
		void Cleanup();

		void SetEnabled(bool a_enabled);
		void UpdatePattern();                                                    // Call when radii settings change
		void UpdatePatternForSize(uint32_t renderWidth, uint32_t renderHeight);  // For dynamic resolution
		bool IsEnabled() const { return enabled; }
		bool IsAvailable() const { return nvapiAvailable; }

		struct Settings
		{
			float innerRadius = 0.50f;
			float middleRadius = 0.65f;
			float outerRadius = 0.80f;
			bool debugOverlay = false;
		} settings;

	private:
		VRVariableRateShading() = default;
		~VRVariableRateShading() = default;
		VRVariableRateShading(const VRVariableRateShading&) = delete;
		VRVariableRateShading& operator=(const VRVariableRateShading&) = delete;

		bool enabled = false;
		bool initialized = false;
		bool nvapiAvailable = false;

		Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> shadingRateView;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> srrTexture;
		uint32_t currentWidth = 0;
		uint32_t currentHeight = 0;
		uint32_t targetWidth = 0;   // Expected render target width (for dynamic res)
		uint32_t targetHeight = 0;  // Expected render target height

		void CreateShadingRateResource(uint32_t width, uint32_t height);
		void UpdateShadingRatePattern();
	};
}

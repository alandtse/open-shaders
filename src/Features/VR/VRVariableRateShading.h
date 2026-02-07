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

		void SetEyeCenters(float leftX, float leftY, float rightX, float rightY);  // Normalized coordinates (0-1)
		void SetAspectRatio(float aspect);                                         // Visual aspect ratio (Width/Height)
		ID3D11ShaderResourceView* GetMaskSRV() const { return nullptr; }           // VRS texture is not standard SRV, handled differently?
		// Actually, srrTexture is a standard texture, we can create an SRV for it if needed for debug
		ID3D11ShaderResourceView* GetDebugSRV();

		bool IsEnabled() const { return enabled; }
		bool IsAvailable() const { return nvapiAvailable; }

		struct Settings
		{
			float innerRadius = 0.50f;
			float middleRadius = 0.65f;
			float outerRadius = 0.80f;
			float edgeRadius = 1.15f;
			bool favorHorizontal = true;
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

		// Eye centers (normalized 0-1 across the whole texture)
		float leftEyeCenterX = 0.25f;
		float leftEyeCenterY = 0.5f;
		float rightEyeCenterX = 0.75f;
		float rightEyeCenterY = 0.5f;
		float targetAspectRatio = 1.0f;  // Default square

		Microsoft::WRL::ComPtr<ID3D11NvShadingRateResourceView> shadingRateView;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> srrTexture;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srrSRV;  // For debug
		uint32_t currentWidth = 0;
		uint32_t currentHeight = 0;
		uint32_t targetWidth = 0;   // Expected render target width (for dynamic res)
		uint32_t targetHeight = 0;  // Expected render target height

		void CreateShadingRateResource(uint32_t width, uint32_t height);
		void UpdateShadingRatePattern();
	};
}

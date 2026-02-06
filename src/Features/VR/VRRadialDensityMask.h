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

		void SetEyeCenters(float leftX, float leftY, float rightX, float rightY);  // Normalized coordinates (0-1)
		void SetAspectRatio(float aspect);                                         // Visual aspect ratio (Width/Height)
		ID3D11ShaderResourceView* GetMaskSRV() const { return maskSRV.get(); }

		bool IsEnabled() const { return enabled; }

		struct Settings
		{
			float innerRadius = 0.5f;
			float middleRadius = 0.65f;
			float outerRadius = 0.8f;
			bool favorHorizontal = true;
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

		// Eye centers (normalized 0-1 across the whole texture)
		float leftEyeCenterX = 0.25f;
		float leftEyeCenterY = 0.5f;
		float rightEyeCenterX = 0.75f;
		float rightEyeCenterY = 0.5f;
		float targetAspectRatio = 1.0f;

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
			float MiddleRadiusSq;  // Squared radius for half quality (checkerboard)
			float OuterRadiusSq;   // Squared radius for reduced quality (cull)
			float HalfWidth;       // Boundary between left/right eye
			float HeightScale;     // Vertical squashing factor (1.0 = circular, >1.0 = wider ellipse)
			float Pad[3];          // Pad to 48 bytes (16-byte alignment)
		};

		void GenerateMask();
		void GenerateMaskForSize(uint32_t width, uint32_t height);
	};
}

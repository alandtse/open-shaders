#pragma once
#include <array>
#include <d3d11.h>
#include <winrt/base.h>

namespace Util
{
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);

	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");

	// Texture manipulation utilities
	void ApplyHighlightTintToTexture(ID3D11Texture2D* texture, bool isHighlighted, const std::array<float, 4>& highlightColor = { 1.0f, 0.5f, 0.0f, 0.3f });
	HRESULT CreateOverlayTextureAndRTV(ID3D11Device* device, int width, int height, ID3D11Texture2D** outTex, ID3D11RenderTargetView** outRTV);

	// VR-aware counts for render targets
	inline int GetRenderTargetCount()
	{
		return globals::game::isVR ? RE::RENDER_TARGETS::kVRTOTAL : RE::RENDER_TARGETS::kTOTAL;
	}

	inline int GetDepthStencilCount()
	{
		return globals::game::isVR ? RE::RENDER_TARGETS_DEPTHSTENCIL::kVRTOTAL : RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL;
	}

	HRESULT SaveTextureToFile(ID3D11Device* device, ID3D11DeviceContext* context, const std::filesystem::path& path, ID3D11Texture2D* tex);
	HRESULT LoadTextureFromFile(ID3D11Device* device, const std::filesystem::path& path, ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSRV);

	// Returns the current scene depth SRV, preferring terrain-blended depth when active.
	// The caller does NOT own the returned pointer.
	//
	// prefer16bit = false (default): R32_FLOAT  -- for compute shaders doing arithmetic on depth
	// prefer16bit = true:            R16_UNORM  -- for pixel shaders via slot 17 / SharedData::GetDepth
	ID3D11ShaderResourceView* GetCurrentSceneDepthSRV(bool prefer16bit = false);

	/**
	 * @brief RAII guard that snapshots the D3D11 pipeline state a fullscreen
	 * draw pass clobbers and restores+Releases it on scope exit.
	 *
	 * Covers OM (RTV/DSV, blend, depth-stencil), RS (state, viewports),
	 * the VS/PS/GS/HS/DS shaders, IA (input layout, vertex/index buffers,
	 * topology), PS sampler/SRV slot 0, and PS constant-buffer slot 1. The
	 * destructor nulls PS SRV slot 0 before restoring to break any SRV-vs-RTV
	 * hazard left by the wrapped pass. Construct it, set up + issue the pass,
	 * then let it go out of scope.
	 */
	struct FullscreenPassScope
	{
		explicit FullscreenPassScope(ID3D11DeviceContext* a_context);
		~FullscreenPassScope();
		FullscreenPassScope(const FullscreenPassScope&) = delete;
		FullscreenPassScope& operator=(const FullscreenPassScope&) = delete;

	private:
		ID3D11DeviceContext* ctx = nullptr;
		ID3D11RenderTargetView* savedRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11BlendState* savedBlend = nullptr;
		FLOAT savedBlendFactor[4] = {};
		UINT savedSampleMask = 0;
		ID3D11DepthStencilState* savedDSState = nullptr;
		UINT savedStencilRef = 0;
		ID3D11VertexShader* savedVS = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11GeometryShader* savedGS = nullptr;
		ID3D11HullShader* savedHS = nullptr;
		ID3D11DomainShader* savedDS = nullptr;
		ID3D11RasterizerState* savedRS = nullptr;
		ID3D11SamplerState* savedSampler0 = nullptr;
		ID3D11ShaderResourceView* savedSRV0 = nullptr;
		ID3D11Buffer* savedPSCB1 = nullptr;
		ID3D11InputLayout* savedIL = nullptr;
		ID3D11Buffer* savedVB[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		UINT savedVBStride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		UINT savedVBOffset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
		ID3D11Buffer* savedIB = nullptr;
		DXGI_FORMAT savedIBFormat = DXGI_FORMAT_UNKNOWN;
		UINT savedIBOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};
}  // namespace Util

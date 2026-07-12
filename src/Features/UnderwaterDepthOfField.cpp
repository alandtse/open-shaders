#include "UnderwaterDepthOfField.h"

#include "Buffer.h"
#include "Deferred.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include "RE/I/ImageSpaceEffectDepthOfField.h"
#include "RE/I/ImageSpaceShaderParam.h"
#include "RE/R/Renderer.h"
#include "RE/T/TESWaterForm.h"
#include "RE/T/TESWaterSystem.h"

#include <DirectXMath.h>

namespace
{
	constexpr UINT kFullscreenVertexCount = 3;
	// Pixel constants mirror ISDepthOfField.hlsl: DOF params at c1-c3, fog at c4-c5, masked params at c6.
	constexpr std::uint32_t kDofParamsOffset = 4;
	constexpr std::uint32_t kDofParams3Offset = 12;
	constexpr std::uint32_t kFogColorOffset = 16;
	constexpr std::uint32_t kFogAmountOffset = kFogColorOffset + 3;
	constexpr std::uint32_t kFogRangeOffset = 20;
	constexpr std::uint32_t kDitherFlagOffset = kFogRangeOffset + 3;
	constexpr std::uint32_t kDofParams6Offset = 24;
	constexpr std::uint32_t kRequiredFogRegisters = 6;
	constexpr std::uint32_t kRequiredMaskedRegisters = 7;

	struct alignas(16) DepthOfFieldInputConstants
	{
		DirectX::XMFLOAT4 fogColor;
		DirectX::XMFLOAT4 fogRange;
		DirectX::XMFLOAT4 dofParams;
		DirectX::XMFLOAT4 dofParams3;
		DirectX::XMFLOAT4 dofParams6;
		DirectX::XMFLOAT4 flags;
	};
	STATIC_ASSERT_ALIGNAS_16(DepthOfFieldInputConstants);

	struct UnderwaterFogConstants
	{
		DepthOfFieldInputConstants values{};
		bool valid = false;
	};

	struct DepthOfFieldShaderOptions
	{
		bool fogged = false;
		bool masked = false;
	};

	struct ScratchTarget
	{
		std::unique_ptr<Texture2D> texture;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};

	ScratchTarget sharpScratch;
	UnderwaterFogConstants currentFog;
	DepthOfFieldShaderOptions currentOptions;
	std::unique_ptr<ConstantBuffer> depthOfFieldInputCB;
	winrt::com_ptr<ID3D11VertexShader> depthOfFieldInputVS;
	winrt::com_ptr<ID3D11PixelShader> depthOfFieldInputPS;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> pointSampler;
	bool insideDepthOfFieldRender = false;
	bool appliedFogToDepthOfFieldInput = false;
	bool waitingForDepthOfFieldInputDraw = false;
	bool insideDepthOfFieldInputPass = false;
	RE::ImageSpaceEffectParam* pendingDownsampleParam = nullptr;

	constexpr std::array<RE::RENDER_TARGETS::RENDER_TARGET, 4> kDepthOfFieldInputTargets{
		RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY2,
		RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY,
		RE::RENDER_TARGETS::kMAIN_COPY,
		RE::RENDER_TARGETS::kMAIN
	};

	bool GetFallbackFogConstants(DepthOfFieldInputConstants& a_constants)
	{
		const auto* waterSystem = RE::TESWaterSystem::GetSingleton();
		const auto* water = waterSystem ? waterSystem->currentWaterType : nullptr;
		if (!water)
			return false;

		const auto& waterData = water->data;
		const float fogFar = waterData.underwaterFogDistFar;
		const float fogNear = waterData.underwaterFogDistNear;
		const float fogAmount = waterData.underwaterFogAmount;
		if (fogAmount <= 0.0f || fogFar == fogNear)
			return false;

		const float cameraNear = globals::game::cameraNear ? *globals::game::cameraNear : 1.0f;
		const float cameraFar = globals::game::cameraFar ? *globals::game::cameraFar : fogFar;
		if (cameraNear <= 0.0f || cameraFar <= cameraNear)
			return false;

		const auto waterColor = Util::TryGetWaterData(0.0f, 0.0f);
		a_constants.fogColor = {
			waterColor.x,
			waterColor.y,
			waterColor.z,
			fogAmount
		};
		a_constants.fogRange = { fogFar, fogNear, 0.0f, 0.0f };
		a_constants.dofParams = { 0.0f, 0.0f, cameraNear, cameraFar };
		a_constants.dofParams3 = { cameraNear, cameraFar, 0.0f, 0.0f };
		a_constants.dofParams6 = a_constants.dofParams;
		a_constants.flags = {};
		return true;
	}

	bool HasPixelConstantRegisters(const RE::ImageSpaceShaderParam* a_param, std::uint32_t a_requiredRegisters)
	{
		if (!a_param || !a_param->pixelConstantGroup)
			return false;

		return a_param->pixelConstantGroupSize >= a_requiredRegisters;
	}

	bool GetOption(const RE::ImageSpaceEffectDepthOfField* a_effect, std::uint32_t a_index)
	{
		if (!a_effect)
			return false;

		const auto index = static_cast<decltype(a_effect->options)::size_type>(a_index);
		return a_effect->options.capacity() > index && a_effect->options[index];
	}

	DirectX::XMFLOAT4 ReadFloat4(const float* a_values, std::uint32_t a_offset)
	{
		return { a_values[a_offset], a_values[a_offset + 1], a_values[a_offset + 2], a_values[a_offset + 3] };
	}

	bool RecordDepthOfFieldParam(RE::ImageSpaceEffectParam* a_param)
	{
		if (!a_param)
			return false;

		auto* shaderParam = skyrim_cast<RE::ImageSpaceShaderParam*>(a_param);
		if (!HasPixelConstantRegisters(shaderParam, kRequiredFogRegisters))
			return false;

		const float* constants = shaderParam->pixelConstantGroup;
		DepthOfFieldInputConstants values{};
		values.dofParams = ReadFloat4(constants, kDofParamsOffset);
		values.dofParams3 = ReadFloat4(constants, kDofParams3Offset);
		values.fogColor = ReadFloat4(constants, kFogColorOffset);
		values.fogRange = ReadFloat4(constants, kFogRangeOffset);
		values.dofParams6 = HasPixelConstantRegisters(shaderParam, kRequiredMaskedRegisters) ? ReadFloat4(constants, kDofParams6Offset) : values.dofParams;

		if (values.fogColor.w <= 0.0f || values.fogRange.x == values.fogRange.y)
			return false;

		currentFog.values = values;
		currentFog.valid = true;
		return true;
	}

	bool ReadDynamicFetchFlag(RE::ImageSpaceEffectParam* a_param)
	{
		auto* shaderParam = skyrim_cast<RE::ImageSpaceShaderParam*>(a_param);
		return HasPixelConstantRegisters(shaderParam, 1) && shaderParam->pixelConstantGroup[0] > 0.5f;
	}

	ID3D11VertexShader* GetDepthOfFieldInputVS()
	{
		if (!depthOfFieldInputVS) {
			depthOfFieldInputVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
				L"Data/Shaders/UnderwaterFogToDepthOfField.hlsl",
				{ { "VSHADER", "" } },
				"vs_5_0")));
			Util::SetResourceName(depthOfFieldInputVS.get(), "UnderwaterDepthOfField::InputFogVS");
		}
		return depthOfFieldInputVS.get();
	}

	ID3D11PixelShader* GetDepthOfFieldInputPS()
	{
		if (!depthOfFieldInputPS) {
			depthOfFieldInputPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
				L"Data/Shaders/UnderwaterFogToDepthOfField.hlsl",
				{ { "PSHADER", "" } },
				"ps_5_0")));
			Util::SetResourceName(depthOfFieldInputPS.get(), "UnderwaterDepthOfField::InputFogPS");
		}
		return depthOfFieldInputPS.get();
	}

	ID3D11RasterizerState* GetRasterizerState()
	{
		if (!rasterizerState) {
			D3D11_RASTERIZER_DESC desc{};
			desc.FillMode = D3D11_FILL_SOLID;
			desc.CullMode = D3D11_CULL_NONE;
			desc.DepthClipEnable = true;
			DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&desc, rasterizerState.put()));
			Util::SetResourceName(rasterizerState.get(), "UnderwaterDepthOfField::RasterizerState");
		}
		return rasterizerState.get();
	}

	ID3D11SamplerState* GetLinearSampler()
	{
		if (globals::deferred && globals::deferred->linearSampler)
			return globals::deferred->linearSampler;

		if (!linearSampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, linearSampler.put()));
			Util::SetResourceName(linearSampler.get(), "UnderwaterDepthOfField::LinearSampler");
		}
		return linearSampler.get();
	}

	ID3D11SamplerState* GetPointSampler()
	{
		if (globals::deferred && globals::deferred->pointSampler)
			return globals::deferred->pointSampler;

		if (!pointSampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&desc, pointSampler.put()));
			Util::SetResourceName(pointSampler.get(), "UnderwaterDepthOfField::PointSampler");
		}
		return pointSampler.get();
	}

	bool EnsureScratchTarget(RE::BSGraphics::RenderTargetData& a_target, ScratchTarget& a_scratch, const char* a_name)
	{
		if (!a_target.texture || !a_target.RTV)
			return false;

		D3D11_TEXTURE2D_DESC desc{};
		a_target.texture->GetDesc(&desc);
		if (a_scratch.texture &&
			a_scratch.width == desc.Width &&
			a_scratch.height == desc.Height &&
			a_scratch.format == desc.Format) {
			return true;
		}

		D3D11_TEXTURE2D_DESC scratchDesc = desc;
		scratchDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		scratchDesc.CPUAccessFlags = 0;
		scratchDesc.MiscFlags = 0;
		scratchDesc.Usage = D3D11_USAGE_DEFAULT;

		auto scratch = std::make_unique<Texture2D>(scratchDesc, a_name);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		scratch->CreateSRV(srvDesc);

		a_scratch.texture = std::move(scratch);
		a_scratch.width = desc.Width;
		a_scratch.height = desc.Height;
		a_scratch.format = desc.Format;
		return true;
	}

	class GraphicsStateScope
	{
	public:
		explicit GraphicsStateScope(ID3D11DeviceContext* a_context) :
			ctx(a_context)
		{
			ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTV, &savedDSV);
			ctx->RSGetViewports(&numVP, savedVP);
			ctx->OMGetBlendState(&savedBlend, savedBlendFactor, &savedSampleMask);
			ctx->OMGetDepthStencilState(&savedDSState, &savedStencilRef);
			ctx->VSGetShader(&savedVS, nullptr, nullptr);
			ctx->PSGetShader(&savedPS, nullptr, nullptr);
			ctx->GSGetShader(&savedGS, nullptr, nullptr);
			ctx->HSGetShader(&savedHS, nullptr, nullptr);
			ctx->DSGetShader(&savedDS, nullptr, nullptr);
			ctx->RSGetState(&savedRS);
			ctx->PSGetSamplers(0, static_cast<UINT>(savedSamplers.size()), savedSamplers.data());
			ctx->PSGetShaderResources(0, static_cast<UINT>(savedSRV.size()), savedSRV.data());
			ctx->PSGetConstantBuffers(0, static_cast<UINT>(savedCB.size()), savedCB.data());
			ctx->IAGetInputLayout(&savedIL);
			ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, savedVB, savedVBStride, savedVBOffset);
			ctx->IAGetIndexBuffer(&savedIB, &savedIBFormat, &savedIBOffset);
			ctx->IAGetPrimitiveTopology(&savedTopology);
		}

		~GraphicsStateScope()
		{
			ID3D11ShaderResourceView* nullSRV[3]{};
			ctx->PSSetShaderResources(0, ARRAYSIZE(nullSRV), nullSRV);
			ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTV, savedDSV);
			if (numVP > 0)
				ctx->RSSetViewports(numVP, savedVP);
			ctx->OMSetBlendState(savedBlend, savedBlendFactor, savedSampleMask);
			ctx->OMSetDepthStencilState(savedDSState, savedStencilRef);
			ctx->VSSetShader(savedVS, nullptr, 0);
			ctx->PSSetShader(savedPS, nullptr, 0);
			ctx->GSSetShader(savedGS, nullptr, 0);
			ctx->HSSetShader(savedHS, nullptr, 0);
			ctx->DSSetShader(savedDS, nullptr, 0);
			ctx->RSSetState(savedRS);
			ctx->PSSetSamplers(0, static_cast<UINT>(savedSamplers.size()), savedSamplers.data());
			ctx->PSSetShaderResources(0, static_cast<UINT>(savedSRV.size()), savedSRV.data());
			ctx->PSSetConstantBuffers(0, static_cast<UINT>(savedCB.size()), savedCB.data());
			ctx->IASetInputLayout(savedIL);
			ctx->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, savedVB, savedVBStride, savedVBOffset);
			ctx->IASetIndexBuffer(savedIB, savedIBFormat, savedIBOffset);
			ctx->IASetPrimitiveTopology(savedTopology);

			ReleaseSavedState();
		}

	private:
		void ReleaseSavedState()
		{
			for (auto*& rtv : savedRTV) {
				if (rtv)
					rtv->Release();
			}
			if (savedDSV)
				savedDSV->Release();
			if (savedBlend)
				savedBlend->Release();
			if (savedDSState)
				savedDSState->Release();
			if (savedVS)
				savedVS->Release();
			if (savedPS)
				savedPS->Release();
			if (savedGS)
				savedGS->Release();
			if (savedHS)
				savedHS->Release();
			if (savedDS)
				savedDS->Release();
			if (savedRS)
				savedRS->Release();
			for (auto*& sampler : savedSamplers) {
				if (sampler)
					sampler->Release();
			}
			for (auto*& srv : savedSRV) {
				if (srv)
					srv->Release();
			}
			for (auto*& cb : savedCB) {
				if (cb)
					cb->Release();
			}
			if (savedIL)
				savedIL->Release();
			for (auto*& vb : savedVB) {
				if (vb)
					vb->Release();
			}
			if (savedIB)
				savedIB->Release();
		}

		ID3D11DeviceContext* ctx = nullptr;
		ID3D11RenderTargetView* savedRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		UINT numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11BlendState* savedBlend = nullptr;
		FLOAT savedBlendFactor[4]{};
		UINT savedSampleMask = 0;
		ID3D11DepthStencilState* savedDSState = nullptr;
		UINT savedStencilRef = 0;
		ID3D11VertexShader* savedVS = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11GeometryShader* savedGS = nullptr;
		ID3D11HullShader* savedHS = nullptr;
		ID3D11DomainShader* savedDS = nullptr;
		ID3D11RasterizerState* savedRS = nullptr;
		std::array<ID3D11SamplerState*, 2> savedSamplers{};
		std::array<ID3D11ShaderResourceView*, 3> savedSRV{};
		std::array<ID3D11Buffer*, 13> savedCB{};
		ID3D11InputLayout* savedIL = nullptr;
		ID3D11Buffer* savedVB[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
		UINT savedVBStride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
		UINT savedVBOffset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
		ID3D11Buffer* savedIB = nullptr;
		DXGI_FORMAT savedIBFormat = DXGI_FORMAT_UNKNOWN;
		UINT savedIBOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	};

	void DrawDepthOfFieldInputPass(RE::BSGraphics::RenderTargetData& a_target, ScratchTarget& a_scratch, ID3D11ShaderResourceView* a_maskSRV)
	{
		auto* context = globals::d3d::context;
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->CopyResource(a_scratch.texture->resource.get(), a_target.texture);

		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(a_scratch.width);
		viewport.Height = static_cast<float>(a_scratch.height);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		context->RSSetState(GetRasterizerState());
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);

		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		ID3D11SamplerState* samplers[] = { GetLinearSampler(), GetPointSampler() };
		context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

		ID3D11ShaderResourceView* srvs[] = {
			a_scratch.texture->srv.get(),
			globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV,
			a_maskSRV
		};
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		auto* cb = depthOfFieldInputCB->CB();
		context->PSSetConstantBuffers(0, 1, &cb);
		if (auto* perFrame = globals::game::perFrame.get(); perFrame && *perFrame) {
			ID3D11Buffer* frameBuffers[] = { *perFrame };
			context->PSSetConstantBuffers(12, ARRAYSIZE(frameBuffers), frameBuffers);
		}

		ID3D11RenderTargetView* rtvs[] = { a_target.RTV };
		context->OMSetRenderTargets(1, rtvs, nullptr);
		context->VSSetShader(GetDepthOfFieldInputVS(), nullptr, 0);
		context->PSSetShader(GetDepthOfFieldInputPS(), nullptr, 0);
		insideDepthOfFieldInputPass = true;
		{
			const SKSE::stl::scope_exit resetInputPass([]() noexcept {
				insideDepthOfFieldInputPass = false;
			});
			context->Draw(kFullscreenVertexCount, 0);
		}

		ID3D11ShaderResourceView* nullSRV[3]{};
		context->PSSetShaderResources(0, ARRAYSIZE(nullSRV), nullSRV);
	}

	RE::BSGraphics::RenderTargetData* FindDepthOfFieldInputTarget(ID3D11ShaderResourceView* a_sourceSRV)
	{
		if (!a_sourceSRV || !globals::game::renderer)
			return nullptr;

		ID3D11Resource* sourceResource = nullptr;
		a_sourceSRV->GetResource(&sourceResource);
		if (!sourceResource)
			return nullptr;

		auto& renderTargets = globals::game::renderer->GetRuntimeData().renderTargets;
		RE::BSGraphics::RenderTargetData* result = nullptr;
		for (auto targetIndex : kDepthOfFieldInputTargets) {
			auto& target = renderTargets[targetIndex];
			if (target.SRV == a_sourceSRV || target.texture == sourceResource) {
				result = &target;
				break;
			}
		}

		sourceResource->Release();
		return result;
	}

	RE::BSGraphics::RenderTargetData* GetBoundDepthOfFieldInputTarget()
	{
		auto* context = globals::d3d::context;
		if (!context)
			return nullptr;

		ID3D11ShaderResourceView* sourceSRV = nullptr;
		context->PSGetShaderResources(0, 1, &sourceSRV);
		auto* target = FindDepthOfFieldInputTarget(sourceSRV);
		if (sourceSRV)
			sourceSRV->Release();
		return target;
	}

	void ApplyUnderwaterFogToDepthOfFieldInput(RE::ImageSpaceEffectParam* a_downsampleParam)
	{
		if (!insideDepthOfFieldRender || appliedFogToDepthOfFieldInput || !currentOptions.fogged)
			return;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context)
			return;

		auto& runtimeData = renderer->GetRuntimeData();
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* inputTarget = GetBoundDepthOfFieldInputTarget();
		auto& underwaterMask = runtimeData.renderTargets[RE::RENDER_TARGETS::kUNDERWATER_MASK];

		if (!inputTarget || !inputTarget->RTV || !depth.depthSRV || !EnsureScratchTarget(*inputTarget, sharpScratch, "UnderwaterDepthOfField::SharpScratch"))
			return;

		auto* maskSRV = currentOptions.masked ? underwaterMask.SRV : nullptr;
		DepthOfFieldInputConstants constants{};
		if (currentFog.valid) {
			constants = currentFog.values;
		} else if (!GetFallbackFogConstants(constants)) {
			return;
		}
		constants.flags.x = maskSRV ? 1.0f : 0.0f;
		constants.flags.y = ReadDynamicFetchFlag(a_downsampleParam) ? 1.0f : 0.0f;

		if (!depthOfFieldInputCB)
			depthOfFieldInputCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DepthOfFieldInputConstants>(), "UnderwaterDepthOfField::InputFogCB");
		depthOfFieldInputCB->Update(constants);

		CS_GPU_PASS("UnderwaterDepthOfField::InputFog");
		GraphicsStateScope graphicsScope(context);
		DrawDepthOfFieldInputPass(*inputTarget, sharpScratch, maskSRV);
		appliedFogToDepthOfFieldInput = true;
	}

	void RunPendingDepthOfFieldInputPass()
	{
		if (!waitingForDepthOfFieldInputDraw || insideDepthOfFieldInputPass)
			return;

		auto* param = pendingDownsampleParam;
		waitingForDepthOfFieldInputDraw = false;
		pendingDownsampleParam = nullptr;
		ApplyUnderwaterFogToDepthOfFieldInput(param);
	}

	class FinalUnderwaterFogBypassScope
	{
	public:
		explicit FinalUnderwaterFogBypassScope(RE::ImageSpaceEffectParam* a_param)
		{
			if (!insideDepthOfFieldRender || !appliedFogToDepthOfFieldInput)
				return;

			auto* shaderParam = skyrim_cast<RE::ImageSpaceShaderParam*>(a_param);
			if (!HasPixelConstantRegisters(shaderParam, kRequiredFogRegisters))
				return;

			pixelConstants = shaderParam->pixelConstantGroup;
			backupFogAmount = pixelConstants[kFogAmountOffset];
			backupDitherFlag = pixelConstants[kDitherFlagOffset];
			pixelConstants[kFogAmountOffset] = 0.0f;
			pixelConstants[kDitherFlagOffset] = 1.0f;
		}

		~FinalUnderwaterFogBypassScope()
		{
			if (pixelConstants) {
				pixelConstants[kFogAmountOffset] = backupFogAmount;
				pixelConstants[kDitherFlagOffset] = backupDitherFlag;
			}
		}

	private:
		float* pixelConstants = nullptr;
		float backupFogAmount = 0.0f;
		float backupDitherFlag = 0.0f;
	};

	struct HDRDownSample4_Render
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
		{
			if (insideDepthOfFieldRender && !appliedFogToDepthOfFieldInput && currentOptions.fogged) {
				pendingDownsampleParam = param;
				waitingForDepthOfFieldInputDraw = true;
			}
			func(imageSpaceShader, shape, param);
			if (pendingDownsampleParam == param) {
				pendingDownsampleParam = nullptr;
				waitingForDepthOfFieldInputDraw = false;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_DrawIndexed
	{
		static void thunk(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
		{
			RunPendingDepthOfFieldInputPass();
			func(This, IndexCount, StartIndexLocation, BaseVertexLocation);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ID3D11DeviceContext_Draw
	{
		static void thunk(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
		{
			RunPendingDepthOfFieldInputPass();
			func(This, VertexCount, StartVertexLocation);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct DepthOfFieldFogged_Render
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
		{
			FinalUnderwaterFogBypassScope bypassFinalFog(param);
			func(imageSpaceShader, shape, param);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct DepthOfFieldMaskedFogged_Render
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
		{
			FinalUnderwaterFogBypassScope bypassFinalFog(param);
			func(imageSpaceShader, shape, param);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace UnderwaterDepthOfField
{
	void InstallHooks()
	{
		stl::write_vfunc<0x1, HDRDownSample4_Render>(RE::VTABLE_BSImagespaceShaderHDRDownSample4[3]);
		stl::write_vfunc<0x1, DepthOfFieldFogged_Render>(RE::VTABLE_BSImagespaceShaderDepthOfFieldFogged[3]);
		stl::write_vfunc<0x1, DepthOfFieldMaskedFogged_Render>(RE::VTABLE_BSImagespaceShaderDepthOfFieldMaskedFogged[3]);
	}

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		if (!a_context)
			return;

		stl::detour_vfunc<12, ID3D11DeviceContext_DrawIndexed>(a_context);
		stl::detour_vfunc<13, ID3D11DeviceContext_Draw>(a_context);
	}

	void RecordShaderConstants(const RE::ImageSpaceEffectDepthOfField* a_effect, RE::ImageSpaceEffectParam* a_param)
	{
		static constexpr std::uint32_t kFoggedOptionIndex = 3;
		static constexpr std::uint32_t kMaskedOptionIndex = 4;

		currentFog = {};
		currentOptions = {};
		currentOptions.fogged = GetOption(a_effect, kFoggedOptionIndex);
		currentOptions.masked = GetOption(a_effect, kMaskedOptionIndex);
		currentOptions.fogged = currentOptions.fogged || currentOptions.masked;
		if (!currentOptions.fogged)
			return;

		if (RecordDepthOfFieldParam(a_param))
			return;

		if (!a_effect)
			return;

		for (std::uint16_t index = 0; index < a_effect->effectParams.capacity(); ++index) {
			if (RecordDepthOfFieldParam(a_effect->effectParams[index]))
				return;
		}
	}

	void BeginRender()
	{
		insideDepthOfFieldRender = true;
		appliedFogToDepthOfFieldInput = false;
		waitingForDepthOfFieldInputDraw = false;
		pendingDownsampleParam = nullptr;
	}

	void EndRender()
	{
		insideDepthOfFieldRender = false;
		appliedFogToDepthOfFieldInput = false;
		waitingForDepthOfFieldInputDraw = false;
		pendingDownsampleParam = nullptr;
		currentFog = {};
		currentOptions = {};
	}
}

#include "ReblurIntegration.h"

#include "Globals.h"
#include "Utils/D3D.h"
#include "RenderContext.h"
#include <DirectXTex.h>

#include <d3dcompiler.h>

namespace
{
	static const DXGI_FORMAT kNRDFormatTable[] = {
		DXGI_FORMAT_R8_UNORM,
		DXGI_FORMAT_R8_SNORM,
		DXGI_FORMAT_R8_UINT,
		DXGI_FORMAT_R8_SINT,
		DXGI_FORMAT_R8G8_UNORM,
		DXGI_FORMAT_R8G8_SNORM,
		DXGI_FORMAT_R8G8_UINT,
		DXGI_FORMAT_R8G8_SINT,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_SNORM,
		DXGI_FORMAT_R8G8B8A8_UINT,
		DXGI_FORMAT_R8G8B8A8_SINT,
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R16_UNORM,
		DXGI_FORMAT_R16_SNORM,
		DXGI_FORMAT_R16_UINT,
		DXGI_FORMAT_R16_SINT,
		DXGI_FORMAT_R16_FLOAT,
		DXGI_FORMAT_R16G16_UNORM,
		DXGI_FORMAT_R16G16_SNORM,
		DXGI_FORMAT_R16G16_UINT,
		DXGI_FORMAT_R16G16_SINT,
		DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R16G16B16A16_UNORM,
		DXGI_FORMAT_R16G16B16A16_SNORM,
		DXGI_FORMAT_R16G16B16A16_UINT,
		DXGI_FORMAT_R16G16B16A16_SINT,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R32_UINT,
		DXGI_FORMAT_R32_SINT,
		DXGI_FORMAT_R32_FLOAT,
		DXGI_FORMAT_R32G32_UINT,
		DXGI_FORMAT_R32G32_SINT,
		DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R32G32B32_UINT,
		DXGI_FORMAT_R32G32B32_SINT,
		DXGI_FORMAT_R32G32B32_FLOAT,
		DXGI_FORMAT_R32G32B32A32_UINT,
		DXGI_FORMAT_R32G32B32A32_SINT,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R10G10B10A2_UNORM,
		DXGI_FORMAT_R10G10B10A2_UINT,
		DXGI_FORMAT_R11G11B10_FLOAT,
		DXGI_FORMAT_R9G9B9E5_SHAREDEXP,
	};
	static_assert(ARRAYSIZE(kNRDFormatTable) == static_cast<uint32_t>(nrd::Format::MAX_NUM));

}

DXGI_FORMAT NRDReblurIntegration::NRDFormatToDXGI(nrd::Format fmt)
{
	auto idx = static_cast<uint32_t>(fmt);
	if (idx >= static_cast<uint32_t>(nrd::Format::MAX_NUM))
		return DXGI_FORMAT_UNKNOWN;
	return kNRDFormatTable[idx];
}

bool NRDReblurIntegration::Init(uint32_t halfWidth, uint32_t halfHeight, nrd::Denoiser denoiser, uint32_t identifier)
{
	Shutdown();
	if (!halfWidth || !halfHeight || halfWidth > UINT16_MAX || halfHeight > UINT16_MAX) return false;
	NRDRendering::LogMemory("before REBLUR pool", halfWidth, halfHeight);
	m_identifier = identifier;
	m_label = nrd::GetDenoiserString(denoiser);
	m_width = halfWidth;
	m_height = halfHeight;

	nrd::DenoiserDesc denoiserDesc{ identifier, denoiser };
	nrd::InstanceCreationDesc desc{};
	desc.denoisers = &denoiserDesc;
	desc.denoisersNum = 1;

	if (const auto result = nrd::CreateInstance(desc, m_instance); result != nrd::Result::SUCCESS || !m_instance) {
		logger::error("NRDReblurIntegration: CreateInstance failed (result={})", static_cast<uint32_t>(result));
		Shutdown();
		return false;
	}

	try {
		CreatePipelines();
		CreateSamplers();
		CreatePoolTextures(halfWidth, halfHeight);

		const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);
		CreateConstantBuffer(instanceDesc.constantBufferMaxDataSize);
	} catch (const std::exception& e) {
		logger::error("NRDReblurIntegration: {} resource creation failed at {}x{}: {}", m_label, halfWidth, halfHeight, e.what());
		Shutdown();
		return false;
	}

	logger::info("NRDReblurIntegration: initialized {} {}x{}", m_label, halfWidth, halfHeight);
	NRDRendering::LogMemory("after REBLUR pool", halfWidth, halfHeight);
	return true;
}

void NRDReblurIntegration::Shutdown()
{
	m_commonSettingsValid = m_denoiserSettingsValid = false;
	m_width = m_height = m_cbMaxSize = 0;
	memset(m_namedSRV, 0, sizeof(m_namedSRV));
	memset(m_namedUAV, 0, sizeof(m_namedUAV));
	DestroyPipelines();
	DestroyPoolTextures();
	DestroyValidationTexture();
	m_constantBuffer = nullptr;
	for (auto& s : m_samplers)
		s = nullptr;
	if (m_instance) {
		nrd::DestroyInstance(*m_instance);
		m_instance = nullptr;
	}
}

void NRDReblurIntegration::Resize(uint32_t halfWidth, uint32_t halfHeight)
{
	if (m_width == halfWidth && m_height == halfHeight)
		return;
	m_width = halfWidth;
	m_height = halfHeight;
	DestroyPoolTextures();
	CreatePoolTextures(halfWidth, halfHeight);
	DestroyValidationTexture();
}

void NRDReblurIntegration::SetCommonSettings(const nrd::CommonSettings& settings)
{
	m_commonSettingsValid = false;
	m_denoiserSettingsValid = false;
	if (!m_instance) return;
	if (settings.resourceSize[0] != m_width || settings.resourceSize[1] != m_height ||
		!settings.rectSize[0] || !settings.rectSize[1] || settings.rectSize[0] > m_width || settings.rectSize[1] > m_height) {
		logger::error("[NRD] {} incompatible resources {}x{}, rect {}x{}, pool {}x{}", m_label, settings.resourceSize[0], settings.resourceSize[1], settings.rectSize[0], settings.rectSize[1], m_width, m_height);
		return;
	}
	m_validationEnabled = settings.enableValidation;
	if (m_validationEnabled) {
		CreateValidationTexture();
		SetNamedUAV(nrd::ResourceType::OUT_VALIDATION, m_validation.uav.get());
	} else {
		DestroyValidationTexture();
		SetNamedUAV(nrd::ResourceType::OUT_VALIDATION, nullptr);
	}
	const auto result = nrd::SetCommonSettings(*m_instance, settings);
	m_commonSettingsValid = result == nrd::Result::SUCCESS;
	if (!m_commonSettingsValid) logger::error("[NRD] {} SetCommonSettings failed: {}", m_label, static_cast<uint32_t>(result));
}

void NRDReblurIntegration::SetDenoiserSettings(const void* settings)
{
	m_denoiserSettingsValid = false;
	if (!m_instance) return;
	const auto result = nrd::SetDenoiserSettings(*m_instance, m_identifier, settings);
	m_denoiserSettingsValid = result == nrd::Result::SUCCESS;
	if (!m_denoiserSettingsValid) logger::error("[NRD] {} SetDenoiserSettings failed: {}", m_label, static_cast<uint32_t>(result));
}

void NRDReblurIntegration::SetNamedSRV(nrd::ResourceType type, ID3D11ShaderResourceView* srv)
{
	auto idx = static_cast<uint32_t>(type);
	if (idx < static_cast<uint32_t>(nrd::ResourceType::TRANSIENT_POOL))
		m_namedSRV[idx] = srv;
}

void NRDReblurIntegration::SetNamedUAV(nrd::ResourceType type, ID3D11UnorderedAccessView* uav)
{
	auto idx = static_cast<uint32_t>(type);
	if (idx < static_cast<uint32_t>(nrd::ResourceType::TRANSIENT_POOL))
		m_namedUAV[idx] = uav;
}

ID3D11View* NRDReblurIntegration::ResolveResource(const nrd::ResourceDesc& res)
{
	auto typeIdx = static_cast<uint32_t>(res.type);

	if (res.type == nrd::ResourceType::PERMANENT_POOL) {
		if (res.indexInPool >= m_permanentPool.size()) return nullptr;
		auto& pt = m_permanentPool[res.indexInPool];
		return (res.descriptorType == nrd::DescriptorType::TEXTURE) ? (ID3D11View*)pt.srv.get() : (ID3D11View*)pt.uav.get();
	}
	if (res.type == nrd::ResourceType::TRANSIENT_POOL) {
		if (res.indexInPool >= m_transientPool.size()) return nullptr;
		auto& tt = m_transientPool[res.indexInPool];
		return (res.descriptorType == nrd::DescriptorType::TEXTURE) ? (ID3D11View*)tt.srv.get() : (ID3D11View*)tt.uav.get();
	}

	// Named resource
	if (typeIdx < static_cast<uint32_t>(nrd::ResourceType::TRANSIENT_POOL)) {
		if (res.descriptorType == nrd::DescriptorType::TEXTURE)
			return m_namedSRV[typeIdx];
		else
			return m_namedUAV[typeIdx];
	}

	return nullptr;
}

bool NRDReblurIntegration::Dispatch()
{
	// Named views are borrowed for the duration of a call only. Never retain a
	// dangling reference after a failed dispatch or after resource replacement.
	const auto clearNamed = [this]() {
		memset(m_namedSRV, 0, sizeof(m_namedSRV));
		memset(m_namedUAV, 0, sizeof(m_namedUAV));
		m_commonSettingsValid = m_denoiserSettingsValid = false;
	};
	if (!m_instance || !m_commonSettingsValid || !m_denoiserSettingsValid) {
		clearNamed();
		return false;
	}
	auto* context = globals::d3d::context;
	const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);
	const nrd::DispatchDesc* dispatches = nullptr;
	uint32_t dispatchCount = 0;
	const auto result = nrd::GetComputeDispatches(*m_instance, &m_identifier, 1, dispatches, dispatchCount);
	if (result != nrd::Result::SUCCESS || !dispatches || !dispatchCount) {
		logger::error("[NRD] {} GetComputeDispatches failed: {}", m_label, static_cast<uint32_t>(result));
		clearNamed();
		return false;
	}
	const uint32_t base = instanceDesc.resourcesBaseRegisterIndex;
	const uint32_t cbSlot = instanceDesc.constantBufferRegisterIndex;
	const uint32_t samplerSlot = instanceDesc.samplersBaseRegisterIndex;
	bool success = true;
	bool constantsUploaded = false;
	for (uint32_t di = 0; success && di < dispatchCount; ++di) {
		const auto& dispatch = dispatches[di];
		if (!dispatch.gridWidth || !dispatch.gridHeight) continue;
		if (dispatch.pipelineIndex >= m_pipelines.size() || dispatch.pipelineIndex >= instanceDesc.pipelinesNum || !m_pipelines[dispatch.pipelineIndex]) {
			success = false; break;
		}
		const auto& pipeline = instanceDesc.pipelines[dispatch.pipelineIndex];
		ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
		ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
		const uint32_t maxUAVs = globals::d3d::device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
		uint32_t srvCount = 0, uavCount = 0, resourceIndex = 0;
		// Validate every view and slot before binding any of them.
		for (uint32_t ri = 0; success && ri < pipeline.resourceRangesNum; ++ri) {
			const auto& range = pipeline.resourceRanges[ri];
			for (uint32_t k = 0; k < range.descriptorsNum; ++k, ++resourceIndex) {
				if (!dispatch.resources || resourceIndex >= dispatch.resourcesNum) { success = false; break; }
				const auto& resource = dispatch.resources[resourceIndex];
				if (resource.descriptorType != range.descriptorType) { success = false; break; }
				auto* view = ResolveResource(resource);
				if (!view) { success = false; break; }
				if (range.descriptorType == nrd::DescriptorType::TEXTURE) {
					if (base + srvCount >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) { success = false; break; }
					srvs[srvCount++] = static_cast<ID3D11ShaderResourceView*>(view);
				} else {
					if (base + uavCount >= maxUAVs) { success = false; break; }
					uavs[uavCount++] = static_cast<ID3D11UnorderedAccessView*>(view);
				}
			}
		}
		if (!success || resourceIndex != dispatch.resourcesNum) { success = false; break; }
		if (dispatch.constantBufferDataSize) {
			if (!dispatch.constantBufferData || !m_constantBuffer || dispatch.constantBufferDataSize > m_cbMaxSize || cbSlot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) { success = false; break; }
			if (!constantsUploaded || !dispatch.constantBufferDataMatchesPreviousDispatch) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const HRESULT hr = context->Map(m_constantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
				if (FAILED(hr)) { logger::error("[NRD] {} constant-buffer Map failed: 0x{:08X}", m_label, uint32_t(hr)); success = false; break; }
				memcpy(mapped.pData, dispatch.constantBufferData, dispatch.constantBufferDataSize);
				context->Unmap(m_constantBuffer.get(), 0);
				constantsUploaded = true;
			}
			auto* cb = m_constantBuffer.get();
			context->CSSetConstantBuffers(cbSlot, 1, &cb);
		}
		ID3D11SamplerState* samplers[2] = {m_samplers[0].get(), m_samplers[1].get()};
		if (samplerSlot + 2 > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) { success = false; break; }
		context->CSSetSamplers(samplerSlot, 2, samplers);
		context->CSSetShader(m_pipelines[dispatch.pipelineIndex].get(), nullptr, 0);
		if (srvCount) context->CSSetShaderResources(base, srvCount, srvs);
		if (uavCount) context->CSSetUnorderedAccessViews(base, uavCount, uavs, nullptr);
		context->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
		std::fill_n(srvs, srvCount, nullptr); std::fill_n(uavs, uavCount, nullptr);
		if (srvCount) context->CSSetShaderResources(base, srvCount, srvs);
		if (uavCount) context->CSSetUnorderedAccessViews(base, uavCount, uavs, nullptr);
	}
	clearNamed();
	context->CSSetShader(nullptr, nullptr, 0);
	if (!success) logger::error("[NRD] {} invalid dispatch resource/pipeline/constant buffer; device status 0x{:08X}", m_label, uint32_t(globals::d3d::device->GetDeviceRemovedReason()));
	return success;
}

void NRDReblurIntegration::CreatePoolTextures(uint32_t w, uint32_t h)
{
	auto device = globals::d3d::device;
	const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);

	uint32_t totalSize = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;
	uint64_t permanentBytes = 0, transientBytes = 0;
	m_permanentPool.resize(instanceDesc.permanentPoolSize);
	m_transientPool.resize(instanceDesc.transientPoolSize);

	for (uint32_t i = 0; i < totalSize; i++) {
		bool isPermanent = (i < instanceDesc.permanentPoolSize);
		uint32_t poolIdx = isPermanent ? i : (i - instanceDesc.permanentPoolSize);
		const auto& texDesc = isPermanent ? instanceDesc.permanentPool[poolIdx] : instanceDesc.transientPool[poolIdx];

		DXGI_FORMAT fmt = NRDFormatToDXGI(texDesc.format);
		if (fmt == DXGI_FORMAT_UNKNOWN || !texDesc.downsampleFactor)
			throw std::runtime_error("NRD: unsupported pool texture format or downsample factor");

		uint32_t tw = (w + texDesc.downsampleFactor - 1) / texDesc.downsampleFactor;
		uint32_t th = (h + texDesc.downsampleFactor - 1) / texDesc.downsampleFactor;
		(isPermanent ? permanentBytes : transientBytes) += uint64_t(tw) * th * DirectX::BitsPerPixel(fmt) / 8;

		D3D11_TEXTURE2D_DESC d3dDesc{};
		d3dDesc.Width = tw;
		d3dDesc.Height = th;
		d3dDesc.MipLevels = 1;
		d3dDesc.ArraySize = 1;
		d3dDesc.Format = fmt;
		d3dDesc.SampleDesc.Count = 1;
		d3dDesc.Usage = D3D11_USAGE_DEFAULT;
		d3dDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		PoolTexture& pt = isPermanent ? m_permanentPool[poolIdx] : m_transientPool[poolIdx];
		DX::ThrowIfFailed(device->CreateTexture2D(&d3dDesc, nullptr, pt.texture.put()));
		const char* poolName = isPermanent ? "Permanent" : "Transient";
		Util::SetResourceName(pt.texture.get(), "NRD::%s %s%u", m_label, poolName, poolIdx);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = fmt;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		DX::ThrowIfFailed(device->CreateShaderResourceView(pt.texture.get(), &srvDesc, pt.srv.put()));
		Util::SetResourceName(pt.srv.get(), "NRD::%s %s%u SRV", m_label, poolName, poolIdx);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = fmt;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(pt.texture.get(), &uavDesc, pt.uav.put()));
		Util::SetResourceName(pt.uav.get(), "NRD::%s %s%u UAV", m_label, poolName, poolIdx);
	}
	logger::info("[NRD] {} pool payload: permanent={} bytes ({} textures), transient={} bytes ({} textures)", m_label, permanentBytes, instanceDesc.permanentPoolSize, transientBytes, instanceDesc.transientPoolSize);
}

void NRDReblurIntegration::DestroyPoolTextures()
{
	m_permanentPool.clear();
	m_transientPool.clear();
}

void NRDReblurIntegration::CreateValidationTexture()
{
	if (m_validation.texture)
		return;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = m_width;
	desc.Height = m_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, m_validation.texture.put()));
	DX::ThrowIfFailed(device->CreateShaderResourceView(m_validation.texture.get(), nullptr, m_validation.srv.put()));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(m_validation.texture.get(), nullptr, m_validation.uav.put()));
	Util::SetResourceName(m_validation.texture.get(), "NRD::%s Validation", m_label);
	Util::SetResourceName(m_validation.srv.get(), "NRD::%s Validation SRV", m_label);
	Util::SetResourceName(m_validation.uav.get(), "NRD::%s Validation UAV", m_label);
}

void NRDReblurIntegration::DestroyValidationTexture()
{
	m_validation = {};
	m_validationEnabled = false;
}

void NRDReblurIntegration::CreatePipelines()
{
	auto device = globals::d3d::device;
	const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);

	m_pipelines.resize(instanceDesc.pipelinesNum);
	for (uint32_t i = 0; i < instanceDesc.pipelinesNum; i++) {
		const auto& pipeline = instanceDesc.pipelines[i];
		const auto& dxbc = pipeline.computeShaderDXBC;
		if (!dxbc.bytecode || !dxbc.size)
			continue;
		DX::ThrowIfFailed(device->CreateComputeShader(dxbc.bytecode, static_cast<SIZE_T>(dxbc.size), nullptr, m_pipelines[i].put()));
		Util::SetResourceName(m_pipelines[i].get(), "NRD::%s Pipeline%u", m_label, i);
	}
}

void NRDReblurIntegration::DestroyPipelines()
{
	m_pipelines.clear();
}

void NRDReblurIntegration::CreateConstantBuffer(uint32_t maxSize)
{
	if (!maxSize)
		return;

	m_cbMaxSize = maxSize;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = (maxSize + 15) & ~15u;  // align to 16 bytes
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, m_constantBuffer.put()));
	Util::SetResourceName(m_constantBuffer.get(), "NRD::%s Constants", m_label);
}

void NRDReblurIntegration::CreateSamplers()
{
	auto device = globals::d3d::device;

	D3D11_SAMPLER_DESC desc{};
	desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MaxAnisotropy = 1;
	desc.MinLOD = 0;
	desc.MaxLOD = D3D11_FLOAT32_MAX;

	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	DX::ThrowIfFailed(device->CreateSamplerState(&desc, m_samplers[0].put()));  // NEAREST
	Util::SetResourceName(m_samplers[0].get(), "NRD::%s NearestClampSampler", m_label);

	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	DX::ThrowIfFailed(device->CreateSamplerState(&desc, m_samplers[1].put()));  // LINEAR
	Util::SetResourceName(m_samplers[1].get(), "NRD::%s LinearClampSampler", m_label);
}

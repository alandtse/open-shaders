#include "NRDReblurIntegration.h"

#include "Globals.h"
#include "Utils/D3D.h"

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

	constexpr uint32_t kMaxRangeSRVs = 32;
	constexpr uint32_t kMaxRangeUAVs = 16;
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

	m_identifier = identifier;
	m_label = nrd::GetDenoiserString(denoiser);
	m_width = halfWidth;
	m_height = halfHeight;

	nrd::DenoiserDesc denoiserDesc{ identifier, denoiser };
	nrd::InstanceCreationDesc desc{};
	desc.denoisers = &denoiserDesc;
	desc.denoisersNum = 1;

	if (nrd::CreateInstance(desc, m_instance) != nrd::Result::SUCCESS) {
		logger::error("NRDReblurIntegration: CreateInstance failed");
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

	logger::info("NRDReblurIntegration: initialized {}x{}", halfWidth, halfHeight);
	return true;
}

void NRDReblurIntegration::Shutdown()
{
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
	if (m_instance) {
		m_validationEnabled = settings.enableValidation;
		if (m_validationEnabled) {
			CreateValidationTexture();
			SetNamedUAV(nrd::ResourceType::OUT_VALIDATION, m_validation.uav.get());
		}
		nrd::SetCommonSettings(*m_instance, settings);
	}
}

void NRDReblurIntegration::SetDenoiserSettings(const void* settings)
{
	if (m_instance)
		nrd::SetDenoiserSettings(*m_instance, m_identifier, settings);
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
		auto& pt = m_permanentPool[res.indexInPool];
		return (res.descriptorType == nrd::DescriptorType::TEXTURE) ? (ID3D11View*)pt.srv.get() : (ID3D11View*)pt.uav.get();
	}
	if (res.type == nrd::ResourceType::TRANSIENT_POOL) {
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
	if (!m_instance)
		return false;

	auto context = globals::d3d::context;
	const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);

	const nrd::DispatchDesc* dispatchDescs = nullptr;
	uint32_t dispatchNum = 0;
	nrd::GetComputeDispatches(*m_instance, &m_identifier, 1, dispatchDescs, dispatchNum);
	bool success = dispatchDescs && dispatchNum;

	static bool s_debugLogged = false;

	for (uint32_t di = 0; success && di < dispatchNum; di++) {
		const auto& dispatch = dispatchDescs[di];

		if (!s_debugLogged) {
			bool pipelineValid = dispatch.pipelineIndex < static_cast<uint32_t>(m_pipelines.size()) && m_pipelines[dispatch.pipelineIndex];
			// Log null UAVs for this dispatch
			const auto& pipeline = instanceDesc.pipelines[dispatch.pipelineIndex < m_pipelines.size() ? dispatch.pipelineIndex : 0];
			uint32_t resIdx2 = 0;
			bool hasNullUAV = false;
			for (uint32_t ri = 0; ri < pipeline.resourceRangesNum; ri++) {
				const auto& range2 = pipeline.resourceRanges[ri];
				for (uint32_t k = 0; k < range2.descriptorsNum; k++, resIdx2++) {
					if (range2.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
						auto* v = ResolveResource(dispatch.resources[resIdx2]);
						if (!v)
							hasNullUAV = true;
					}
				}
			}
			logger::debug("NRD dispatch[{}] '{}' pipeline={} valid={} grid={}x{} nullUAV={}", di, dispatch.name ? dispatch.name : "?", dispatch.pipelineIndex, pipelineValid, dispatch.gridWidth, dispatch.gridHeight, hasNullUAV);
		}

		if (!dispatch.gridWidth || !dispatch.gridHeight)
			continue;

		// Set compute shader
		if (dispatch.pipelineIndex >= static_cast<uint32_t>(m_pipelines.size()) || !m_pipelines[dispatch.pipelineIndex]) { success=false; break; }
		context->CSSetShader(m_pipelines[dispatch.pipelineIndex].get(), nullptr, 0);

		// Update constant buffer
		if (dispatch.constantBufferDataSize && dispatch.constantBufferData && m_constantBuffer) {
			if (!dispatch.constantBufferDataMatchesPreviousDispatch) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(context->Map(m_constantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					memcpy(mapped.pData, dispatch.constantBufferData, dispatch.constantBufferDataSize);
					context->Unmap(m_constantBuffer.get(), 0);
				} else { success=false; break; }
			}
			auto cb = m_constantBuffer.get();
			context->CSSetConstantBuffers(instanceDesc.constantBufferRegisterIndex, 1, &cb);
		}

		// Bind samplers
		{
			ID3D11SamplerState* samplers[2] = { m_samplers[0].get(), m_samplers[1].get() };
			context->CSSetSamplers(instanceDesc.samplersBaseRegisterIndex, 2, samplers);
		}

		// Walk resource ranges and separate SRVs / UAVs
		const auto& pipeline = instanceDesc.pipelines[dispatch.pipelineIndex];
		uint32_t resIdx = 0;
		uint32_t srvSlot = instanceDesc.resourcesBaseRegisterIndex;
		uint32_t uavSlot = instanceDesc.resourcesBaseRegisterIndex;

		for (uint32_t ri = 0; ri < pipeline.resourceRangesNum; ri++) {
			const auto& range = pipeline.resourceRanges[ri];

			if (range.descriptorType == nrd::DescriptorType::TEXTURE) {
				// SRVs
				ID3D11ShaderResourceView* srvs[kMaxRangeSRVs] = {};
				const uint32_t count = std::min(range.descriptorsNum, kMaxRangeSRVs);
				for (uint32_t k = 0; k < range.descriptorsNum; k++, resIdx++) {
					if (k < count) {
						srvs[k] = static_cast<ID3D11ShaderResourceView*>(ResolveResource(dispatch.resources[resIdx]));
						if (!srvs[k]) success=false;
					} else success=false;
				}
				context->CSSetShaderResources(srvSlot, count, srvs);
				srvSlot += range.descriptorsNum;
			} else {
				// UAVs
				ID3D11UnorderedAccessView* uavs[kMaxRangeUAVs] = {};
				const uint32_t count = std::min(range.descriptorsNum, kMaxRangeUAVs);
				for (uint32_t k = 0; k < range.descriptorsNum; k++, resIdx++) {
					if (k < count) {
						uavs[k] = static_cast<ID3D11UnorderedAccessView*>(ResolveResource(dispatch.resources[resIdx]));
						if (!uavs[k]) success=false;
					} else success=false;
				}
				context->CSSetUnorderedAccessViews(uavSlot, count, uavs, nullptr);
				uavSlot += range.descriptorsNum;
			}
		}

		if (success) context->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);

		// Clear bindings
		{
			static ID3D11ShaderResourceView* nullSRVs[kMaxRangeSRVs] = {};
			static ID3D11UnorderedAccessView* nullUAVs[kMaxRangeUAVs] = {};
			const uint32_t base = instanceDesc.resourcesBaseRegisterIndex;
			context->CSSetShaderResources(base, std::min(srvSlot - base, kMaxRangeSRVs), nullSRVs);
			context->CSSetUnorderedAccessViews(base, std::min(uavSlot - base, kMaxRangeUAVs), nullUAVs, nullptr);
		}
	}

	// Clear named bindings for next frame
	memset(m_namedSRV, 0, sizeof(m_namedSRV));
	memset(m_namedUAV, 0, sizeof(m_namedUAV));

	if (!s_debugLogged) {
		logger::debug("NRD dispatch complete: {} total dispatches, {} pipelines", dispatchNum, m_pipelines.size());
		s_debugLogged = true;
	}

	context->CSSetShader(nullptr, nullptr, 0);
	return success;
}

void NRDReblurIntegration::CreatePoolTextures(uint32_t w, uint32_t h)
{
	auto device = globals::d3d::device;
	const auto& instanceDesc = *nrd::GetInstanceDesc(*m_instance);

	uint32_t totalSize = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;
	m_permanentPool.resize(instanceDesc.permanentPoolSize);
	m_transientPool.resize(instanceDesc.transientPoolSize);

	for (uint32_t i = 0; i < totalSize; i++) {
		bool isPermanent = (i < instanceDesc.permanentPoolSize);
		uint32_t poolIdx = isPermanent ? i : (i - instanceDesc.permanentPoolSize);
		const auto& texDesc = isPermanent ? instanceDesc.permanentPool[poolIdx] : instanceDesc.transientPool[poolIdx];

		DXGI_FORMAT fmt = NRDFormatToDXGI(texDesc.format);
		if (fmt == DXGI_FORMAT_UNKNOWN)
			continue;

		uint32_t tw = (w + texDesc.downsampleFactor - 1) / texDesc.downsampleFactor;
		uint32_t th = (h + texDesc.downsampleFactor - 1) / texDesc.downsampleFactor;

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

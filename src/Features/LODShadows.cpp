#include "LODShadows.h"

#include <bit>

#include "Globals.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LODShadows::Settings, Enabled, UpdateBudgetMs, DepthBias)

namespace
{
	constexpr uint32_t kIndicesPerTriangle = 3;
	constexpr uint32_t kMaximumTriangles = 4194304;
	constexpr float kMinimumUpdateBudgetMs = 0.01f;
	constexpr float kMaximumUpdateBudgetMs = 0.4f;
	constexpr uint32_t kMaximumCacheFrames = 600;
	constexpr float kCenterMargin = 0.125f;
	constexpr float kDirectionThreshold = 0.001f;
	constexpr uint32_t kMaskGroupSize = 8;
	constexpr float kMinimumDirectionLength = 1e-6f;
	constexpr float kCellSize = 4096.0f;
	constexpr int kRasterDepthBias = 1;
	constexpr uint32_t kTraceFrames = 7200;
	constexpr uint32_t kTerrainIdentityTraceFrames = 32;
	constexpr uint64_t kHashSeed = 14695981039346656037ull;
	constexpr uint64_t kHashPrime = 1099511628211ull;

	float ShadowClockRate()
	{
		const auto* calendar = globals::game::calendar;
		const float timeScale = calendar ? calendar->GetTimescale() : Util::EnvironmentControls::kDefaultTimeScale;
		return std::isfinite(timeScale) ? std::max(1.0f, timeScale / Util::EnvironmentControls::kDefaultTimeScale) : 1.0f;
	}

	uint32_t BatchTriangleBudget(uint32_t a_triangleBudget)
	{
		return uint32_t(std::min(double(a_triangleBudget) * ShadowClockRate(), double(std::numeric_limits<uint32_t>::max() / kIndicesPerTriangle)));
	}

	void HashBytes(uint64_t& a_hash, const void* a_data, size_t a_size)
	{
		for (size_t i = 0; i < a_size; ++i)
			a_hash = (a_hash ^ static_cast<const uint8_t*>(a_data)[i]) * kHashPrime;
	}

	DirectX::XMMATRIX WorldMatrix(const RE::NiTransform& a_transform)
	{
		alignas(16) DirectX::XMFLOAT3X4 transform;
		Util::StoreTransform3x4NoScale(transform, a_transform);
		return DirectX::XMMatrixScaling(a_transform.scale, a_transform.scale, a_transform.scale) * DirectX::XMLoadFloat3x4(&transform);
	}
}

std::pair<std::string, std::vector<std::string>> LODShadows::GetFeatureSummary()
{
	return { T("feature.lod_shadows.description", "Casts cached sun and moon shadows from terrain, distant objects and trees."), {} };
}

void LODShadows::LoadSettings(json& a_json)
{
	const bool wasSettingsEnabled = settings.Enabled;
	settings = a_json;
	if (!std::isfinite(settings.UpdateBudgetMs))
		settings.UpdateBudgetMs = Settings{}.UpdateBudgetMs;
	if (!std::isfinite(settings.DepthBias))
		settings.DepthBias = Settings{}.DepthBias;
	settings.UpdateBudgetMs = std::clamp(settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs);
	settings.DepthBias = std::clamp(settings.DepthBias, 0.0f, 64.0f);
	if (wasSettingsEnabled != settings.Enabled)
		resetRequested = true;
}

void LODShadows::SaveSettings(json& a_json) { a_json = settings; }

json LODShadows::GetRuntimeFlags()
{
	return json{ { "RefreshEveryFrame", refreshEveryFrame }, { "HistoryInterpolation", interpolateHistory }, { "HardwareComparison", hardwareComparison } };
}

bool LODShadows::SetRuntimeFlag(std::string_view a_name, bool a_value)
{
	if (a_name == "HardwareComparison") {
		hardwareComparison = a_value;
		return true;
	}
	if (a_name == "HistoryInterpolation") {
		interpolateHistory = a_value;
		return true;
	}
	if (a_name != "RefreshEveryFrame")
		return false;
	refreshEveryFrame = a_value;
	return true;
}

void LODShadows::RestoreDefaultSettings()
{
	settings = {};
	resetRequested = true;
}
void LODShadows::GameLoaded() { resetRequested = true; }
void LODShadows::OnSceneTransitionReset(bool) { resetRequested = true; }

void LODShadows::DrawSettings()
{
	ImGui::Checkbox(T("feature.lod_shadows.enabled", "Enable LOD shadows"), &settings.Enabled);
	ImGui::SliderFloat(T("feature.lod_shadows.update_budget", "GPU update budget (ms)"), &settings.UpdateBudgetMs, kMinimumUpdateBudgetMs, kMaximumUpdateBudgetMs, "%.3f");
	ImGui::SliderFloat(T("feature.lod_shadows.depth_bias", "Depth bias"), &settings.DepthBias, 0.0f, 64.0f, "%.1f");
}

void LODShadows::RegisterUxActions()
{
	FEATURE_COMMAND("rebuild", "Invalidate the LOD depth cache and reset GPU update statistics.", [](Feature* a_self, const json&) {
		static_cast<LODShadows*>(a_self)->resetRequested = true;
	});
	FEATURE_COMMAND("startTrace", "Record up to 7200 exterior render frames of light directions, cascade projections, cache versions, caster counts and CPU/GPU timings. Ignored while recording or writing. No settings are saved.", [](Feature* a_self, const json&) {
		static_cast<LODShadows*>(a_self)->traceRequest = TraceRequest::Start;
	});
	FEATURE_COMMAND("stopTrace", "Stop recording and asynchronously save the collected frames to Captures/LODShadows_<process>_<frame>.json. Query diagnostics for trace state, path and errors.", [](Feature* a_self, const json&) {
		static_cast<LODShadows*>(a_self)->traceRequest = TraceRequest::Stop;
	});
	FEATURE_QUERY("diagnostics", "Return map resolution, terrain replacement state, geometry and caster counts, cache direction errors, build and render frames, trace state and path, plus GPU timings with their submission frames. Timings exclude final mask sampling and forward shadow sampling in lighting passes.", [](const Feature* a_self, const json&) -> json {
		const auto* self = static_cast<const LODShadows*>(a_self);
		std::scoped_lock lock(self->diagnosticsMutex);
		return self->diagnostics;
	});
}

void LODShadows::CompileShaders()
{
	vertexShader = nullptr;
	treeVertexShader = nullptr;
	alphaShader = nullptr;
	for (auto& cache : layouts)
		cache.clear();
	vertexBytecode = Util::CompileShaderBlob(L"Data\\Shaders\\LODShadows\\Caster.hlsl", { { "VSHADER", "1" } }, "vs_5_0");
	treeBytecode = Util::CompileShaderBlob(L"Data\\Shaders\\LODShadows\\Caster.hlsl", { { "VSHADER", "1" }, { "INSTANCED_TREE", "1" } }, "vs_5_0");
	auto pixelBytecode = Util::CompileShaderBlob(L"Data\\Shaders\\LODShadows\\Caster.hlsl", {}, "ps_5_0");
	if (!vertexBytecode || !treeBytecode || !pixelBytecode)
		return;
	auto device = globals::d3d::device;
	DX::ThrowIfFailed(device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(), nullptr, vertexShader.put()));
	DX::ThrowIfFailed(device->CreateVertexShader(treeBytecode->GetBufferPointer(), treeBytecode->GetBufferSize(), nullptr, treeVertexShader.put()));
	DX::ThrowIfFailed(device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(), nullptr, alphaShader.put()));
	Util::SetResourceName(vertexShader.get(), "LODShadows::Caster VS");
	Util::SetResourceName(treeVertexShader.get(), "LODShadows::TreeCaster VS");
	Util::SetResourceName(alphaShader.get(), "LODShadows::AlphaTest PS");
}

void LODShadows::SetupResources()
{
	ready = false;
	traceEnabled = Util::PathHelpers::SafeExists(Util::PathHelpers::GetCommunityShaderPath() / "LODShadows.trace");
	if (traceEnabled) {
		traceFrames.reserve(kTraceFrames);
		logger::info("LOD Shadows trace armed for {} exterior frames", kTraceFrames);
	}
	try {
		auto device = globals::d3d::device;
		D3D11_TEXTURE2D_DESC textureDesc{};
		textureDesc.Width = textureDesc.Height = kResolution;
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = kMapCount * kDepthSlicesPerMap;
		textureDesc.Format = DXGI_FORMAT_R16_TYPELESS;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		DX::ThrowIfFailed(device->CreateTexture2D(&textureDesc, nullptr, depthTexture.put()));
		Util::SetResourceName(depthTexture.get(), "LODShadows::DepthMaps");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = kMapCount * kDepthSlicesPerMap;
		DX::ThrowIfFailed(device->CreateShaderResourceView(depthTexture.get(), &srvDesc, depthSRV.put()));
		Util::SetResourceName(depthSRV.get(), "LODShadows::DepthMaps SRV");
		for (uint32_t i = 0; i < depthViews.size(); ++i) {
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_D16_UNORM;
			desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.FirstArraySlice = i;
			desc.Texture2DArray.ArraySize = 1;
			DX::ThrowIfFailed(device->CreateDepthStencilView(depthTexture.get(), &desc, depthViews[i].put()));
			Util::SetResourceName(depthViews[i].get(), "LODShadows::DepthMap %u DSV", i);
		}
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = true;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
		Util::SetResourceName(depthState.get(), "LODShadows::DepthState");
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = true;
		rasterDesc.DepthBias = kRasterDepthBias;
		for (uint32_t i = 0; i < kMapCount; ++i) {
			DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, rasterizers[i].put()));
			Util::SetResourceName(rasterizers[i].get(), "LODShadows::Rasterizer %u", i);
		}
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, alphaSampler.put()));
		Util::SetResourceName(alphaSampler.get(), "LODShadows::AlphaSampler");
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
		Util::SetResourceName(comparisonSampler.get(), "LODShadows::ComparisonSampler");
		drawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawData>(), "LODShadows::CasterData");
		mapBuffer = std::make_unique<Buffer>(StructuredBufferDesc<MapData>(uint64_t(kMapCount), false, false), nullptr, "LODShadows::MapData");
		D3D11_SHADER_RESOURCE_VIEW_DESC mapView{};
		mapView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		mapView.Buffer.NumElements = kMapCount;
		mapBuffer->CreateSRV(mapView);
		maskBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<std::array<uint32_t, 4>>(), "LODShadows::MaskData");
		for (auto& query : queries) {
			query.batch.Configure(1, "LODShadows::Update");
			if (!query.batch.Preallocate(device))
				return;
		}
		for (auto& query : filterQueries) {
			query.batch.Configure(1, "LODShadows::FilterReceivers");
			if (!query.batch.Preallocate(device))
				return;
		}
		CompileShaders();
		ready = vertexShader && treeVertexShader && alphaShader && comparisonSampler;
		Invalidate();
		Publish();
	} catch (const std::exception& error) {
		logger::error("LOD Shadows resource creation failed: {}", error.what());
	}
}

void LODShadows::ClearShaderCache()
{
	ready = false;
	maskShader.Reset();
	filterShader.Reset();
	maskFailed = false;
	Invalidate();
	try {
		CompileShaders();
		ready = depthTexture && vertexShader && treeVertexShader && alphaShader && comparisonSampler;
	} catch (const std::exception& error) {
		logger::error("LOD Shadows shader creation failed: {}", error.what());
	}
}

void LODShadows::Invalidate()
{
	for (auto& map : maps)
		map.valid = false;
	for (auto& map : previousMaps)
		map.valid = false;
	for (auto& map : completedDepthMaps)
		map.valid = false;
	casters.clear();
	visibleGeometry.clear();
	terrainGeometryCount = 0;
	terrainAvailable = false;
	pendingIndex = -1;
	nextMap = 0;
	clearPending = false;
	maskReady = false;
}

void LODShadows::CreateMaskResources()
{
	const uint32_t eyes = globals::game::isVR ? 2 : 1;
	const uint32_t width = (uint32_t(globals::state->screenSize.x) / eyes + kMaskDownsample - 1) / kMaskDownsample * eyes;
	const uint32_t height = (uint32_t(globals::state->screenSize.y) + kMaskDownsample - 1) / kMaskDownsample;
	if (maskTextures[0] && maskTextures[0]->desc.Width == width && maskTextures[0]->desc.Height == height)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = desc.Format;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	for (uint32_t i = 0; i < maskTextures.size(); ++i) {
		maskTextures[i] = std::make_unique<Texture2D>(desc, i == 0 ? "LODShadows::ReceiverMask" : "LODShadows::FilteredMask");
		maskTextures[i]->CreateSRV(srv);
		maskTextures[i]->CreateUAV(uav);
	}
}

void LODShadows::DrawMask()
{
	CS_GPU_PASS("LODShadows::FilterReceivers");
	auto* depth = Util::GetCurrentSceneDepthSRV(false);
	auto* generate = maskShader.Get(L"Data\\Shaders\\LODShadows\\FilterMask.cs.hlsl", {}, "cs_5_0", "main", "LODShadows::ReceiverMask CS");
	auto* filter = filterShader.Get(L"Data\\Shaders\\LODShadows\\FilterMask.cs.hlsl", { { "FILTER", "1" } }, "cs_5_0", "main", "LODShadows::FilterMask CS");
	if (!depth || !generate || !filter)
		return;
	CreateMaskResources();
	const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	const uint32_t eyes = globals::game::isVR ? 2 : 1;
	maskDimensions = { (uint32_t(renderSize.x) / eyes + kMaskDownsample - 1) / kMaskDownsample * eyes,
		(uint32_t(renderSize.y) + kMaskDownsample - 1) / kMaskDownsample };
	if (!maskDimensions[0] || !maskDimensions[1])
		return;
	maskBuffer->Update(std::array<uint32_t, 4>{ maskDimensions[0], maskDimensions[1], 0, 0 });
	auto* context = globals::d3d::context;
	auto& query = filterQueries[filterQueryCursor];
	const bool measured = !query.pending && query.batch.BeginBatch(globals::d3d::device, context);
	if (measured)
		query.batch.BeginInterval(globals::d3d::device, context);
	const SKSE::stl::scope_exit finishTiming([&]() noexcept {
		if (!measured)
			return;
		query.batch.CommitInterval(context);
		query.batch.EndBatch(context);
		query.pending = true;
		query.frame = globals::game::graphicsState->GetFrameCount();
		filterQueryCursor = (filterQueryCursor + 1) % kQueryFrames;
	});
	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	winrt::com_ptr<ID3D11Buffer> savedBuffer;
	winrt::com_ptr<ID3D11SamplerState> savedSampler;
	winrt::com_ptr<ID3D11UnorderedAccessView> savedUAV;
	std::array<ID3D11ShaderResourceView*, 2> savedViews{};
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(0, 1, savedBuffer.put());
	context->CSGetSamplers(0, 1, savedSampler.put());
	context->CSGetUnorderedAccessViews(0, 1, savedUAV.put());
	context->CSGetShaderResources(0, uint32_t(savedViews.size()), savedViews.data());
	const SKSE::stl::scope_exit restore([&]() noexcept {
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(0, uint32_t(savedViews.size()), savedViews.data());
		for (auto* view : savedViews)
			if (view)
				view->Release();
		auto* buffer = savedBuffer.get();
		auto* sampler = savedSampler.get();
		auto* uav = savedUAV.get();
		context->CSSetConstantBuffers(0, 1, &buffer);
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(savedShader.get(), nullptr, 0);
	});
	ID3D11ShaderResourceView* nullView = nullptr;
	context->PSSetShaderResources(kMaskSlot, 1, &nullView);
	auto* buffer = maskBuffer->CB();
	auto* sampler = alphaSampler.get();
	context->CSSetConstantBuffers(0, 1, &buffer);
	context->CSSetSamplers(0, 1, &sampler);
	const uint32_t groupsX = (maskDimensions[0] + kMaskGroupSize - 1) / kMaskGroupSize;
	const uint32_t groupsY = (maskDimensions[1] + kMaskGroupSize - 1) / kMaskGroupSize;
	std::array<ID3D11ShaderResourceView*, 2> views{ depth, nullptr };
	auto* uav = maskTextures[0]->uav.get();
	context->CSSetShaderResources(0, uint32_t(views.size()), views.data());
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(generate, nullptr, 0);
	context->Dispatch(groupsX, groupsY, 1);
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	views = { nullptr, maskTextures[0]->srv.get() };
	context->CSSetShaderResources(0, uint32_t(views.size()), views.data());
	uav = maskTextures[1]->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(filter, nullptr, 0);
	context->Dispatch(groupsX, groupsY, 1);
	maskReady = true;
}

void LODShadows::Prepass()
{
	maskReady = false;
	if (ready && settings.Enabled && !maskFailed && globals::state->HasDirectionalShadows()) {
		try {
			DrawMask();
		} catch (const std::exception& error) {
			maskFailed = true;
			logger::error("LOD Shadows receiver filtering failed: {}", error.what());
		}
	}
	Publish();
	auto* view = maskReady ? maskTextures[1]->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(kMaskSlot, 1, &view);
}

void LODShadows::Collect(RE::NiAVObject* a_object, bool a_root, bool a_loadedObject)
{
	if (!a_object)
		return;
	if (!a_root && a_object->GetAppCulled()) {
		++hiddenNodes;
		return;
	}
	if (auto node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			Collect(child.get(), false, a_loadedObject);
	} else if (auto geometry = a_object->AsGeometry()) {
		if (a_loadedObject) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property || !property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows) ||
				!collectedGeometry.insert(geometry).second)
				return;
		}
		visibleGeometry.emplace_back(geometry);
		HashBytes(sceneHash, &geometry, sizeof(geometry));
		HashBytes(sceneHash, &geometry->world, sizeof(geometry->world));
		const auto lodMode = geometry->GetLODMode();
		HashBytes(sceneHash, &lodMode, sizeof(lodMode));
		auto& data = geometry->GetGeometryRuntimeData();
		HashBytes(sceneHash, &data.rendererData, sizeof(data.rendererData));
		if (auto segmented = geometry->AsSubIndexTriShape()) {
			auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
			if (segments.segmentData) {
				for (uint32_t i = 0; i < segments.numSegments; ++i) {
					const auto& range = segments.segmentData[i];
					if (range.unkTriCount) {
						enabledSegments += range.unkFlags != 0;
						disabledSegments += range.unkFlags == 0;
					}
					HashBytes(sceneHash, &range.index, sizeof(range.index));
					HashBytes(sceneHash, &range.numTris, sizeof(range.numTris));
					HashBytes(sceneHash, &range.flags, sizeof(range.flags));
				}
			}
		} else if (geometry->GetType() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape) {
			auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(geometry)->GetMultiStreamTrishapeRuntimeData();
			HashBytes(sceneHash, &instances.instanceCount, sizeof(instances.instanceCount));
			for (auto group : instances.instanceGroups) {
				if (group && group->vertexBuffer) {
					HashBytes(sceneHash, &group->vertexBuffer->buffer, sizeof(group->vertexBuffer->buffer));
					HashBytes(sceneHash, &group->instanceCount, sizeof(group->instanceCount));
				}
			}
		}
	}
}

void LODShadows::CollectTerrain()
{
	Collect(RE::BGSTerrainManager::GetLODLandRoot().get(), true);
	terrainAvailable = !visibleGeometry.empty();
	const auto lodTerrainCount = visibleGeometry.size();
	landBounds = {};
	const auto* tes = globals::game::tes;
	const auto* grid = tes ? tes->gridCells : nullptr;
	if (grid && grid->cells) {
		landBounds = { (tes->currentGridX + 0.5f) * kCellSize, (tes->currentGridY + 0.5f) * kCellSize,
			grid->length * kCellSize * 0.5f, grid->length * kCellSize * 0.5f };
		for (uint32_t x = 0; x < grid->length; ++x) {
			for (uint32_t y = 0; y < grid->length; ++y) {
				const auto* cell = grid->GetCell(x, y);
				if (!cell || !cell->IsAttached() || !cell->IsExteriorCell())
					continue;
				const auto* land = cell->GetRuntimeData().cellLand;
				if (land && land->loadedData)
					for (auto* mesh : land->loadedData->mesh)
						Collect(mesh);
			}
		}
	}
	terrainGeometryCount = uint32_t(visibleGeometry.size());
	loadedTerrainGeometryCount = uint32_t(visibleGeometry.size() - lodTerrainCount);
	HashBytes(sceneHash, &landBounds, sizeof(landBounds));
	if (traceEnabled) {
		terrainHashes.fill(kHashSeed);
		for (const auto& geometry : visibleGeometry) {
			const auto* pointer = geometry.get();
			HashBytes(terrainHashes[0], &pointer, sizeof(pointer));
			HashBytes(terrainHashes[1], &geometry->world, sizeof(geometry->world));
			const auto mode = geometry->GetLODMode();
			HashBytes(terrainHashes[2], &mode, sizeof(mode));
			const auto& data = geometry->GetGeometryRuntimeData();
			HashBytes(terrainHashes[3], &data.rendererData, sizeof(data.rendererData));
			uint64_t geometryHash = kHashSeed;
			uint64_t rendererHash = kHashSeed;
			HashBytes(geometryHash, &pointer, sizeof(pointer));
			HashBytes(rendererHash, &data.rendererData, sizeof(data.rendererData));
			terrainHashes[6] += geometryHash;
			terrainHashes[7] += rendererHash;
			if (auto* segmented = geometry->AsSubIndexTriShape()) {
				const auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
				if (segments.segmentData)
					for (uint32_t i = 0; i < segments.numSegments; ++i) {
						const auto& range = segments.segmentData[i];
						HashBytes(terrainHashes[4], &range.index, sizeof(range.index));
						HashBytes(terrainHashes[4], &range.numTris, sizeof(range.numTris));
						HashBytes(terrainHashes[4], &range.flags, sizeof(range.flags));
					}
			}
		}
		HashBytes(terrainHashes[5], &landBounds, sizeof(landBounds));
	}
}

void LODShadows::CollectLoadedObjects()
{
	collectedGeometry.clear();
	for (const auto& geometry : visibleGeometry)
		collectedGeometry.insert(geometry.get());
	if (auto* tes = globals::game::tes) {
		tes->ForEachReference([&](RE::TESObjectREFR* reference) {
			const auto* base = reference ? reference->GetBaseObject() : nullptr;
			if (base && base->Is(RE::FormType::Static, RE::FormType::StaticCollection, RE::FormType::Tree) &&
				!reference->IsDisabled() && !reference->IsDeleted())
				Collect(reference->Get3D(), false, true);
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
}

void LODShadows::AddCaster(RE::BSGeometry* a_geometry)
{
	auto shape = a_geometry->AsTriShape();
	auto& data = a_geometry->GetGeometryRuntimeData();
	auto mesh = static_cast<RE::BSGraphics::TriShape*>(data.rendererData);
	auto property = data.shaderProperty.get();
	if (!shape || !mesh || !mesh->vertexBuffer || !mesh->indexBuffer || data.skinInstance || !property) {
		++unsupportedCasters;
		return;
	}
	const auto type = a_geometry->GetType();
	const bool instanced = type == RE::BSGeometry::Type::kMultiStreamInstanceTriShape;
	if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kSubIndexTriShape &&
		type != RE::BSGeometry::Type::kMeshLODTriShape && type != RE::BSGeometry::Type::kMultiIndexTriShape && !instanced) {
		++unsupportedCasters;
		return;
	}
	Caster caster;
	caster.lodLandscape = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODLandscape);
	caster.geometry.reset(a_geometry);
	caster.vertices.copy_from(Util::AsReal(mesh->vertexBuffer));
	caster.indices.copy_from(Util::AsReal(mesh->indexBuffer));
	caster.vertexDescriptor = std::bit_cast<uint64_t>(mesh->vertexDesc);
	constexpr uint64_t kStrideMask = 0xf;
	caster.stride = uint32_t(caster.vertexDescriptor & kStrideMask) * sizeof(float);
	caster.indexCount = shape->GetTrishapeRuntimeData().triangleCount * kIndicesPerTriangle;
	if (type == RE::BSGeometry::Type::kMeshLODTriShape) {
		constexpr uint8_t kSingleLevel = 0x80;
		const auto mode = a_geometry->GetLODMode();
		const uint32_t level = mode & ~kSingleLevel;
		const auto& counts = static_cast<RE::BSMeshLODTriShape*>(a_geometry)->GetMeshLODTriShapeRuntimeData().lodTriangleCounts;
		if (level > std::size(counts) || ((mode & kSingleLevel) && level == std::size(counts))) {
			++unsupportedCasters;
			return;
		}
		uint32_t prefix = 0;
		for (uint32_t i = 0; i < level; ++i)
			prefix += counts[i] * kIndicesPerTriangle;
		caster.firstIndex = mode & kSingleLevel ? prefix : 0;
		caster.indexCount = mode & kSingleLevel ? counts[level] * kIndicesPerTriangle : prefix;
	}
	caster.transform = a_geometry->world;
	caster.bound = a_geometry->worldBound;
	if (caster.stride < sizeof(float) * 3) {
		++unsupportedCasters;
		return;
	}
	if (!caster.indexCount)
		return;
	if (auto material = property->material) {
		caster.uvTransform = { material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
		if (material->GetType() == RE::BSShaderMaterial::Type::kLighting)
			caster.materialAlpha = static_cast<RE::BSLightingShaderMaterialBase*>(material)->materialAlpha;
	}
	if (auto alpha = data.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		caster.alphaThreshold = alpha->alphaThreshold / 255.0f;
	if (instanced)
		caster.alphaThreshold = std::max(caster.alphaThreshold, 0.5f);
	if (caster.alphaThreshold > 0) {
		auto texture = property->GetBaseTexture();
		if (!mesh->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !texture || !texture->rendererTexture || !texture->rendererTexture->resourceView) {
			++unsupportedCasters;
			return;
		}
		caster.texture.copy_from(Util::AsReal(texture->rendererTexture->resourceView));
	}
	D3D11_BUFFER_DESC indexDesc{};
	caster.indices->GetDesc(&indexDesc);
	if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
		++unsupportedCasters;
		return;
	}
	if (instanced) {
		auto& instances = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry)->GetMultiStreamTrishapeRuntimeData();
		caster.instanceStride = instances.instanceSize * sizeof(uint16_t);
		if (caster.instanceStride < sizeof(uint16_t) * 8) {
			++unsupportedCasters;
			return;
		}
		for (auto group : instances.instanceGroups) {
			if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
				continue;
			caster.instances.copy_from(Util::AsReal(group->vertexBuffer->buffer));
			caster.instanceCount = uint32_t(std::min(size_t(group->instanceCount), group->vertexBuffer->byteWidth / caster.instanceStride));
			if (caster.instanceCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (auto segmented = a_geometry->AsSubIndexTriShape()) {
		auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
		if (!segments.segmentData) {
			++unsupportedCasters;
			return;
		}
		for (uint32_t i = 0; i < (segments.nonSegmented ? 1u : segments.numSegments); ++i) {
			const auto& range = segments.segmentData[i];
			if (!range.flags)
				continue;
			caster.firstIndex = segments.nonSegmented ? 0 : range.index;
			caster.indexCount = (segments.nonSegmented ? shape->GetTrishapeRuntimeData().triangleCount : range.numTris) * kIndicesPerTriangle;
			if ((uint64_t(caster.firstIndex) + caster.indexCount) * sizeof(uint16_t) > indexDesc.ByteWidth) {
				++unsupportedCasters;
				continue;
			}
			if (caster.indexCount && Intersects(caster))
				casters.push_back(caster);
		}
	} else if (Intersects(caster)) {
		casters.push_back(std::move(caster));
	}
}

bool LODShadows::Intersects(const Caster& a_caster) const
{
	using namespace DirectX;
	const auto& center = a_caster.bound.center;
	const auto point = XMVector3TransformCoord(XMVectorSet(center.x, center.y, center.z, 1), XMLoadFloat4x4(&pendingMap.data.worldToClip));
	const float radiusXY = a_caster.bound.radius / kHalfExtents[pendingIndex];
	const float radiusZ = a_caster.bound.radius / (kDepthExtent * 2);
	return std::abs(XMVectorGetX(point)) <= 1 + radiusXY && std::abs(XMVectorGetY(point)) <= 1 + radiusXY && XMVectorGetZ(point) >= -radiusZ && XMVectorGetZ(point) <= 1 + radiusZ;
}

void LODShadows::BeginMap(uint32_t a_index, const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction)
{
	using namespace DirectX;
	pendingIndex = a_index;
	pendingMap = {};
	pendingMap.center = a_center;
	pendingMap.direction = a_direction;
	for (uint32_t slice = a_index * kDepthSlicesPerMap; slice < (a_index + 1) * kDepthSlicesPerMap; ++slice) {
		if ((!maps[a_index].valid || slice != maps[a_index].slice) &&
			(!previousMaps[a_index].valid || slice != previousMaps[a_index].slice)) {
			pendingMap.slice = slice;
			break;
		}
	}
	assert(!maps[a_index].valid || pendingMap.slice != maps[a_index].slice);
	assert(!previousMaps[a_index].valid || pendingMap.slice != previousMaps[a_index].slice);
	pendingMap.scene = sceneHash;
	pendingMap.frame = globals::game::graphicsState->GetFrameCount();
	pendingMap.transitionGeneration = transitionGeneration;
	pendingMap.terrainValid = terrainAvailable;
	pendingLandBounds = landBounds;
	const auto direction = XMVectorSet(a_direction.x, a_direction.y, a_direction.z, 0);
	const auto up = std::abs(a_direction.z) > 0.99f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(0, 0, 1, 0);
	const auto rotation = XMMatrixLookToLH(XMVectorZero(), direction, up);
	const auto center = XMVector3TransformCoord(XMVectorSet(a_center.x, a_center.y, a_center.z, 1), rotation);
	const float extent = kHalfExtents[a_index];
	const float texel = extent * 2 / kResolution;
	const auto translation = XMMatrixTranslation(-std::round(XMVectorGetX(center) / texel) * texel, -std::round(XMVectorGetY(center) / texel) * texel, -XMVectorGetZ(center) + kDepthExtent);
	XMStoreFloat4x4(&pendingMap.data.worldToClip, rotation * translation * XMMatrixOrthographicLH(extent * 2, extent * 2, 0, kDepthExtent * 2));
	casters.clear();
	unsupportedCasters = 0;
	uint32_t sourceIndex = 0;
	uint32_t sourceEnd = geometryCounts[0];
	const bool captureCasters = traceEnabled && traceCasterSnapshots[a_index].is_null();
	json casterSnapshot = captureCasters ? json::array() : json();
	for (uint32_t i = 0; i < visibleGeometry.size(); ++i) {
		const auto unsupported = unsupportedCasters;
		const auto firstCaster = casters.size();
		AddCaster(visibleGeometry[i].get());
		if (traceEnabled) {
			while (i >= sourceEnd && sourceIndex + 1 < geometryCounts.size())
				sourceEnd += geometryCounts[++sourceIndex];
			for (size_t sourceCaster = firstCaster; sourceCaster < casters.size(); ++sourceCaster) {
				const auto& caster = casters[sourceCaster];
				pendingMap.sourceTriangles[sourceIndex] += uint64_t(caster.indexCount / kIndicesPerTriangle) * caster.instanceCount;
				++pendingMap.sourceCasters[sourceIndex];
				if (captureCasters) {
					const auto& casterCenter = caster.bound.center;
					casterSnapshot.push_back({ { "source", sourceIndex }, { "geometry", reinterpret_cast<uintptr_t>(caster.geometry.get()) },
						{ "center", { casterCenter.x, casterCenter.y, casterCenter.z } }, { "radius", caster.bound.radius },
						{ "triangles", uint64_t(caster.indexCount / kIndicesPerTriangle) * caster.instanceCount },
						{ "firstIndex", caster.firstIndex }, { "indexCount", caster.indexCount }, { "alphaThreshold", caster.alphaThreshold } });
				}
			}
		}
		if (i < terrainGeometryCount && unsupportedCasters != unsupported)
			pendingMap.terrainValid = false;
	}
	if (captureCasters) {
		auto& snapshot = traceCasterSnapshots[a_index];
		snapshot = { { "frame", pendingMap.frame }, { "casters", std::move(casterSnapshot) } };
		if (const auto* camera = RE::Main::WorldRootCamera()) {
			const auto& frustum = camera->GetRuntimeData2().viewFrustum;
			const auto& position = camera->world.translate;
			snapshot["camera"] = { { "position", { position.x, position.y, position.z } },
				{ "rotation", json::array() }, { "frustum", { frustum.fLeft, frustum.fRight, frustum.fBottom, frustum.fTop, frustum.fNear, frustum.fFar } },
				{ "orthographic", frustum.bOrtho } };
			for (const auto& row : camera->world.rotate.entry)
				for (float value : row)
					snapshot["camera"]["rotation"].push_back(value);
			const auto& projection = globals::game::isVR ? camera->GetVRRuntimeData().worldToCam : camera->GetRuntimeData().worldToCam;
			snapshot["camera"]["worldToClip"] = json::array();
			for (const auto& row : projection)
				for (float value : row)
					snapshot["camera"]["worldToClip"].push_back(value);
		}
	}
	std::ranges::sort(casters, {}, [](const Caster& caster) { return caster.alphaThreshold > 0; });
	casterIndex = 0;
	indexOffset = instanceOffset = 0;
	clearPending = true;
}

ID3D11InputLayout* LODShadows::GetLayout(const Caster& a_caster)
{
	const bool instanced = bool(a_caster.instances);
	auto& cache = layouts[instanced];
	if (auto it = cache.find(a_caster.vertexDescriptor); it != cache.end())
		return it->second.get();
	const auto desc = std::bit_cast<RE::BSGraphics::VertexDesc>(a_caster.vertexDescriptor);
	std::array<D3D11_INPUT_ELEMENT_DESC, 4> elements{
		D3D11_INPUT_ELEMENT_DESC{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) ? desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0) : 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		D3D11_INPUT_ELEMENT_DESC{ "TEXCOORD", 5, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1 }
	};
	auto blob = instanced ? treeBytecode.get() : vertexBytecode.get();
	winrt::com_ptr<ID3D11InputLayout> layout;
	DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(elements.data(), instanced ? 4 : 2, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put()));
	Util::SetResourceName(layout.get(), "LODShadows::CasterLayout");
	return cache.emplace(a_caster.vertexDescriptor, std::move(layout)).first->second.get();
}

void LODShadows::ResolveTimings()
{
	for (auto& query : filterQueries) {
		if (!query.pending)
			continue;
		const auto status = query.batch.TryResolve(globals::d3d::context, [&](uint32_t, uint64_t ticks, uint64_t frequency) {
			lastFilterMs = double(ticks) * 1000.0 / double(frequency);
			lastFilterFrame = query.frame;
		});
		if (status != Util::TimestampQueryBatch::Status::NotReady) {
			query.pending = false;
			query.batch.Reset();
		}
	}
	for (auto& query : queries) {
		if (!query.pending)
			continue;
		const auto status = query.batch.TryResolve(globals::d3d::context, [&](uint32_t, uint64_t a_ticks, uint64_t a_frequency) {
			const double elapsedMs = double(a_ticks) * 1000.0 / double(a_frequency);
			if (query.immediateRefresh) {
				lastRefreshMs = elapsedMs;
				lastRefreshFrame = query.frame;
				maxRefreshMs = std::max(maxRefreshMs, elapsedMs);
				return;
			}
			lastUpdateMs = elapsedMs;
			lastUpdateFrame = query.frame;
			maxUpdateMs = std::max(maxUpdateMs, lastUpdateMs);
			++timingSamples;
			if (query.triangles && lastUpdateMs > 0 && (query.budgetLimited || lastUpdateMs > settings.UpdateBudgetMs)) {
				const double estimate = query.triangles * settings.UpdateBudgetMs / lastUpdateMs;
				triangleBudget = uint32_t(std::clamp(std::min(estimate, triangleBudget * 1.1), 256.0, double(kMaximumTriangles)));
			}
		});
		if (status != Util::TimestampQueryBatch::Status::NotReady) {
			query.pending = false;
			query.batch.Reset();
		}
	}
}

void LODShadows::UpdateMaps(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, bool a_refreshImmediately)
{
	if (a_refreshImmediately) {
		for (auto& map : completedDepthMaps)
			map.valid = false;
	} else {
		PublishCompletedMaps(false);
	}
	auto& query = queries[queryCursor];
	if (query.pending && !a_refreshImmediately)
		return;
	if (!a_refreshImmediately && pendingIndex < 0) {
		RE::BSSpinLockGuard lock(RE::BGSTerrainManager::GetCameraTerrainLock());
		std::array<uint32_t, kMapCount> candidates{};
		uint32_t candidateCount = 0;
		std::array<double, kMapCount> deadlines{};
		for (uint32_t i = 0; i < kMapCount; ++i) {
			const auto index = (nextMap + i) % kMapCount;
			if (completedDepthMaps[index].valid)
				continue;
			const auto& map = maps[index];
			const bool blending = interpolateHistory && previousMaps[index].valid;
			if (!NeedsUpdate(index, a_center, a_direction) && (!blending || map.direction == a_direction))
				continue;
			deadlines[index] = map.valid ? map.completedTimeMs + (blending ? map.blendDurationMs : 0.0) : 0.0;
			candidates[candidateCount++] = index;
		}
		std::sort(candidates.begin(), candidates.begin() + candidateCount, [&](uint32_t left, uint32_t right) {
			return deadlines[left] < deadlines[right];
		});
		double buildTimeMs = 0;
		const auto batchTriangles = BatchTriangleBudget(triangleBudget);
		for (uint32_t i = 0; i < candidateCount; ++i) {
			const auto index = candidates[i];
			const auto& map = maps[index];
			const double workloadTimeMs = std::ceil(double(map.triangleCount) / batchTriangles) * frameDurationMs;
			buildTimeMs += std::max(map.buildDurationMs, workloadTimeMs) + frameDurationMs;
			if (frameTimeMs + buildTimeMs >= deadlines[index]) {
				BeginMap(candidates[0], a_center, a_direction);
				break;
			}
		}
		if (pendingIndex < 0)
			return;
	}
	CS_GPU_PASS_SELECT(a_refreshImmediately, "LODShadows::FullRefresh", "LODShadows::Update");
	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	const bool measured = !query.pending && query.batch.BeginBatch(device, context);
	if (measured)
		query.batch.BeginInterval(device, context);
	const SKSE::stl::scope_exit finishTiming([&]() noexcept {
		if (!measured)
			return;
		query.batch.CommitInterval(context);
		query.batch.EndBatch(context);
		query.pending = true;
		query.frame = globals::game::graphicsState->GetFrameCount();
		query.triangles = lastTriangles;
		query.budgetLimited = pendingIndex >= 0;
		query.immediateRefresh = a_refreshImmediately;
		queryCursor = (queryCursor + 1) % kQueryFrames;
	});
	if (a_refreshImmediately) {
		for (uint32_t i = 0; i < kMapCount; ++i) {
			{
				RE::BSSpinLockGuard lock(RE::BGSTerrainManager::GetCameraTerrainLock());
				BeginMap(i, a_center, a_direction);
			}
			RenderBatch(true);
		}
	} else {
		RenderBatch(false);
	}
}

void LODShadows::RenderBatch(bool a_refreshImmediately)
{
	auto context = globals::d3d::context;
	const auto firstTriangle = lastTriangles;
	if (!a_refreshImmediately)
		pendingMap.buildDurationMs += frameDurationMs;
	const auto batchTriangles = BatchTriangleBudget(triangleBudget);
	{
		Util::FullscreenPassScope scope(context, 1, 0, 1);
		winrt::com_ptr<ID3D11Buffer> savedVSConstant;
		context->VSGetConstantBuffers(0, 1, savedVSConstant.put());
		const SKSE::stl::scope_exit restoreVS([&]() noexcept {
			auto saved = savedVSConstant.get();
			context->VSSetConstantBuffers(0, 1, &saved);
		});
		ID3D11ShaderResourceView* nullView = nullptr;
		context->PSSetShaderResources(kDepthSlot, 1, &nullView);
		context->CSSetShaderResources(kDepthSlot, 1, &nullView);
		auto depth = depthViews[pendingMap.slice].get();
		context->OMSetRenderTargets(0, nullptr, depth);
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->OMSetBlendState(nullptr, nullptr, ~0u);
		context->RSSetState(rasterizers[pendingIndex].get());
		const D3D11_VIEWPORT viewport{ 0, 0, float(kResolution), float(kResolution), 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		auto buffer = drawBuffer->CB();
		context->VSSetConstantBuffers(0, 1, &buffer);
		context->PSSetConstantBuffers(0, 1, &buffer);
		auto sampler = alphaSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		if (clearPending) {
			context->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1, 0);
			clearPending = false;
		}
		while (casterIndex < casters.size() && (a_refreshImmediately || lastTriangles < batchTriangles)) {
			const auto& caster = casters[casterIndex];
			DrawData data;
			DirectX::XMStoreFloat4x4(&data.worldToClip, WorldMatrix(caster.transform) * DirectX::XMLoadFloat4x4(&pendingMap.data.worldToClip));
			DirectX::XMStoreFloat4x4(&data.world, DirectX::XMMatrixTranspose(WorldMatrix(caster.transform)));
			data.uvTransform = caster.uvTransform;
			data.alpha = { caster.alphaThreshold, caster.materialAlpha, caster.lodLandscape ? 1.0f : 0.0f, 0 };
			data.landBounds = pendingLandBounds;
			drawBuffer->Update(data);
			context->VSSetShader(caster.instances ? treeVertexShader.get() : vertexShader.get(), nullptr, 0);
			context->PSSetShader(caster.alphaThreshold > 0 ? alphaShader.get() : nullptr, nullptr, 0);
			auto texture = caster.texture.get();
			context->PSSetShaderResources(0, 1, &texture);
			context->IASetInputLayout(GetLayout(caster));
			ID3D11Buffer* vertices[]{ caster.vertices.get(), caster.instances.get() };
			const UINT strides[]{ caster.stride, caster.instanceStride };
			const UINT offsets[]{ 0, 0 };
			context->IASetVertexBuffers(0, caster.instances ? 2 : 1, vertices, strides, offsets);
			context->IASetIndexBuffer(caster.indices.get(), DXGI_FORMAT_R16_UINT, 0);
			const uint32_t count = a_refreshImmediately ? caster.indexCount - indexOffset :
			                                              std::min(caster.indexCount - indexOffset, (batchTriangles - lastTriangles) * kIndicesPerTriangle);
			const uint32_t instances = caster.instances && indexOffset == 0 && count == caster.indexCount ?
			                               (a_refreshImmediately ? caster.instanceCount - instanceOffset :
																   std::min(caster.instanceCount - instanceOffset, (batchTriangles - lastTriangles) / (count / kIndicesPerTriangle))) :
			                               1;
			if (caster.instances)
				context->DrawIndexedInstanced(count, instances, caster.firstIndex + indexOffset, 0, instanceOffset);
			else
				context->DrawIndexed(count, caster.firstIndex + indexOffset, 0);
			++lastDraws;
			lastTriangles += count / kIndicesPerTriangle * instances;
			indexOffset += count;
			if (indexOffset == caster.indexCount) {
				indexOffset = 0;
				instanceOffset += instances;
				if (instanceOffset == caster.instanceCount) {
					instanceOffset = 0;
					++casterIndex;
				}
			}
		}
	}
	pendingMap.triangleCount += lastTriangles - firstTriangle;
	if (casterIndex == casters.size()) {
		pendingMap.valid = true;
		completedDepthMaps[pendingIndex] = pendingMap;
		nextMap = (pendingIndex + 1) % kMapCount;
		pendingIndex = -1;
		casters.clear();
		PublishCompletedMaps(a_refreshImmediately);
	}
}

void LODShadows::PublishCompletedMaps(bool a_refreshImmediately)
{
	for (uint32_t i = 0; i < kMapCount; ++i) {
		auto& completed = completedDepthMaps[i];
		if (!completed.valid || (!a_refreshImmediately && GetHistoryBlend(i) < 1.0f))
			continue;
		previousMaps[i] = maps[i];
		previousMaps[i].valid &= !a_refreshImmediately;
		completed.completedTimeMs = frameTimeMs;
		completed.blendDurationMs = kHistoryIntervalsMs[i] / ShadowClockRate();
		maps[i] = completed;
		completed.valid = false;
		++completedMaps;
	}
}

float LODShadows::GetHistoryBlend(uint32_t a_index) const
{
	const auto& map = maps[a_index];
	if (!interpolateHistory || !previousMaps[a_index].valid || map.blendDurationMs <= 0)
		return 1.0f;
	return float(std::clamp((frameTimeMs - map.completedTimeMs) / map.blendDurationMs, 0.0, 1.0));
}

bool LODShadows::NeedsUpdate(uint32_t a_index, const RE::NiPoint3& a_position, const RE::NiPoint3& a_direction) const
{
	const auto& map = maps[a_index];
	return !map.valid || map.scene != sceneHash || map.transitionGeneration != transitionGeneration ||
	       (map.center - a_position).Length() > kHalfExtents[a_index] * kCenterMargin ||
	       (map.direction - a_direction).Length() > kDirectionThreshold || globals::game::graphicsState->GetFrameCount() - map.frame > kMaximumCacheFrames;
}

bool LODShadows::ReplacesTerrainShadows() const
{
	if (!ready || !settings.Enabled || resetRequested || !globals::state->HasDirectionalShadows())
		return false;
	const auto* manager = RE::BGSTerrainManager::GetActiveManager();
	return manager && manager->worldSpace && manager->worldSpace->GetFormID() == worldSpaceID && !manager->mapMode &&
	       !RE::BGSTerrainManager::LoadScreenUp() && std::ranges::all_of(maps, [](const Map& map) { return map.valid && map.terrainValid; });
}

void LODShadows::Publish()
{
	static_assert(kDataSlot == kDepthSlot + 1);
	if (!mapBuffer)
		return;
	std::array<MapData, kMapCount> data{};
	uint32_t validMaps = 0;
	for (uint32_t i = 0; i < kMapCount; ++i) {
		data[i] = maps[i].data;
		const bool valid = ready && settings.Enabled && maps[i].valid;
		data[i].parameters = { float(maps[i].slice), valid ? 1.0f : 0.0f, settings.DepthBias / (kDepthExtent * 2), 0 };
		data[i].filter = { 1.0f / kResolution, kHalfExtents[i] * 2 / kResolution, kDepthExtent * 2, 0 };
		data[i].historyWorldToClip = previousMaps[i].data.worldToClip;
		data[i].history = { float(previousMaps[i].slice), GetHistoryBlend(i), previousMaps[i].valid ? 1.0f : 0.0f, hardwareComparison ? 1.0f : 0.0f };
		validMaps += valid;
	}
	data[0].parameters.w = float(validMaps);
	data[1].parameters.w = ReplacesTerrainShadows() ? 1.0f : 0.0f;
	data[0].filter.w = maskReady ? 1.0f : 0.0f;
	data[1].filter.w = float(maskDimensions[0]);
	data[2].filter.w = float(maskDimensions[1]);
	auto context = globals::d3d::context;
	context->UpdateSubresource(mapBuffer->resource.get(), 0, nullptr, data.data(), 0, 0);
	ID3D11ShaderResourceView* views[]{ depthSRV.get(), mapBuffer->srv.get() };
	context->PSSetShaderResources(kDepthSlot, 2, views);
	context->CSSetShaderResources(kDepthSlot, 2, views);
	auto* sampler = comparisonSampler.get();
	context->PSSetSamplers(kComparisonSamplerSlot, 1, &sampler);
	std::scoped_lock lock(diagnosticsMutex);
	diagnostics = { { "ready", ready }, { "enabled", settings.Enabled }, { "validMaps", validMaps }, { "completedMaps", completedMaps },
		{ "geometryCount", visibleGeometry.size() }, { "pendingCasters", casters.size() - casterIndex * (pendingIndex >= 0) }, { "unsupportedCasters", unsupportedCasters },
		{ "updateDraws", lastDraws }, { "updateTriangles", lastTriangles }, { "triangleBudget", triangleBudget },
		{ "lastUpdateGpuMs", lastUpdateMs }, { "maxUpdateGpuMs", maxUpdateMs }, { "timingSamples", timingSamples },
		{ "lastFullRefreshGpuMs", lastRefreshMs }, { "maxFullRefreshGpuMs", maxRefreshMs }, { "refreshEveryFrame", refreshEveryFrame } };
	diagnostics["mapResolution"] = kResolution;
	diagnostics["historyInterpolation"] = interpolateHistory;
	diagnostics["hardwareComparison"] = hardwareComparison;
	diagnostics["historyWeights"] = { data[0].history.y, data[1].history.y, data[2].history.y };
	diagnostics["completedDepthWaiting"] = { completedDepthMaps[0].valid, completedDepthMaps[1].valid, completedDepthMaps[2].valid };
	diagnostics["terrainGeometryCount"] = terrainGeometryCount;
	diagnostics["loadedTerrainGeometryCount"] = loadedTerrainGeometryCount;
	diagnostics["loadedObjectGeometryCount"] = geometryCounts[3];
	diagnostics["replacesTerrainShadows"] = data[1].parameters.w > 0;
	diagnostics["lastFilterGpuMs"] = lastFilterMs;
	diagnostics["filteredMaskReady"] = maskReady;
	diagnostics["filterResolution"] = maskDimensions;
	diagnostics["renderFrame"] = globals::game::graphicsState->GetFrameCount();
	diagnostics["gpuSubmissionFrames"] = { lastUpdateFrame, lastFilterFrame, lastRefreshFrame };
	diagnostics["pendingIndex"] = pendingIndex;
	diagnostics["pendingBuildFrame"] = pendingIndex >= 0 ? json(pendingMap.frame) : json(nullptr);
	diagnostics["mapBuildFrames"] = { maps[0].frame, maps[1].frame, maps[2].frame };
	diagnostics["trace"] = { { "recording", traceEnabled }, { "frames", traceFrames.size() },
		{ "writing", traceWriting.load() }, { "path", tracePath }, { "error", traceError } };
	const auto scene = globals::game::smState->shadowSceneNode[0];
	if (const auto* shadowLight = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr) {
		const auto& shadowData = shadowLight->GetShadowDirectionalLightRuntimeData();
		diagnostics["nativeStartSplitDistances"] = { shadowData.startSplitDistances[0], shadowData.startSplitDistances[1] };
		diagnostics["nativeEndSplitDistances"] = { shadowData.endSplitDistances[0], shadowData.endSplitDistances[1] };
	}
	const auto sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
	const auto light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
	if (light) {
		const auto direction = light->GetWorldDirection();
		diagnostics["mapDirectionErrors"] = { (maps[0].direction - direction).Length(), (maps[1].direction - direction).Length(), (maps[2].direction - direction).Length() };
	}
}

void LODShadows::ReflectionsPrepass()
{
	maskReady = false;
	if (!mapBuffer)
		return;
	Publish();
	ID3D11ShaderResourceView* views[]{ depthSRV.get(), mapBuffer->srv.get() };
	globals::d3d::context->PSSetShaderResources(kDepthSlot, 2, views);
}

void LODShadows::EarlyPrepass()
{
	const double nowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
	frameDurationMs = frameTimeMs > 0 ? nowMs - frameTimeMs : 0;
	frameTimeMs = nowMs;
	ProcessTraceRequest();
	maskReady = false;
	lastDraws = lastTriangles = 0;
	ResolveTimings();
	if (resetRequested.exchange(false)) {
		Invalidate();
		lastUpdateMs = maxUpdateMs = 0;
		lastRefreshMs = maxRefreshMs = 0;
		timingSamples = completedMaps = 0;
	}
	if (!ready || !settings.Enabled || !globals::state->HasDirectionalShadows()) {
		if (wasEnabled)
			Invalidate();
		wasEnabled = false;
		Publish();
		return;
	}
	wasEnabled = true;
	try {
		auto manager = RE::BGSTerrainManager::GetActiveManager();
		auto scene = globals::game::smState->shadowSceneNode[0];
		auto sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
		auto light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
		if (!manager || !manager->worldSpace || manager->mapMode || RE::BGSTerrainManager::LoadScreenUp() || !light) {
			Invalidate();
			Publish();
			return;
		}
		auto direction = light->GetWorldDirection();
		const auto directionLength = direction.Length();
		if (!std::isfinite(directionLength) || directionLength < kMinimumDirectionLength) {
			Invalidate();
			Publish();
			return;
		}
		direction /= directionLength;
		const auto generation = Util::GetCompletedCelestialTransitionGeneration();
		const float clockRate = ShadowClockRate();
		const bool subframeUpdates = clockRate > 1.0f && frameDurationMs * clockRate >= kHistoryIntervalsMs[0];
		const bool refreshImmediately = refreshEveryFrame || subframeUpdates || generation != transitionGeneration;
		const auto worldID = manager->worldSpace->GetFormID();
		if (worldID != worldSpaceID)
			Invalidate();
		worldSpaceID = worldID;
		transitionGeneration = generation;
		const auto collectionStart = traceEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		hiddenNodes = enabledSegments = disabledSegments = 0;
		sceneHash = kHashSeed;
		visibleGeometry.clear();
		{
			RE::BSSpinLockGuard lock(RE::BGSTerrainManager::GetCameraTerrainLock());
			CollectTerrain();
			sourceHashes[0] = sceneHash;
			geometryCounts[0] = uint32_t(visibleGeometry.size());
			sceneHash = kHashSeed;
			Collect(RE::BGSTerrainManager::GetLODObjectRoot().get(), true);
			sourceHashes[1] = sceneHash;
			geometryCounts[1] = uint32_t(visibleGeometry.size()) - geometryCounts[0];
			sceneHash = kHashSeed;
			Collect(RE::BGSTerrainManager::GetTreeNode().get(), true);
			sourceHashes[2] = sceneHash;
			geometryCounts[2] = uint32_t(visibleGeometry.size()) - geometryCounts[0] - geometryCounts[1];
		}
		sceneHash = kHashSeed;
		const auto lodGeometryCount = visibleGeometry.size();
		CollectLoadedObjects();
		sourceHashes[3] = sceneHash;
		geometryCounts[3] = uint32_t(visibleGeometry.size() - lodGeometryCount);
		sceneHash = kHashSeed;
		HashBytes(sceneHash, sourceHashes.data(), sizeof(sourceHashes));
		const double collectionMs = traceEnabled ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - collectionStart).count() : 0;
		UpdateMaps(Util::GetCameraPosition(), direction, refreshImmediately);
		RecordTrace(direction, refreshImmediately, collectionMs);
	} catch (const std::exception& error) {
		logger::error("LOD Shadows disabled after rendering failure: {}", error.what());
		ready = false;
		Invalidate();
	}
	Publish();
}

void LODShadows::ProcessTraceRequest()
{
	const auto request = traceRequest.exchange(TraceRequest::None);
	if (request == TraceRequest::Start && !traceEnabled && !traceWriting) {
		traceFrames.clear();
		traceFrames.reserve(kTraceFrames);
		traceCasterSnapshots = {};
		{
			std::scoped_lock lock(diagnosticsMutex);
			tracePath.clear();
			traceError.clear();
		}
		traceEnabled = true;
	} else if (request == TraceRequest::Stop && traceEnabled) {
		SaveTrace();
	}
}

void LODShadows::RecordTrace(const RE::NiPoint3& a_direction, bool a_refreshImmediately, double a_collectionMs)
{
	if (!traceEnabled)
		return;
	TraceFrame record{};
	record.frame = globals::game::graphicsState->GetFrameCount();
	record.timeMs = frameTimeMs;
	record.world = worldSpaceID;
	record.hour = globals::game::calendar ? globals::game::calendar->GetHour() : 0;
	record.camera = Util::GetCameraPosition();
	if (const auto* camera = globals::game::playerCamera) {
		if (camera->cameraRoot)
			record.cameraRoot = camera->cameraRoot->world.translate;
		record.cameraAnchor = camera->GetRuntimeData2().pos;
		record.cameraAnchorValid = camera->GetRuntimeData2().isProcessed;
	}
	record.light = a_direction;
	const auto* scene = globals::game::smState->shadowSceneNode[0];
	const auto* sun = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
	if (sun) {
		record.cascadeLight = sun->GetShadowDirectionalLightRuntimeData().sunVector;
		const auto copyProjections = [&](const auto& lightData) {
			for (uint32_t i = 0; i < std::min(lightData.shadowmapDescriptors.size(), uint32_t(record.cascadeProjections.size())); ++i)
				DirectX::XMStoreFloat4x4(&record.cascadeProjections[i], DirectX::XMLoadFloat4x4(Util::AsReal(&lightData.shadowmapDescriptors[i].lightTransform)));
		};
		if (globals::game::isVR)
			copyProjections(sun->GetVRRuntimeData());
		else
			copyProjections(sun->GetRuntimeData());
	}
	record.sourceHashes = sourceHashes;
	record.terrainHashes = terrainHashes;
	if (traceFrames.size() < kTerrainIdentityTraceFrames) {
		RE::BSSpinLockGuard lock(RE::BGSTerrainManager::GetCameraTerrainLock());
		record.terrainGeometry = json::array();
		for (uint32_t i = 0; i < terrainGeometryCount; ++i) {
			const auto& geometry = visibleGeometry[i];
			const auto& data = geometry->GetGeometryRuntimeData();
			const auto* mesh = static_cast<RE::BSGraphics::TriShape*>(data.rendererData);
			const auto* shape = geometry->AsTriShape();
			record.terrainGeometry.push_back({ { "geometry", reinterpret_cast<uintptr_t>(geometry.get()) }, { "name", geometry->name.c_str() },
				{ "renderer", reinterpret_cast<uintptr_t>(mesh) },
				{ "vertices", mesh ? reinterpret_cast<uintptr_t>(mesh->vertexBuffer) : 0 },
				{ "indices", mesh ? reinterpret_cast<uintptr_t>(mesh->indexBuffer) : 0 },
				{ "triangles", shape ? shape->GetTrishapeRuntimeData().triangleCount : 0 },
				{ "lodMode", geometry->GetLODMode() },
				{ "position", { geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z } },
				{ "loadedTerrain", i >= terrainGeometryCount - loadedTerrainGeometryCount } });
		}
	}
	record.geometryCounts = geometryCounts;
	record.loadedTerrainGeometryCount = loadedTerrainGeometryCount;
	record.landBounds = landBounds;
	record.hiddenNodes = hiddenNodes;
	record.enabledSegments = enabledSegments;
	record.disabledSegments = disabledSegments;
	record.maps = maps;
	for (uint32_t i = 0; i < kMapCount; ++i) {
		record.historyFrames[i] = previousMaps[i].valid ? previousMaps[i].frame : 0;
		record.historyWeights[i] = GetHistoryBlend(i);
	}
	record.pending = pendingMap;
	record.pendingIndex = pendingIndex;
	record.remainingCasters = pendingIndex >= 0 ? casters.size() - casterIndex : 0;
	record.completedMaps = completedMaps;
	record.draws = lastDraws;
	record.triangles = lastTriangles;
	record.triangleBudget = triangleBudget;
	record.unsupportedCasters = unsupportedCasters;
	record.immediateRefresh = a_refreshImmediately;
	record.collectionMs = a_collectionMs;
	record.gpuMs = { lastUpdateMs, lastFilterMs, lastRefreshMs };
	record.gpuFrames = { lastUpdateFrame, lastFilterFrame, lastRefreshFrame };
	traceFrames.push_back(record);
	if (traceFrames.size() == kTraceFrames)
		SaveTrace();
}

void LODShadows::SaveTrace()
{
	traceEnabled = false;
	if (traceFrames.empty())
		return;
	const auto path = Util::PathHelpers::GetCommunityShaderPath() / "Captures" /
	                  std::format("LODShadows_{}_{}.json", GetCurrentProcessId(), traceFrames.front().frame);
	const auto runtime = REL::Module::get().version().string();
	{
		std::scoped_lock lock(diagnosticsMutex);
		tracePath = path.string();
	}
	traceWriting = true;
	traceWriter = std::jthread([this, frames = std::move(traceFrames), casterSnapshots = std::move(traceCasterSnapshots), path, runtime]() {
		const SKSE::stl::scope_exit finish([&]() noexcept { traceWriting = false; });
		try {
			const auto point = [](const RE::NiPoint3& value) { return json::array({ value.x, value.y, value.z }); };
			const auto matrix = [](const DirectX::XMFLOAT4X4& value) {
				json result = json::array();
				for (const auto& row : value.m)
					for (float element : row)
						result.push_back(element);
				return result;
			};
			const auto mapData = [&](const Map& map) {
				return json{ { "buildFrame", map.frame }, { "scene", map.scene }, { "slice", map.slice },
					{ "center", point(map.center) }, { "direction", point(map.direction) },
					{ "matrix", matrix(map.data.worldToClip) }, { "terrainValid", map.terrainValid },
					{ "buildDurationMs", map.buildDurationMs }, { "triangleCount", map.triangleCount },
					{ "transitionGeneration", map.transitionGeneration }, { "sourceTriangles", map.sourceTriangles }, { "sourceCasters", map.sourceCasters } };
			};
			json result{ { "runtime", runtime }, { "resolution", kResolution }, { "halfExtents", kHalfExtents },
				{ "depthExtent", kDepthExtent }, { "directionThreshold", kDirectionThreshold },
				{ "maximumCacheFrames", kMaximumCacheFrames },
				{ "sources", { "terrain", "lodObjects", "lodTrees", "loadedObjects" } },
				{ "gpuTimers", { "cacheUpdate", "receiverMaskAndCrossBlur", "timeJumpRefresh" } },
				{ "gpuScope", "GPU times identify their submission frame and exclude material/volume sampling; they are not total feature cost." },
				{ "terrainHashFields", { "geometry", "transform", "lodMode", "rendererData", "segments", "landBounds", "geometrySet", "rendererSet" } },
				{ "casterSnapshots", casterSnapshots },
				{ "frames", json::array() }, { "mapVersions", json::array() } };
			std::array<uint32_t, kMapCount> publishedFrames{};
			for (const auto& record : frames) {
				json versions = json::array();
				for (uint32_t i = 0; i < kMapCount; ++i) {
					const auto& map = record.maps[i];
					versions.push_back(map.valid ? json(map.frame) : json(nullptr));
					if (map.valid && publishedFrames[i] != map.frame) {
						auto version = mapData(map);
						version["index"] = i;
						version["firstObservedFrame"] = record.frame;
						result["mapVersions"].push_back(std::move(version));
						publishedFrames[i] = map.frame;
					}
				}
				result["frames"].push_back({ { "frame", record.frame }, { "timeMs", record.timeMs }, { "world", record.world }, { "hour", record.hour },
					{ "camera", point(record.camera) }, { "light", point(record.light) }, { "cascadeLight", point(record.cascadeLight) },
					{ "cameraRoot", point(record.cameraRoot) }, { "cameraAnchor", point(record.cameraAnchor) }, { "cameraAnchorValid", record.cameraAnchorValid },
					{ "cascadeProjections", { matrix(record.cascadeProjections[0]), matrix(record.cascadeProjections[1]) } },
					{ "sourceHashes", record.sourceHashes }, { "terrainHashes", record.terrainHashes }, { "geometryCounts", record.geometryCounts },
					{ "terrainGeometry", record.terrainGeometry },
					{ "loadedTerrainGeometryCount", record.loadedTerrainGeometryCount },
					{ "landBounds", { record.landBounds.x, record.landBounds.y, record.landBounds.z, record.landBounds.w } },
					{ "hiddenNodes", record.hiddenNodes }, { "enabledSegments", record.enabledSegments }, { "disabledSegments", record.disabledSegments },
					{ "maps", std::move(versions) }, { "historyFrames", record.historyFrames }, { "historyWeights", record.historyWeights }, { "pendingIndex", record.pendingIndex },
					{ "pendingBuildFrame", record.pendingIndex >= 0 ? json(record.pending.frame) : json(nullptr) },
					{ "remainingCasters", record.remainingCasters }, { "completedMaps", record.completedMaps },
					{ "draws", record.draws }, { "triangles", record.triangles }, { "triangleBudget", record.triangleBudget },
					{ "unsupportedCasters", record.unsupportedCasters }, { "immediateRefresh", record.immediateRefresh },
					{ "collectionCpuMs", record.collectionMs }, { "gpuMs", record.gpuMs }, { "gpuSubmissionFrames", record.gpuFrames } });
			}
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			if (error)
				throw std::runtime_error(error.message());
			if (!Util::FileHelpers::WriteJsonAtomically(path, result, -1, "LOD Shadows trace"))
				throw std::runtime_error("Failed to write LOD shadow trace");
			logger::info("LOD Shadows trace saved: {}", path.string());
		} catch (const std::exception& error) {
			logger::error("LOD Shadows trace failed: {}", error.what());
			std::scoped_lock lock(diagnosticsMutex);
			traceError = error.what();
		}
	});
}

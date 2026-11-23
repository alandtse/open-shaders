#include "InteriorSunOpenings.h"

#include "GpuPass.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

namespace InteriorSunOpenings
{
	namespace
	{
		constexpr wchar_t kShader[] = L"Data\\Shaders\\InteriorSun\\PhysicalOpenings.hlsl";
		constexpr UINT kGroupSize = 8;
		constexpr UINT kReduction = 4;
		constexpr UINT kMaximumGrid = 1024;
		constexpr float kDirectionToleranceSquared = 1.0e-10f;

		struct alignas(16) Constants
		{
			UINT slice;
			float drop;
			UINT width, height;
			UINT rimCoverage, gridWidth, gridHeight, padding{};
			std::array<float4, 2> gridFromShadow;
			float2 shadowDepthFromGrid;
			float paddingDepth[2]{};
		};

		struct alignas(16) ShapeConstants
		{
			DirectX::XMFLOAT4X4 worldToClip;
			float4 uvTransform;
			float alphaThreshold{};
			float padding[3]{};
		};

		std::unique_ptr<Texture2D> MakeTexture(UINT width, UINT height, const char* name, bool renderTarget = false)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			if (renderTarget)
				desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
			auto texture = std::make_unique<Texture2D>(desc, name);
			if (renderTarget)
				texture->CreateRTV({ desc.Format, D3D11_RTV_DIMENSION_TEXTURE2D });
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{ desc.Format, D3D11_SRV_DIMENSION_TEXTURE2D };
			srv.Texture2D.MipLevels = 1;
			texture->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{ desc.Format, D3D11_UAV_DIMENSION_TEXTURE2D };
			texture->CreateUAV(uav);
			return texture;
		}

		void Dispatch(ID3D11ComputeShader* shader, UINT width, UINT height, ID3D11UnorderedAccessView* uav,
			ID3D11ShaderResourceView* shadow, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* rim, ID3D11ShaderResourceView* auxiliary)
		{
			auto ctx = globals::d3d::context;
			ID3D11ShaderResourceView* inputs[]{ shadow, depth, rim, auxiliary };
			ctx->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
			ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			ctx->CSSetShader(shader, nullptr, 0);
			ctx->Dispatch((width + kGroupSize - 1) / kGroupSize, (height + kGroupSize - 1) / kGroupSize, 1);
			uav = nullptr;
			ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		}

		void Dispatch(ID3D11ComputeShader* shader, Texture2D& output, ID3D11ShaderResourceView* shadow,
			ID3D11ShaderResourceView* depth = nullptr, ID3D11ShaderResourceView* rim = nullptr, ID3D11ShaderResourceView* auxiliary = nullptr)
		{
			Dispatch(shader, output.desc.Width, output.desc.Height, output.uav.get(), shadow, depth, rim, auxiliary);
		}
	}

	bool Detector::Initialize()
	{
		auto compute = [](auto& shader, const char* name, bool regions = false) {
			std::vector<std::pair<const char*, const char*>> defines;
			if (regions)
				defines.push_back({ "OPENING_REGIONS", "1" });
			shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(kShader, defines, "cs_5_0", name)));
			if (shader)
				Util::SetResourceName(shader.get(), "InteriorSun::%s CS", name);
			return bool(shader);
		};
		auto pixel = [](auto& shader, const char* name) {
			shader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(kShader, { { "OPENING_COMPOSITE", "1" } }, "ps_5_0", name)));
			if (shader)
				Util::SetResourceName(shader.get(), "InteriorSun::%s PS", name);
			return bool(shader);
		};
		if (!compute(reduceDepth, "ReduceDepth") || !compute(findRims, "FindRims") ||
			!compute(seedRegions, "SeedRegions", true) || !compute(joinRegions, "JoinRegions", true) || !compute(measureRims, "MeasureRims", true) || !compute(classifyOpenings, "ClassifyOpenings") ||
			!compute(findCenters, "FindWideCenters") || !compute(makeEntries, "OpeningEntries") || !pixel(merge, "MergeEntries") || !pixel(closeDepth, "CloseShadowDepth"))
			return false;

		auto device = globals::d3d::device;
		constexpr wchar_t shellShader[] = L"Data\\Shaders\\InteriorSun\\OpeningShell.hlsl";
		shellBytecode = Util::CompileShaderBlob(shellShader, {}, "vs_5_0", "ShellVS");
		depthPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(shellShader, {}, "ps_5_0", "ShellDepth")));
		cutoutPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(shellShader, {}, "ps_5_0", "ShellCutoutDepth")));
		if (!shellBytecode || !depthPS || !cutoutPS)
			return false;
		DX::ThrowIfFailed(device->CreateVertexShader(shellBytecode->GetBufferPointer(), shellBytecode->GetBufferSize(), nullptr, shellVS.put()));
		Util::SetResourceName(shellVS.get(), "InteriorSun::OpeningShell VS");
		Util::SetResourceName(depthPS.get(), "InteriorSun::OpeningShell PS");
		Util::SetResourceName(cutoutPS.get(), "InteriorSun::OpeningShellCutout PS");
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.DepthClipEnable = true;
		raster.CullMode = D3D11_CULL_NONE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&raster, twoSidedRaster.put()));
		Util::SetResourceName(twoSidedRaster.get(), "InteriorSun::OpeningShell TwoSided");
		D3D11_BLEND_DESC blend{};
		auto& target = blend.RenderTarget[0];
		target.BlendEnable = true;
		target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_MIN;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		DX::ThrowIfFailed(device->CreateBlendState(&blend, nearestDepth.put()));
		Util::SetResourceName(nearestDepth.get(), "InteriorSun::OpeningShell NearestDepth");
		shapeConstants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShapeConstants>(), "InteriorSun::OpeningShell Constants");
		constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<Constants>(), "InteriorSun::OpeningConstants");
		return true;
	}

	void Detector::SetShell(std::vector<ShellShape> shapes)
	{
		std::ranges::sort(shapes, {}, [](const ShellShape& s) { return s.geometry.get(); });
		if (shapes != shell) {
			shell = std::move(shapes);
			fitDirty = true;
		}
	}

	void Detector::FitGrid(float3 ray, float minimumWidth)
	{
		direction = ray;
		u = (std::abs(ray.z) < 0.99f ? float3::UnitZ : float3::UnitY).Cross(ray);
		u.Normalize();
		v = ray.Cross(u);
		float3 lower{ FLT_MAX }, upper{ -FLT_MAX };
		for (const auto& shape : shell) {
			const auto& bound = shape.geometry->worldBound;
			const float3 center{ bound.center.x, bound.center.y, bound.center.z };
			const float3 projected{ u.Dot(center), v.Dot(center), ray.Dot(center) };
			lower = float3::Min(lower, projected - float3(bound.radius));
			upper = float3::Max(upper, projected + float3(bound.radius));
		}
		const auto span = upper - lower;
		if (!std::isfinite(span.x + span.y + span.z))
			throw std::runtime_error("non-finite interior shell bounds");
		pitch = minimumWidth * 0.5f;
		while (std::max(span.x, span.y) > pitch * (kMaximumGrid - 2 * kGroupSize))
			pitch *= 2;
		gridOrigin = float3{ (std::floor(lower.x / pitch) - 1) * pitch, (std::floor(lower.y / pitch) - 1) * pitch, lower.z - pitch };
		depthRange = upper.z + pitch - gridOrigin.z;
		auto size = [&](float extent) { return UINT(std::ceil((extent / pitch + 1) / kGroupSize)) * kGroupSize; };
		UINT w = size(upper.x - gridOrigin.x), h = size(upper.y - gridOrigin.y);
		const float3 x = u * (2.0f / (w * pitch)), y = v * (-2.0f / (h * pitch)), z = ray / depthRange;
		worldToClip = float4x4{ x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0,
			-1 - 2 * gridOrigin.x / (w * pitch), 1 + 2 * gridOrigin.y / (h * pitch), -gridOrigin.z / depthRange, 1 };
		if (!grid.entries || grid.entries->desc.Width != w || grid.entries->desc.Height != h) {
			Grid next;
			const UINT cells = w * h;
			next.regions = std::make_unique<Buffer>(StructuredBufferDesc<std::array<UINT, 3>>(cells, true, false), nullptr, "InteriorSun::OpeningRegions");
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{ DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER };
			uav.Buffer.NumElements = cells;
			next.regions->CreateUAV(uav);
			D3D11_SHADER_RESOURCE_VIEW_DESC regionView{ DXGI_FORMAT_UNKNOWN, D3D11_SRV_DIMENSION_BUFFER };
			regionView.Buffer.NumElements = cells;
			next.regions->CreateSRV(regionView);
			next.candidates = MakeTexture(w, h, "InteriorSun::OpeningCandidates");
			next.centers = MakeTexture(w, h, "InteriorSun::WideOpeningCenters");
			next.entries = MakeTexture(w, h, "InteriorSun::PhysicalOpeningEntries");
			for (;;) {
				next.levels.push_back({ MakeTexture(w, h, "InteriorSun::OpeningDepth", next.levels.empty()), MakeTexture(w, h, "InteriorSun::OpeningRim") });
				if (w <= kGroupSize && h <= kGroupSize)
					break;
				w = (w + kReduction - 1) / kReduction;
				h = (h + kReduction - 1) / kReduction;
			}
			grid = std::move(next);
		}
		width = minimumWidth;
		fitDirty = false;
		dirty = true;
	}

	bool Detector::Prepare(Projection& projection, UINT target, UINT slice, const RE::BSGraphics::ViewData& eye, const RE::NiPoint3& adjust, float minimumWidth, float depthDrop, int rimCoverage, bool doubleSided)
	{
		if (failed || shell.empty())
			return false;
		auto& stencil = globals::game::renderer->GetDepthStencilData().depthStencils[target];
		if (!stencil.depthSRV)
			return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv;
		Util::AsReal(stencil.depthSRV)->GetDesc(&srv);
		if (srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY || srv.Texture2DArray.FirstArraySlice != 0 || slice >= srv.Texture2DArray.ArraySize)
			return false;
		const float4x4 viewProj = eye.viewProjMat;
		const auto inverse = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewProj));
		const float3 dz = inverse.r[2];
		const float nativeRange = dz.Length();
		if (!(nativeRange > 0) || !std::isfinite(nativeRange) || eye.viewPort[1] <= eye.viewPort[0] || eye.viewPort[2] <= eye.viewPort[3])
			return false;
		const float3 ray = dz / nativeRange;
		minimumWidth = std::clamp(minimumWidth, 4.0f, 128.0f);
		try {
			if (!constants && !Initialize()) {
				failed = true;
				return false;
			}
			if (fitDirty || width != minimumWidth || (direction - ray).LengthSquared() > kDirectionToleranceSquared)
				FitGrid(ray, minimumWidth);
		} catch (const std::exception& e) {
			logger::error("[Interior Sun] Could not prepare physical opening detection: {}", e.what());
			failed = true;
			return false;
		}
		float newDrop = std::clamp(depthDrop, 8.0f, 512.0f) / depthRange;
		UINT newCoverage = std::clamp(rimCoverage, 1, 100);
		dirty |= drop != newDrop || coverage != newCoverage || forceDoubleSided != doubleSided;
		drop = newDrop;
		coverage = newCoverage;
		forceDoubleSided = doubleSided;
		const float3 dx = float3(inverse.r[0]) * (2 / (eye.viewPort[1] - eye.viewPort[0]));
		const float3 dy = float3(inverse.r[1]) * (-2 / (eye.viewPort[2] - eye.viewPort[3]));
		const float3 origin = float3(inverse.r[3]) + float3{ adjust.x, adjust.y, adjust.z } -
		                      dx * ((eye.viewPort[0] + eye.viewPort[1]) * 0.5f) - dy * (1 - (eye.viewPort[2] + eye.viewPort[3]) * 0.5f);
		projection.target = target;
		projection.slice = slice;
		projection.gridFromShadow = { float4{ dx.Dot(u), dy.Dot(u), origin.Dot(u) - gridOrigin.x, 0 } / pitch,
			float4{ dx.Dot(v), dy.Dot(v), origin.Dot(v) - gridOrigin.y, 0 } / pitch };
		projection.shadowDepthFromGrid = float2{ depthRange, gridOrigin.z - origin.Dot(direction) } / nativeRange;
		return true;
	}

	void Detector::RasterizeShell(ID3D11RasterizerState* backCull, const std::array<winrt::com_ptr<ID3D11SamplerState>, 4>& materialSamplers)
	{
		CS_GPU_PASS("InteriorSun::OpeningShell");
		auto ctx = globals::d3d::context;
		auto& depth = *grid.levels.front().depth;
		auto rtv = depth.rtv.get();
		const float clear[4]{ 1, 1, 1, 1 };
		ctx->ClearRenderTargetView(rtv, clear);
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->OMSetBlendState(nearestDepth.get(), nullptr, UINT_MAX);
		D3D11_VIEWPORT vp{ 0, 0, float(depth.desc.Width), float(depth.desc.Height), 0, 1 };
		ctx->RSSetViewports(1, &vp);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(shellVS.get(), nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		auto cb = shapeConstants->CB();
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		for (const auto& shape : shell) {
			auto geometry = shape.geometry.get();
			auto& data = geometry->GetGeometryRuntimeData();
			auto mesh = data.rendererData;
			const UINT count = geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount * 3u;
			if (!mesh || !mesh->vertexBuffer || !mesh->indexBuffer || !count)
				continue;
			using Vertex = RE::BSGraphics::Vertex;
			const UINT stride = UINT(std::bit_cast<uint64_t>(data.vertexDesc) & 0xfu) * sizeof(float);
			const UINT uv = data.vertexDesc.HasFlag(Vertex::VF_UV) ? data.vertexDesc.GetAttributeOffset(Vertex::VA_TEXCOORD0) : 0;
			const bool full = uv ? uv >= 16 : data.vertexDesc.HasFlag(Vertex::VF_FULLPREC) || stride >= 16;
			if (stride < (full ? 12u : 8u) || uv + sizeof(uint32_t) > stride)
				continue;
			auto [it, inserted] = layouts.try_emplace(uv * 2 + full);
			if (inserted) {
				D3D11_INPUT_ELEMENT_DESC layout[]{
					{ "POSITION", 0, full ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uv, D3D11_INPUT_PER_VERTEX_DATA, 0 }
				};
				DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(layout, ARRAYSIZE(layout), shellBytecode->GetBufferPointer(), shellBytecode->GetBufferSize(), it->second.put()));
				Util::SetResourceName(it->second.get(), "InteriorSun::OpeningShell Layout");
			}
			auto prop = data.shaderProperty.get();
			if (!prop || !prop->material)
				continue;
			auto alpha = data.alphaProperty.get();
			auto texture = alpha && alpha->GetAlphaTesting() && uv ? prop->GetBaseTexture() : nullptr;
			auto mask = texture && texture->rendererTexture ? Util::AsReal(texture->rendererTexture->resourceView) : nullptr;
			ShapeConstants values{};
			auto& transform = shape.transform;
			alignas(16) DirectX::XMFLOAT3X4 world;
			Util::StoreTransform3x4NoScale(world, transform);
			DirectX::XMStoreFloat4x4(&values.worldToClip, DirectX::XMMatrixScaling(transform.scale, transform.scale, transform.scale) *
															  DirectX::XMLoadFloat3x4(&world) * DirectX::XMLoadFloat4x4(&worldToClip));
			if (mask) {
				auto material = static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
				auto sampler = materialSamplers[UINT(material->textureClampMode) & 3u].get();
				ctx->PSSetSamplers(0, 1, &sampler);
				values.uvTransform = float4{ material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
				values.alphaThreshold = alpha->alphaThreshold / 255.0f;
			}
			shapeConstants->Update(values);
			auto vertices = Util::AsReal(mesh->vertexBuffer);
			const UINT offset = 0;
			const bool twoSided = forceDoubleSided || prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			ctx->RSSetState(twoSided ? twoSidedRaster.get() : backCull);
			ctx->IASetInputLayout(it->second.get());
			ctx->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
			ctx->IASetIndexBuffer(Util::AsReal(mesh->indexBuffer), DXGI_FORMAT_R16_UINT, 0);
			ctx->PSSetShader(mask ? cutoutPS.get() : depthPS.get(), nullptr, 0);
			ctx->PSSetShaderResources(0, 1, &mask);
			ctx->DrawIndexed(count, 0, 0);
		}
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
	}

	void Detector::Draw(const Projection& projection, ID3D11ShaderResourceView* panes, ID3D11RenderTargetView* mergedTarget, ID3D11ShaderResourceView* mergedEntries,
		ID3D11VertexShader* fullscreen, ID3D11RasterizerState* raster, ID3D11DepthStencilState* writeDepth,
		const std::array<winrt::com_ptr<ID3D11SamplerState>, 4>& materialSamplers)
	{
		CS_GPU_PASS("InteriorSun::DetectPhysicalOpenings");
		auto ctx = globals::d3d::context;
		auto& stencil = globals::game::renderer->GetDepthStencilData().depthStencils[projection.target];
		auto shadow = Util::AsReal(stencil.depthSRV);
		D3D11_TEXTURE2D_DESC desc;
		Util::AsReal(stencil.texture)->GetDesc(&desc);
		const auto& gridSize = grid.entries->desc;
		constants->Update(Constants{ projection.slice, drop, desc.Width, desc.Height, coverage, gridSize.Width, gridSize.Height, 0, projection.gridFromShadow, projection.shadowDepthFromGrid });
		auto cb = constants->CB();
		Util::FullscreenPassScope graphics(ctx);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);

		winrt::com_ptr<ID3D11ComputeShader> savedShader;
		winrt::com_ptr<ID3D11UnorderedAccessView> savedUAV;
		winrt::com_ptr<ID3D11Buffer> savedCB;
		ID3D11ShaderResourceView* savedInputs[4]{};
		winrt::com_ptr<ID3D11DeviceContext1> ctx1;
		ctx->QueryInterface(IID_PPV_ARGS(ctx1.put()));
		UINT first = 0, count = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
		if (ctx1)
			ctx1->CSGetConstantBuffers1(0, 1, savedCB.put(), &first, &count);
		else
			ctx->CSGetConstantBuffers(0, 1, savedCB.put());
		ctx->CSGetShader(savedShader.put(), nullptr, nullptr);
		ctx->CSGetUnorderedAccessViews(0, 1, savedUAV.put());
		ctx->CSGetShaderResources(0, ARRAYSIZE(savedInputs), savedInputs);
		ctx->CSSetConstantBuffers(0, 1, &cb);

		if (dirty) {
			auto& base = grid.levels.front();
			RasterizeShell(raster, materialSamplers);
			for (size_t i = 1; i < grid.levels.size(); ++i)
				Dispatch(reduceDepth.get(), *grid.levels[i].depth, nullptr, grid.levels[i - 1].depth->srv.get());
			ID3D11ShaderResourceView* parent = nullptr;
			for (auto& level : grid.levels | std::views::reverse) {
				Dispatch(findRims.get(), *level.rim, nullptr, level.depth->srv.get(), parent);
				parent = level.rim->srv.get();
			}
			for (auto stage : { seedRegions.get(), joinRegions.get(), measureRims.get() })
				Dispatch(stage, gridSize.Width, gridSize.Height, grid.regions->uav.get(), nullptr, base.depth->srv.get(), parent, nullptr);
			Dispatch(classifyOpenings.get(), *grid.candidates, nullptr, base.depth->srv.get(), parent, grid.regions->srv.get());
			Dispatch(findCenters.get(), *grid.centers, nullptr, grid.candidates->srv.get());
			Dispatch(makeEntries.get(), *grid.entries, nullptr, grid.candidates->srv.get(), nullptr, grid.centers->srv.get());
			dirty = false;
		}

		D3D11_VIEWPORT viewport{ 0, 0, float(desc.Width), float(desc.Height), 0, 1 };
		ctx->RSSetViewports(1, &viewport);
		ctx->RSSetState(raster);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->VSSetShader(fullscreen, nullptr, 0);
		ctx->OMSetBlendState(nullptr, nullptr, UINT_MAX);
		ctx->OMSetRenderTargets(1, &mergedTarget, nullptr);
		ctx->PSSetShader(merge.get(), nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ID3D11ShaderResourceView* inputs[]{ shadow, panes, grid.entries->srv.get() };
		ctx->PSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		ctx->Draw(3, 0);

		ID3D11ShaderResourceView* none[4]{};
		ctx->PSSetShaderResources(0, ARRAYSIZE(inputs), none);
		ctx->CSSetShaderResources(0, ARRAYSIZE(none), none);
		ctx->OMSetRenderTargets(0, nullptr, Util::AsReal(stencil.views[projection.slice]));
		ctx->OMSetDepthStencilState(writeDepth, 0);
		ctx->PSSetShader(closeDepth.get(), nullptr, 0);
		ctx->PSSetShaderResources(1, 1, &mergedEntries);
		ctx->Draw(3, 0);
		ctx->OMSetRenderTargets(0, nullptr, nullptr);
		auto oldCB = savedCB.get();
		if (ctx1)
			ctx1->CSSetConstantBuffers1(0, 1, &oldCB, &first, &count);
		else
			ctx->CSSetConstantBuffers(0, 1, &oldCB);
		ctx->CSSetShader(savedShader.get(), nullptr, 0);
		auto oldUAV = savedUAV.get();
		ctx->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
		ctx->CSSetShaderResources(0, ARRAYSIZE(savedInputs), savedInputs);
		for (auto input : savedInputs)
			if (input)
				input->Release();
	}
}

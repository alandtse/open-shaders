#include "InteriorSun.h"
#include "InteriorSunOpenings.h"

#include "Buffer.h"
#include "GpuPass.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DevBenchUx.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"

namespace
{
	constexpr UINT kReceiverSlot = 66;
	constexpr UINT kCasterSlot = 69;
	constexpr size_t kCascadeCount = kCasterSlot - kReceiverSlot;
	constexpr wchar_t kShader[] = L"Data\\Shaders\\InteriorSun\\WindowShadow.hlsl";
	constexpr auto kRescanInterval = std::chrono::seconds(2);

	struct Pane
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiSourceTexturePtr mask;
		std::string sourcePath;
	};

	struct LoadedReference
	{
		RE::NiPointer<RE::NiAVObject> root;
		std::vector<Pane> panes;
		std::vector<RE::NiPointer<RE::BSGeometry>> shell;
		std::string meshPath;
		bool seen{};
	};

	struct WindowTextureRule
	{
		RE::NiSourceTexturePtr mask;
		std::optional<std::vector<std::string>> whitelist;
		std::vector<std::string> blacklist;

		bool Matches(const std::string& mesh) const
		{
			return (!whitelist || std::ranges::find(*whitelist, mesh) != whitelist->end()) &&
			       std::ranges::find(blacklist, mesh) == blacklist.end();
		}
	};

	struct alignas(16) PaneConstants
	{
		float alphaReference;
		float padding[3]{};
		DirectX::XMFLOAT4X4 worldViewProj;
		float4 uvTransform;
	};

	struct Cascade
	{
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		UINT width{}, height{};
		bool written{};
		bool detectOpenings{};
		InteriorSunOpenings::Projection openings;
	};

	struct Pipeline
	{
		winrt::com_ptr<ID3DBlob> bytecode;
		winrt::com_ptr<ID3D11VertexShader> paneVS, fullscreenVS;
		winrt::com_ptr<ID3D11PixelShader> panePS, initializePS, opaquePS;
		winrt::com_ptr<ID3D11RasterizerState> raster;
		winrt::com_ptr<ID3D11BlendState> furthestPane;
		winrt::com_ptr<ID3D11DepthStencilState> writeDepth;
		std::array<winrt::com_ptr<ID3D11SamplerState>, 4> samplers;
		std::unique_ptr<ConstantBuffer> constants;
		std::unordered_map<UINT, winrt::com_ptr<ID3D11InputLayout>> layouts;
		std::array<std::unordered_map<uint32_t, winrt::com_ptr<ID3D11PixelShader>>, 2> utilityCasters;

		bool Initialize();
		ID3D11InputLayout* Layout(UINT uvOffset);
		ID3D11PixelShader* UtilityCaster(const RE::BSShader& shader, uint32_t descriptor, bool opaque);
	};

	struct WindowState
	{
		std::unordered_map<std::string, WindowTextureRule> masks;
		std::vector<Pane> panes;
		std::unordered_map<RE::FormID, LoadedReference> references;
		std::unordered_set<const RE::BSGeometry*> geometries;
		std::array<Cascade, kCascadeCount> cascades;
		Cascade volumetricCasterEntry;
		Cascade mergedEntry;
		InteriorSunOpenings::Detector detector;
		RE::TESObjectCELL* cell{};
		std::chrono::steady_clock::time_point nextScan{};
		Pipeline pipeline;
		bool detectionFailed{};
		bool failed{}, shadowPhase{}, receiverBound{}, casterBound{};
		UINT target{}, slice{};
		Cascade* current{};
		winrt::com_ptr<ID3D11PixelShader> savedPS;
		ID3D11PixelShader* overridePS{};
	};
	WindowState windows;
	thread_local bool renderThread = false;

	std::string AssetPath(const char* path, std::string_view directory)
	{
		if (!path || !*path)
			return {};
		auto normalized = Util::FixFilePath(path);
		if (normalized.starts_with('/'))
			normalized.erase(0, 1);
		if (normalized.starts_with("data/"))
			normalized.erase(0, 5);
		if (!normalized.starts_with(directory))
			normalized.insert(0, directory);
		return normalized;
	}

	bool ReadMeshList(const json& list, std::vector<std::string>& meshes)
	{
		if (!list.is_array())
			return false;
		for (const auto& item : list) {
			if (!item.is_string())
				return false;
			auto mesh = AssetPath(item.get_ref<const std::string&>().c_str(), "meshes/");
			if (!mesh.ends_with(".nif"))
				return false;
			meshes.push_back(std::move(mesh));
		}
		return true;
	}

	RE::NiSourceTexturePtr FindMask(RE::BSShaderMaterial* material, const std::string& mesh, std::string& sourcePath)
	{
		const char* path{};
		RE::NiSourceTexture* diffuse{};
		if (material->GetType() == RE::BSShaderMaterial::Type::kLighting) {
			auto lighting = static_cast<RE::BSLightingShaderMaterialBase*>(material);
			diffuse = lighting->diffuseTexture.get();
			path = lighting->textureSet ? lighting->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
		} else if (material->GetType() == RE::BSShaderMaterial::Type::kEffect) {
			auto effect = static_cast<RE::BSEffectShaderMaterial*>(material);
			diffuse = effect->sourceTexture.get();
			path = effect->sourceTexturePath.c_str();
		}
		for (const char* candidate : { diffuse ? diffuse->name.c_str() : nullptr, path }) {
			if (!candidate || !*candidate)
				continue;
			auto key = AssetPath(candidate, "textures/");
			auto it = windows.masks.find(key);
			constexpr std::string_view pbr = "textures/pbr/";
			if (it == windows.masks.end() && key.starts_with(pbr))
				it = windows.masks.find("textures/" + key.substr(pbr.size()));
			if (it != windows.masks.end()) {
				if (!it->second.Matches(mesh))
					return nullptr;
				sourcePath = std::move(key);
				return it->second.mask ? it->second.mask : RE::NiSourceTexturePtr(diffuse);
			}
		}
		return nullptr;
	}

	void CollectPanes(RE::TESObjectCELL* cell)
	{
		std::vector<Pane> panes;
		std::vector<InteriorSunOpenings::ShellShape> shell;
		std::unordered_set<const RE::BSGeometry*> geometries;
		if (windows.cell != cell)
			windows.references.clear();
		cell->ForEachReference([&](RE::TESObjectREFR* ref) {
			auto root = ref->Get3D();
			if (!root || ref->IsDisabled())
				return RE::BSContainer::ForEachResult::kContinue;
			auto& reference = windows.references[ref->GetFormID()];
			reference.seen = true;
			if (reference.root.get() != root) {
				reference.root.reset(root);
				reference.panes.clear();
				reference.shell.clear();
				auto base = ref->GetBaseObject();
				auto model = base ? base->As<RE::TESModel>() : nullptr;
				reference.meshPath = AssetPath(model ? model->GetModel() : nullptr, "meshes/");
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
					auto& data = geometry->GetGeometryRuntimeData();
					auto property = data.shaderProperty.get();
					if (!data.skinInstance && geometry->AsTriShape() && property && property->material &&
						data.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) {
						std::string path;
						if (auto mask = FindMask(property->material, reference.meshPath, path))
							reference.panes.push_back({ RE::NiPointer<RE::BSGeometry>(geometry), mask, std::move(path) });
					}
					if (base && base->Is(RE::FormType::Static, RE::FormType::StaticCollection) && !data.skinInstance &&
						geometry->AsTriShape() && property && property->material && property->material->GetType() == RE::BSShaderMaterial::Type::kLighting &&
						property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows) && geometry->worldBound.radius > 0 &&
						(!data.alphaProperty || !data.alphaProperty->GetAlphaBlending() || data.alphaProperty->GetAlphaTesting()))
						reference.shell.emplace_back(geometry);
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			for (const auto& geometry : reference.shell)
				if (geometry->GetGeometryRuntimeData().rendererData && !geometry->GetAppCulled())
					shell.push_back({ geometry, geometry->world });
			for (const auto& pane : reference.panes)
				if (geometries.insert(pane.geometry.get()).second)
					panes.push_back(pane);
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::erase_if(windows.references, [](const auto& pair) { return !pair.second.seen; });
		for (auto& [id, reference] : windows.references)
			reference.seen = false;
		if (windows.cell != cell || geometries != windows.geometries)
			logger::info("[Interior Sun] Cell {:08X}: {} mapped window shapes", cell->GetFormID(), panes.size());
		windows.detector.SetShell(std::move(shell));
		windows.cell = cell;
		windows.panes = std::move(panes);
		windows.geometries = std::move(geometries);
		windows.nextScan = std::chrono::steady_clock::now() + kRescanInterval;
	}

	bool Pipeline::Initialize()
	{
		auto device = globals::d3d::device;
		bytecode = Util::CompileShaderBlob(kShader, {}, "vs_5_0", "PaneVS");
		if (!bytecode)
			return false;
		DX::ThrowIfFailed(device->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, paneVS.put()));
		Util::SetResourceName(paneVS.get(), "InteriorSun::Pane VS");
		fullscreenVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(kShader, {}, "vs_5_0", "FullscreenVS")));
		const std::vector<std::pair<const char*, const char*>> paneDefines{
			{ "INTERIOR_WINDOW_PANE", "1" }, { "RENDER_SHADOWMAP", "1" }, { "TEXTURE", "1" }, { "ALPHA_TEST", "1" },
			{ "SHADOWFILTER", "0" }, { "SHADOWSPLITCOUNT", "3" }
		};
		panePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", paneDefines, "ps_5_0")));
		initializePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(kShader, {}, "ps_5_0", "InitializeDepthPS")));
		opaquePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(kShader, {}, "ps_5_0", "OpaqueCasterPS")));
		if (!fullscreenVS || !panePS || !initializePS || !opaquePS)
			return false;
		Util::SetResourceName(fullscreenVS.get(), "InteriorSun::Fullscreen VS");
		Util::SetResourceName(panePS.get(), "InteriorSun::Pane PS");
		Util::SetResourceName(initializePS.get(), "InteriorSun::InitializeDepth PS");
		Util::SetResourceName(opaquePS.get(), "InteriorSun::OpaqueCaster PS");

		D3D11_RASTERIZER_DESC rs{};
		rs.FillMode = D3D11_FILL_SOLID;
		// Only the sun-facing side admits light into the room.
		rs.CullMode = D3D11_CULL_BACK;
		rs.DepthClipEnable = true;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rs, raster.put()));
		Util::SetResourceName(raster.get(), "InteriorSun::WindowRaster");
		D3D11_BLEND_DESC blend{};
		auto& rt = blend.RenderTarget[0];
		rt.BlendEnable = true;
		rt.SrcBlend = rt.DestBlend = rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
		DX::ThrowIfFailed(device->CreateBlendState(&blend, furthestPane.put()));
		Util::SetResourceName(furthestPane.get(), "InteriorSun::FurthestPane");
		D3D11_DEPTH_STENCIL_DESC depth{};
		depth.DepthEnable = true;
		depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depth, writeDepth.put()));
		Util::SetResourceName(writeDepth.get(), "InteriorSun::WriteWindowDepth");
		D3D11_SAMPLER_DESC sampling{};
		sampling.Filter = D3D11_FILTER_ANISOTROPIC;
		sampling.MaxAnisotropy = D3D11_MAX_MAXANISOTROPY;
		sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampling.MaxLOD = D3D11_FLOAT32_MAX;
		for (UINT mode = 0; mode < samplers.size(); ++mode) {
			sampling.AddressU = (mode & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
			sampling.AddressV = (mode & 1) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
			DX::ThrowIfFailed(device->CreateSamplerState(&sampling, samplers[mode].put()));
			Util::SetResourceName(samplers[mode].get(), "InteriorSun::WindowSampler %u", mode);
		}
		constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PaneConstants>(), "InteriorSun::PaneConstants");
		return true;
	}

	ID3D11InputLayout* Pipeline::Layout(UINT uvOffset)
	{
		auto [it, inserted] = layouts.try_emplace(uvOffset);
		if (inserted) {
			D3D11_INPUT_ELEMENT_DESC elements[]{
				{ "POSITION", 0, uvOffset < 16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uvOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 }
			};
			DX::ThrowIfFailed(globals::d3d::device->CreateInputLayout(elements, ARRAYSIZE(elements), bytecode->GetBufferPointer(), bytecode->GetBufferSize(), it->second.put()));
			Util::SetResourceName(it->second.get(), "InteriorSun::WindowLayout");
		}
		return it->second.get();
	}

	ID3D11PixelShader* Pipeline::UtilityCaster(const RE::BSShader& shader, uint32_t descriptor, bool opaque)
	{
		auto [it, inserted] = utilityCasters[opaque].try_emplace(descriptor);
		if (inserted) {
			auto defines = SIE::ShaderCache::GetTechniqueDefines(shader, descriptor);
			defines.emplace_back("INTERIOR_WINDOW_CASTER", "1");
			it->second.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", defines, "ps_5_0", opaque ? "ClipWindowCaster" : "main")));
		}
		return it->second.get();
	}

	void SizeCascade(Cascade& cascade, UINT width, UINT height)
	{
		if (cascade.width == width && cascade.height == height && cascade.srv)
			return;
		auto device = globals::d3d::device;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		winrt::com_ptr<ID3D11Texture2D> texture;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, texture.put()));
		Util::SetResourceName(texture.get(), "InteriorSun::WindowEntry");
		Cascade sized;
		DX::ThrowIfFailed(device->CreateRenderTargetView(texture.get(), nullptr, sized.rtv.put()));
		DX::ThrowIfFailed(device->CreateShaderResourceView(texture.get(), nullptr, sized.srv.put()));
		Util::SetResourceName(sized.rtv.get(), "InteriorSun::WindowEntry RTV");
		Util::SetResourceName(sized.srv.get(), "InteriorSun::WindowEntry SRV");
		sized.width = width;
		sized.height = height;
		cascade = std::move(sized);
	}

	bool DrawPane(const Pane& pane, const RE::BSGraphics::ViewData& eye, const RE::NiPoint3& adjust)
	{
		auto geometry = pane.geometry.get();
		auto& data = geometry->GetGeometryRuntimeData();
		auto mesh = data.rendererData;
		auto source = pane.mask->rendererTexture;
		if (!mesh || !mesh->vertexBuffer || !mesh->indexBuffer || !source || !source->resourceView || !data.shaderProperty || !data.shaderProperty->material)
			return false;
		auto& pipeline = windows.pipeline;
		auto context = globals::d3d::context;
		const auto count = geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount * 3u;
		const auto uvOffset = data.vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
		const UINT stride = UINT(std::bit_cast<uint64_t>(data.vertexDesc) & 0xfu) * sizeof(float);
		if (!count || uvOffset < 8 || uvOffset + sizeof(uint32_t) > stride)
			return false;
		auto transform = geometry->world;
		transform.translate -= adjust;
		alignas(16) DirectX::XMFLOAT3X4 world;
		Util::StoreTransform3x4NoScale(world, transform);
		const float4x4 viewProj = eye.viewProjMat;
		PaneConstants cb{};
		DirectX::XMStoreFloat4x4(&cb.worldViewProj, DirectX::XMMatrixScaling(transform.scale, transform.scale, transform.scale) *
														DirectX::XMLoadFloat3x4(&world) * DirectX::XMLoadFloat4x4(&viewProj));
		auto material = data.shaderProperty->material;
		cb.uvTransform = float4{ material->texCoordScale[0].x, material->texCoordScale[0].y, material->texCoordOffset[0].x, material->texCoordOffset[0].y };
		cb.alphaReference = data.alphaProperty && data.alphaProperty->GetAlphaTesting() ? data.alphaProperty->alphaThreshold / 255.0f : 0.5f;
		pipeline.constants->Update(cb);
		context->IASetInputLayout(pipeline.Layout(uvOffset));
		auto vertices = Util::AsReal(mesh->vertexBuffer);
		const UINT offset = 0;
		context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
		context->IASetIndexBuffer(Util::AsReal(mesh->indexBuffer), DXGI_FORMAT_R16_UINT, 0);
		auto mask = Util::AsReal(source->resourceView);
		const auto mode = material->GetType() == RE::BSShaderMaterial::Type::kEffect ?
		                      UINT(static_cast<RE::BSEffectShaderMaterial*>(material)->effectClampMode.get()) :
		                      UINT(static_cast<RE::BSLightingShaderMaterialBase*>(material)->textureClampMode);
		auto sampler = pipeline.samplers[mode & 3u].get();
		context->PSSetSamplers(0, 1, &sampler);
		context->PSSetShaderResources(0, 1, &mask);
		context->DrawIndexed(count, 0, 0);
		return true;
	}

	void UnbindCaster()
	{
		if (!windows.casterBound)
			return;
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* none{};
		context->PSSetShaderResources(kCasterSlot, 1, &none);
		winrt::com_ptr<ID3D11PixelShader> bound;
		context->PSGetShader(bound.put(), nullptr, nullptr);
		if (windows.overridePS && bound.get() == windows.overridePS)
			context->PSSetShader(windows.savedPS.get(), nullptr, 0);
		windows.overridePS = nullptr;
		windows.savedPS = nullptr;
		windows.casterBound = false;
	}

	bool IsCurrentTarget()
	{
		if (!windows.current)
			return false;
		auto matches = [](const auto& state) { return state.depthStencil == windows.target && state.depthStencilSlice == windows.slice; };
		return globals::game::isVR ? matches(globals::game::shadowState->GetVRRuntimeData()) : matches(globals::game::shadowState->GetRuntimeData());
	}

	bool Rasterize(Cascade& cascade, const RE::BSGraphics::ViewData& eye, const RE::NiPoint3& adjust, ID3D11DepthStencilView* dsv)
	{
		CS_GPU_PASS("InteriorSun::WindowShadow");
		auto context = globals::d3d::context;
		auto& pipeline = windows.pipeline;
		Util::FullscreenPassScope scope(context);
		D3D11_VIEWPORT viewport{ eye.viewPort[0] * cascade.width, (1.0f - eye.viewPort[2]) * cascade.height,
			(eye.viewPort[1] - eye.viewPort[0]) * cascade.width, (eye.viewPort[2] - eye.viewPort[3]) * cascade.height, 0.0f, 1.0f };
		if (viewport.Width <= 0 || viewport.Height <= 0)
			return false;
		context->RSSetViewports(1, &viewport);
		context->RSSetState(pipeline.raster.get());
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		const float clear[4]{};
		context->ClearRenderTargetView(cascade.rtv.get(), clear);
		auto rtv = cascade.rtv.get();
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(pipeline.furthestPane.get(), nullptr, UINT_MAX);
		context->VSSetShader(pipeline.paneVS.get(), nullptr, 0);
		context->PSSetShader(pipeline.panePS.get(), nullptr, 0);
		auto cb = pipeline.constants->CB();
		context->VSSetConstantBuffers(0, 1, &cb);
		const UINT alphaReferenceSlot = globals::game::isVR ? 13 : 11;
		context->PSSetConstantBuffers(alphaReferenceSlot, 1, &cb);
		bool drew = false;
		for (auto& pane : windows.panes)
			drew |= DrawPane(pane, eye, adjust);
		// The volumetric pass intersects both depth maps, so only the main map should impose the pane's alpha.
		if (cascade.detectOpenings || !drew || !dsv)
			return drew || cascade.detectOpenings;

		context->OMSetRenderTargets(0, nullptr, dsv);
		context->OMSetDepthStencilState(pipeline.writeDepth.get(), 0);
		context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
		context->IASetInputLayout(nullptr);
		context->VSSetShader(pipeline.fullscreenVS.get(), nullptr, 0);
		context->PSSetShader(pipeline.initializePS.get(), nullptr, 0);
		auto entry = cascade.srv.get();
		context->PSSetShaderResources(0, 1, &entry);
		context->Draw(3, 0);
		return true;
	}

	bool DrawCascade(UINT index, UINT target, UINT slice, const RE::BSGraphics::ViewData& eye, const RE::NiPoint3& adjust)
	{
		auto& targets = globals::game::renderer->GetDepthStencilData().depthStencils;
		if (target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL || slice >= std::size(targets[target].views) || !targets[target].texture)
			return false;
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
		globals::d3d::context->OMGetRenderTargets(0, nullptr, dsv.put());
		if (!dsv || dsv.get() != Util::AsReal(targets[target].views[slice]))
			return false;
		D3D11_TEXTURE2D_DESC desc;
		Util::AsReal(targets[target].texture)->GetDesc(&desc);
		const bool auxiliary = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		auto& cascade = auxiliary ? windows.volumetricCasterEntry : windows.cascades[index];
		SizeCascade(cascade, desc.Width, desc.Height);
		const auto& settings = globals::features::interiorSun.settings;
		cascade.detectOpenings = !auxiliary && settings.DetectOpenings && !windows.detectionFailed &&
		                         windows.detector.Prepare(cascade.openings, target, slice, eye, adjust, settings.MinimumOpeningWidth, settings.MinimumDepthDrop, settings.MinimumRimCoverage, settings.ForceDoubleSidedRendering);
		if (!Rasterize(cascade, eye, adjust, auxiliary ? nullptr : dsv.get()))
			return false;
		windows.current = &cascade;
		windows.target = target;
		windows.slice = slice;
		cascade.written = true;
		return true;
	}

	void BeginCamera(RE::NiCamera* camera)
	{
		UnbindCaster();
		windows.current = nullptr;
		auto scene = globals::game::smState->shadowSceneNode[0];
		auto sun = scene ? scene->GetRuntimeData().sunShadowDirLight : nullptr;
		if (!sun)
			return;
		if (globals::game::isVR) {
			auto& descriptors = sun->GetVRRuntimeData().shadowmapDescriptors;
			auto& state = globals::game::shadowState->GetVRRuntimeData();
			for (UINT i = 0; i < std::min({ sun->shadowMapCount, descriptors.size(), UINT(kCascadeCount) }); ++i) {
				auto& descriptor = descriptors[i];
				for (UINT eye = 0; eye < std::size(descriptor.camera); ++eye)
					if (descriptor.camera[eye].get() == camera &&
						(DrawCascade(i, descriptor.renderTarget, descriptor.shadowmapIndex, state.cameraData.getEye(eye), state.posAdjust.getEye(eye)) ||
							DrawCascade(i, descriptor.vrRenderTarget[eye], descriptor.shadowmapIndex, state.cameraData.getEye(eye), state.posAdjust.getEye(eye))))
						return;
			}
		} else {
			auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
			auto& state = globals::game::shadowState->GetRuntimeData();
			for (UINT i = 0; i < std::min({ sun->shadowMapCount, descriptors.size(), UINT(kCascadeCount) }); ++i) {
				auto& descriptor = descriptors[i];
				if (descriptor.camera.get() == camera && DrawCascade(i, descriptor.renderTarget, descriptor.shadowmapIndex, state.cameraData.getEye(), state.posAdjust.getEye()))
					return;
			}
		}
	}
}

void InteriorSun::LoadWindowTextures()
{
	auto path = Util::PathHelpers::GetCommunityShaderPath() / "InteriorSun" / "InteriorSunWindows.json";
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return;
	auto config = Util::FileHelpers::ReadJsonObject(path, "Interior Sun window textures");
	if (!config)
		return;
	for (const auto& [source, value] : config->items()) {
		auto key = AssetPath(source.c_str(), "textures/");
		auto original = std::filesystem::path(key);
		if (original.has_root_path() || std::ranges::find(original, "..") != original.end() || original.extension() != ".dds")
			continue;
		WindowTextureRule rule;
		const json* mask = &value;
		if (value.is_object()) {
			if ((value.contains("whitelist") && !ReadMeshList(value.at("whitelist"), rule.whitelist.emplace())) ||
				(value.contains("blacklist") && !ReadMeshList(value.at("blacklist"), rule.blacklist))) {
				logger::warn("[Interior Sun] Invalid mesh list for {}", source);
				continue;
			}
			mask = value.contains("mask") ? &value.at("mask") : nullptr;
		}
		if (!mask || mask->is_null()) {
			windows.masks.emplace(key, std::move(rule));
			continue;
		}
		if (!mask->is_string())
			continue;
		auto file = std::filesystem::path(mask->get<std::string>());
		if (!file.has_filename() || file != file.filename() || Util::FixFilePath(file.extension().string()) != ".dds") {
			logger::warn("[Interior Sun] Invalid sibling DDS mapping for {}", source);
			continue;
		}
		auto replacement = (original.parent_path() / file).make_preferred().string();
		RE::NiPointer<RE::NiTexture> texture;
		if (RE::BSResourceNiBinaryStream(replacement).good())
			RE::BSShaderManager::GetTexture(replacement.c_str(), true, texture, false);
		if (!texture || texture->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get()) {
			logger::warn("[Interior Sun] Could not load window shadow texture {}", replacement);
			continue;
		}
		rule.mask.reset(static_cast<RE::NiSourceTexture*>(texture.get()));
		windows.masks.emplace(key, std::move(rule));
	}
	logger::info("[Interior Sun] Loaded {} shadow-only window texture mappings", windows.masks.size());
}

void InteriorSun::PreShadowMaps()
{
	renderThread = true;
	if (windows.receiverBound) {
		ID3D11ShaderResourceView* none[kCascadeCount]{};
		globals::d3d::context->PSSetShaderResources(kReceiverSlot, UINT(kCascadeCount), none);
		globals::d3d::context->CSSetShaderResources(kReceiverSlot, UINT(kCascadeCount), none);
		windows.receiverBound = false;
	}
	auto cell = globals::game::tes->interiorCell;
	if (IsInteriorWithSun(cell))
		ApplyInteriorShadowDistance();
	if (windows.masks.empty() || !IsInteriorWithSun(cell) || !globals::shaderCache->IsEnabled() || !globals::state->ShaderEnabled(RE::BSShader::Type::Utility)) {
		if (windows.cell)
			OnSceneTransitionReset(true);
		return;
	}
	if (windows.cell != cell || std::chrono::steady_clock::now() >= windows.nextScan)
		CollectPanes(cell);
	if ((windows.panes.empty() && !settings.DetectOpenings) || windows.failed)
		return;
	try {
		if (!windows.pipeline.constants && !windows.pipeline.Initialize()) {
			windows.failed = true;
			return;
		}
		windows.shadowPhase = true;
		for (auto& cascade : windows.cascades)
			cascade.written = false;
	} catch (const std::exception& e) {
		logger::error("[Interior Sun] Could not initialize window shadows: {}", e.what());
		windows.failed = true;
	}
}

void InteriorSun::StartShadowCamera::thunk(RE::BSShaderAccumulator* accumulator, RE::NiCamera* camera)
{
	if (renderThread && windows.shadowPhase && !windows.failed && camera) {
		try {
			BeginCamera(camera);
		} catch (const std::exception& e) {
			logger::error("[Interior Sun] Could not draw window shadow map: {}", e.what());
			windows.failed = true;
		}
	}
	func(accumulator, camera);
}

void InteriorSun::OnDraw()
{
	if (!renderThread || !windows.shadowPhase || windows.panes.empty())
		return;
	if (windows.failed || !IsCurrentTarget()) {
		UnbindCaster();
		return;
	}
	auto context = globals::d3d::context;
	if (!windows.casterBound) {
		auto srv = windows.current->srv.get();
		context->PSSetShaderResources(kCasterSlot, 1, &srv);
		windows.casterBound = true;
	}
	winrt::com_ptr<ID3D11PixelShader> bound;
	context->PSGetShader(bound.put(), nullptr, nullptr);
	if (!windows.overridePS || bound.get() != windows.overridePS)
		windows.savedPS = bound;
	auto replacement = windows.pipeline.opaquePS.get();
	auto state = globals::state;
	if (state->currentShader && state->currentShader->shaderType.get() == RE::BSShader::Type::Utility)
		replacement = windows.pipeline.UtilityCaster(*state->currentShader, windows.savedPS ? state->modifiedPixelDescriptor : state->modifiedVertexDescriptor, !windows.savedPS);
	else if (windows.savedPS)
		replacement = windows.savedPS.get();
	if (!replacement) {
		windows.failed = true;
		UnbindCaster();
		return;
	}
	if (replacement != bound.get())
		context->PSSetShader(replacement, nullptr, 0);
	windows.overridePS = replacement;
}

bool InteriorSun::ShouldSkipRenderPass(const RE::BSRenderPass* pass)
{
	return renderThread && windows.shadowPhase && IsCurrentTarget() && windows.geometries.contains(pass->geometry);
}

void InteriorSun::PostShadowMaps()
{
	if (!windows.shadowPhase)
		return;
	UnbindCaster();
	windows.current = nullptr;
	windows.shadowPhase = false;
	ID3D11ShaderResourceView* entries[kCascadeCount]{};
	for (size_t i = 0; i < kCascadeCount; ++i) {
		auto& cascade = windows.cascades[i];
		if (cascade.written) {
			if (cascade.detectOpenings) {
				try {
					auto& merged = windows.mergedEntry;
					SizeCascade(merged, cascade.width, cascade.height);
					auto& pipeline = windows.pipeline;
					windows.detector.Draw(cascade.openings, cascade.srv.get(), merged.rtv.get(), merged.srv.get(),
						pipeline.fullscreenVS.get(), pipeline.raster.get(), pipeline.writeDepth.get(), pipeline.samplers);
					std::swap(cascade.srv, merged.srv);
					std::swap(cascade.rtv, merged.rtv);
				} catch (const std::exception& e) {
					logger::error("[Interior Sun] Could not combine physical openings: {}", e.what());
					windows.detectionFailed = true;
					cascade.detectOpenings = false;
					if (windows.panes.empty())
						continue;
				}
			}
			entries[i] = cascade.srv.get();
			windows.receiverBound = true;
		}
	}
	globals::d3d::context->PSSetShaderResources(kReceiverSlot, UINT(kCascadeCount), entries);
	globals::d3d::context->CSSetShaderResources(kReceiverSlot, UINT(kCascadeCount), entries);
}

void InteriorSun::OnSceneTransitionReset(bool)
{
	windows.cell = nullptr;
	windows.references.clear();
	windows.detector.SetShell({});
	windows.panes.clear();
	windows.geometries.clear();
}

void InteriorSun::ClearShaderCache()
{
	UnbindCaster();
	windows.pipeline = {};
	windows.detector = {};
	windows.nextScan = {};
	windows.detectionFailed = false;
	windows.failed = false;
}

void InteriorSun::RegisterUxActions()
{
	FEATURE_RENDER_QUERY("windowShadows", "Report matched window textures and meshes, sun cascades drawn, physical opening detection, and shadow distances.", [](const Feature* feature, const json&) -> json {
		auto result = json::object();
		const auto self = static_cast<const InteriorSun*>(feature);
		result["configuredDistance"] = self->settings.InteriorShadowDistance;
		result["activeDistance"] = RE::BSShaderManager::GetShadowDistanceCurrent();
		result["sunlit"] = self->IsActiveInteriorSun();
		auto& paths = result["matchedTextures"] = json::object();
		for (const auto& pane : windows.panes)
			paths[pane.sourcePath] = paths.value(pane.sourcePath, 0u) + 1u;
		auto& meshes = result["matchedMeshes"] = json::object();
		for (const auto& [id, reference] : windows.references)
			for (const auto& pane : reference.panes) {
				auto& count = meshes[pane.sourcePath][reference.meshPath];
				count = count.is_null() ? 1u : count.get<unsigned>() + 1u;
			}
		result["mappings"] = windows.masks.size();
		result["panes"] = windows.panes.size();
		result["cascades"] = windows.receiverBound ? std::ranges::count(windows.cascades, true, &Cascade::written) : 0;
		result["openingCascades"] = windows.receiverBound ? std::ranges::count_if(windows.cascades, [](const Cascade& c) { return c.written && c.detectOpenings; }) : 0;
		result["failed"] = windows.failed;
		return result;
	});
}

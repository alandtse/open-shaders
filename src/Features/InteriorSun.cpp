#include "InteriorSun.h"
#include "Features/InteriorSun/WindowRays.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "State.h"

#define I18N_KEY_PREFIX "feature.interior_sun."

#include <numbers>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	InteriorSun::Settings,
	ForceDoubleSidedRendering,
	InteriorShadowDistance,
	WindowLightOnly,
	DetectOpenings,
	FrameShadowsFromGlowMaps,
	GlowMapThreshold)

namespace
{
	// Appends a shape's triangles in world space from its CPU geometry copy. False when the shape keeps none in a readable layout.
	bool AppendWorldTriangles(RE::BSGeometry* geometry, std::vector<float3>& vertices, std::vector<float2>* uvs = nullptr, float2 uvScale = { 1, 1 }, float2 uvOffset = { 0, 0 })
	{
		const auto triShape = geometry->AsTriShape();
		const auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!triShape || !rendererData || !rendererData->rawVertexData || !rendererData->rawIndexData)
			return false;

		// Positions must be full-precision floats at offset 0
		auto& desc = rendererData->vertexDesc;
		const uint32_t stride = desc.GetSize();
		if (stride < 16 || desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0) != 16)
			return false;

		const auto& counts = triShape->GetTrishapeRuntimeData();
		const size_t firstVertex = vertices.size();
		for (uint32_t i = 0; i < counts.triangleCount * 3u; ++i) {
			const uint32_t index = rendererData->rawIndexData[i];
			if (index >= counts.vertexCount) {
				vertices.resize(firstVertex);
				if (uvs)
					uvs->resize(firstVertex);
				return false;
			}
			const auto position = reinterpret_cast<const float*>(rendererData->rawVertexData + static_cast<size_t>(index) * stride);
			const auto world = geometry->world * RE::NiPoint3(position[0], position[1], position[2]);
			vertices.emplace_back(world.x, world.y, world.z);
			if (uvs) {
				const auto uv = reinterpret_cast<const uint16_t*>(rendererData->rawVertexData + static_cast<size_t>(index) * stride + 16);
				uvs->emplace_back(DirectX::PackedVector::XMConvertHalfToFloat(uv[0]) * uvScale.x + uvOffset.x, DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) * uvScale.y + uvOffset.y);
			}
		}
		return true;
	}
}

void InteriorSun::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("force_double_sided"), "Force Double-Sided Rendering"), &settings.ForceDoubleSidedRendering);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("force_double_sided_tooltip"),
							  "Disables backface culling during sun shadowmap rendering in interiors. "
							  "Will prevent most light leaking through unmasked/unprepared interiors at a small performance cost. "));
	}
	if (ImGui::SliderFloat(T(TKEY("interior_shadow_distance"), "Interior Shadow Distance"), &settings.InteriorShadowDistance, 1000.0f, 8000.0f)) {
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
		auto tes = globals::game::tes;
		SetShadowDistance(tes && tes->interiorCell);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_shadow_distance_tooltip"),
							  "Sets the distance shadows are rendered at in interiors. "
							  "Lower values provide higher quality shadows and improved performance but may cause distant interior spaces to light up incorrectly. "));
	}
	ImGui::Checkbox(T(TKEY("window_light_only"), "Sunlight Through Windows Only"), &settings.WindowLightOnly);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("window_light_only_tooltip"),
							  "Only lets the sun and moon into an interior through its window glass, which stops light leaking through walls, seams and roofs. "
							  "Has no effect in interiors whose windows are not marked as glass."));
	}
	ImGui::Checkbox(T(TKEY("frame_shadows_from_glow_maps"), "Window Frames Cast Shadows"), &settings.FrameShadowsFromGlowMaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("frame_shadows_from_glow_maps_tooltip"),
							  "Lets light through only the bright parts of each window's glow map, so frames and mullions cast shadows. "
							  "Works with window retextures through their own glow maps. Takes effect on entering an interior."));
	}
	if (settings.FrameShadowsFromGlowMaps)
		ImGui::SliderFloat(T(TKEY("glow_map_threshold"), "Window Frame Threshold"), &settings.GlowMapThreshold, 0.05f, 0.95f);
	ImGui::Checkbox(T(TKEY("detect_openings"), "Detect Openings"), &settings.DetectOpenings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("detect_openings_tooltip"),
							  "Also lets light in through see-through shapes in the outer walls, such as lattices, that have nothing behind them. "
							  "Found automatically when entering an interior."));
	}
}

void InteriorSun::LoadSettings(json& o_json)
{
	settings = o_json;
	if (gInteriorShadowDistance)
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
}

void InteriorSun::SaveSettings(json& o_json)
{
	o_json = settings;
}

void InteriorSun::RestoreDefaultSettings()
{
	settings = {};
	if (gInteriorShadowDistance)
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
}

void InteriorSun::PostPostLoad()
{
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

	// Hooks and patch to enable directional lighting for interiors
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x399, 0x37D, 0x639));
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x3AE, 0x392, 0x64E));
	REL::safe_fill(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x397, 0x37B, 0x637), REL::NOP, 2);

	// Hook for overriding the rooms and portals passed to the directional light culling step to fix light leaking through unrendered geometry
	stl::detour_thunk<DirShadowLightCulling>(REL::RelocationID(101498, 108492));

	// Hooks and patches in AIProcess::CalculateLightValue to force interior cells with directional lights to perform raycast checks
	REL::safe_fill(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1E7, 0x1F1), REL::NOP, REL::Module::IsAE() ? 2 : 6);
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1ED, 0x1F3));
	REL::safe_fill(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x2CA, 0x22B), REL::NOP, REL::Module::IsAE() ? 6 : 2);

	gShadowDistance = &RE::BSShaderManager::GetShadowDistanceCurrent();
	gInteriorShadowDistance = reinterpret_cast<float*>(REL::RelocationID(513755, 391724).address());
	*gInteriorShadowDistance = settings.InteriorShadowDistance;

	// Patches BSShadowDirectionalLight::SetFrameCamera to read the correct shadow distance value in interior cells.
	const std::uintptr_t aeOffset = REL::Module::IsAtLeast(REL::Version(1, 7, 99, 0)) ? 0xE9C : 0xE6C;
	const std::uintptr_t address = REL::RelocationID(101499, 108496).address() + REL::Relocate<std::uintptr_t>(0xD62, aeOffset, 0xE72);
	const std::int32_t displacement = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(gShadowDistance) - (address + 8));
	REL::safe_write(address + 4, &displacement, sizeof(displacement));

	rasterStateCullMode = globals::game::isVR ? &globals::game::shadowState->GetVRRuntimeData().rasterStateCullMode : &globals::game::shadowState->GetRuntimeData().rasterStateCullMode;

	// Hook to draw window holes into the sun shadow maps before their casters. VR keeps a camera per eye in its descriptors, which is not handled.
	if (!globals::game::isVR)
		stl::write_vfunc<0x25, BSShaderAccumulator_StartAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);

	logger::info("[Interior Sun] Installed hooks");
}

void InteriorSun::EarlyPrepass()
{
	const auto cell = globals::game::tes->interiorCell;
	isInteriorWithSun = IsInteriorWithSun(cell);
	if (!isInteriorWithSun && !arraysCleared) {
		ClearArrays();
		currentCell = nullptr;
		currentPortalGraph = nullptr;
	}

	if (cell && isInteriorWithSun)
		UpdateWindowGlass(cell);
	else if (windowGlassCell)
		ReleaseWindowGlass();

	// Changing these needs the cell scanned again (openings, glows) and the holes rebuilt
	if (settings.DetectOpenings != lastDetectOpenings || settings.FrameShadowsFromGlowMaps != lastFrameShadows) {
		lastDetectOpenings = settings.DetectOpenings;
		lastFrameShadows = settings.FrameShadowsFromGlowMaps;
		lastCandidateTriangles.reset();
		windowOpeningVertices.clear();
		windowOpeningUVs.clear();
		windowOpeningRuns.clear();
		windowGlassCellFrames = 0;
		forceHolesUpload = true;
	}

	renderThread = std::this_thread::get_id();
}

void InteriorSun::SetupResources()
{
	auto device = globals::d3d::device;

	windowHolesCB = new ConstantBuffer(ConstantBufferDesc<WindowHolesCB>(), "InteriorSun::WindowHolesCB");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, windowHolesSampler.put()));
	Util::SetResourceName(windowHolesSampler.get(), "InteriorSun::WindowHolesSampler");

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, windowHolesRasterState.put()));
	Util::SetResourceName(windowHolesRasterState.get(), "InteriorSun::WindowHolesRasterState");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = true;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, windowHolesDepthState.put()));
	Util::SetResourceName(windowHolesDepthState.get(), "InteriorSun::WindowHolesDepthState");
}

void InteriorSun::ClearShaderCache()
{
	windowHolesVS = nullptr;
	windowHolesPS = nullptr;
}

ID3D11PixelShader* InteriorSun::GetWindowHolesPS()
{
	if (!windowHolesPS)
		windowHolesPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\WindowHolesVS.hlsl", {}, "ps_5_0", "psmain")));
	return windowHolesPS.get();
}

ID3D11VertexShader* InteriorSun::GetWindowHolesVS()
{
	if (!windowHolesVS)
		windowHolesVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\WindowHolesVS.hlsl", {}, "vs_5_0")));
	return windowHolesVS.get();
}

void InteriorSun::UpdateWindowGlass(RE::TESObjectCELL* cell)
{
	if (cell != windowGlassCell) {
		ReleaseWindowGlass();
		windowGlassCell = cell;
	}

	++windowGlassCellFrames;
	if (std::ranges::find(WindowGlassScanFrames, windowGlassCellFrames) != std::end(WindowGlassScanFrames))
		CollectWindowGlass(cell);
	TakeOpeningScan();
}

void InteriorSun::CollectWindowGlass(RE::TESObjectCELL* cell)
{
	std::vector<float3> vertices;
	std::vector<float2> uvs;
	std::vector<HoleDraw> runs;
	uint32_t windowGlassShapes = 0, windowGlassShapesWithGlow = 0, windowGlassShapesSkipped = 0;
	auto scan = settings.DetectOpenings ? std::make_shared<OpeningScan>() : nullptr;

	cell->ForEachReference([&](RE::TESObjectREFR* ref) {
		const auto base = ref->GetBaseObject();
		const auto root = base ? ref->Get3D() : nullptr;
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;

		// Windows and walls are statics; skipping everything else keeps the scan short
		if (!base->Is(RE::FormType::Static))
			return RE::BSContainer::ForEachResult::kContinue;

		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
			const auto& data = geometry->GetGeometryRuntimeData();
			const auto property = data.shaderProperty.get();
			if (!property)
				return RE::BSVisit::BSVisitControl::kContinue;

			// The flag mesh authors put on window glass, which also keeps it out of forced double-sided rendering
			if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask)) {
				// Its glow map marks the panes, white on glass and black on the frame, so the frame can stay shadowed
				const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
				const auto material = lighting ? static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material) : nullptr;
				RE::NiPointer<RE::NiSourceTexture> glow;
				if (material && material->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap)
					glow = static_cast<const RE::BSLightingShaderMaterialGlowmap*>(material)->glowTexture;
				const float2 uvScale = material ? float2(material->texCoordScale[0].x, material->texCoordScale[0].y) : float2(1, 1);
				const float2 uvOffset = material ? float2(material->texCoordOffset[0].x, material->texCoordOffset[0].y) : float2(0, 0);
				const auto first = static_cast<uint32_t>(vertices.size());
				if (AppendWorldTriangles(geometry, vertices, &uvs, uvScale, uvOffset)) {
					++windowGlassShapes;
					if (glow && glow->rendererTexture && glow->rendererTexture->resourceView)
						++windowGlassShapesWithGlow;
					else
						glow = nullptr;
					runs.push_back({ first, static_cast<uint32_t>(vertices.size()) - first, glow });
				} else
					++windowGlassShapesSkipped;
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			if (!scan)
				return RE::BSVisit::BSVisitControl::kContinue;

			// See-through shapes that cast are candidates: as casters they still shadow their own solid parts once opened
			const auto alpha = data.alphaProperty.get();
			if (alpha && alpha->GetAlphaTesting()) {
				if (!property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows))
					return RE::BSVisit::BSVisitControl::kContinue;
				// Only its see-through pixels can let light in, so it needs a readable diffuse texture
				const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
				const auto material = lighting ? static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material) : nullptr;
				const auto diffuse = material ? material->diffuseTexture : nullptr;
				if (!diffuse || !diffuse->rendererTexture || !diffuse->rendererTexture->resourceView)
					return RE::BSVisit::BSVisitControl::kContinue;
				const float2 uvScale(material->texCoordScale[0].x, material->texCoordScale[0].y);
				const float2 uvOffset(material->texCoordOffset[0].x, material->texCoordOffset[0].y);
				const auto first = static_cast<uint32_t>(scan->candidateTriangles.size());
				if (AppendWorldTriangles(geometry, scan->candidateTriangles, &scan->candidateUVs, uvScale, uvOffset)) {
					const auto model = base->As<RE::TESModel>();
					OpeningCandidate candidate{ model ? model->GetModel() : "", geometry->name.c_str(), ref->GetFormID(), first,
						static_cast<uint32_t>(scan->candidateTriangles.size()) - first };
					candidate.diffuse = diffuse;
					candidate.alphaThreshold = alpha->alphaThreshold / 255.0f;
					scan->candidates.push_back(std::move(candidate));
				}
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return RE::BSContainer::ForEachResult::kContinue;
	});

	// The shell test runs again only when the candidates changed since the last scan of this cell. The room's
	// geometry, the bulk of the scan's time, is copied only then.
	std::vector<float3> scanKey;
	if (scan)
		scanKey = scan->candidateTriangles;
	const bool runShellTest = scan && !scanKey.empty() && (!lastCandidateTriangles || *lastCandidateTriangles != scanKey);
	if (runShellTest)
		CollectOccluders(cell, *scan);

	if (runShellTest) {
		scan->cell = cell;
		lastCandidateTriangles = std::make_shared<std::vector<float3>>(std::move(scanKey));
		openingScan = scan;
		std::thread([scan] {
			RunOpeningScan(*scan);
			scan->done = true;
		}).detach();
	}

	if (vertices != windowGlassVertices || std::exchange(forceHolesUpload, false)) {
		windowGlassVertices = std::move(vertices);
		windowGlassUVs = std::move(uvs);
		windowGlassRuns = std::move(runs);
		logger::info("[Interior Sun] Window glass in cell {:08X}: {} shapes ({} with glow maps), {} triangles, {} unreadable shapes", cell->GetFormID(), windowGlassShapes, windowGlassShapesWithGlow, windowGlassVertices.size() / 3, windowGlassShapesSkipped);
		UploadWindowHoles(cell);
	}
}

void InteriorSun::CollectOccluders(RE::TESObjectCELL* cell, OpeningScan& scan)
{
	cell->ForEachReference([&](RE::TESObjectREFR* ref) {
		const auto base = ref->GetBaseObject();
		const auto root = base && base->Is(RE::FormType::Static) ? ref->Get3D() : nullptr;
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
			const auto& data = geometry->GetGeometryRuntimeData();
			const auto property = data.shaderProperty.get();
			const auto alpha = data.alphaProperty.get();
			// Solid shapes only: glass, see-through and blended shapes don't block the test's rays
			if (!property || property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask) || (alpha && (alpha->GetAlphaTesting() || alpha->GetAlphaBlending())))
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto first = static_cast<uint32_t>(scan.occluders.size());
			if (AppendWorldTriangles(geometry, scan.occluders))
				scan.occluderShapes.push_back(first);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return RE::BSContainer::ForEachResult::kContinue;
	});
	scan.occluderShapes.push_back(static_cast<uint32_t>(scan.occluders.size()));
}

void InteriorSun::UploadWindowHoles(const RE::TESObjectCELL* cell)
{
	struct HoleVertex
	{
		float3 position;
		float2 uv;
	};
	std::vector<HoleVertex> holes;
	windowHoleDraws.clear();

	// Glass grouped by glow map, so each map is bound once; without frame shadows every pane opens whole
	std::vector<HoleDraw> runs;
	for (auto run : windowGlassRuns) {
		if (!settings.FrameShadowsFromGlowMaps)
			run.texture = nullptr;
		run.mode = run.texture ? kBrightGlow : kWhole;
		runs.push_back(run);
	}
	std::vector<HoleDraw> openingRuns;
	if (settings.DetectOpenings)
		openingRuns = windowOpeningRuns;
	const auto sameDraw = [](const HoleDraw& a, const HoleDraw& b) { return a.texture == b.texture && a.mode == b.mode && a.alphaThreshold == b.alphaThreshold; };
	const auto byTexture = [](const HoleDraw& run) { return run.texture.get(); };
	std::ranges::stable_sort(runs, {}, byTexture);
	std::ranges::stable_sort(openingRuns, {}, byTexture);
	const auto append = [&](const std::vector<HoleDraw>& list, const std::vector<float3>& positions, const std::vector<float2>& uvs) {
		for (const auto& run : list) {
			if (windowHoleDraws.empty() || !sameDraw(windowHoleDraws.back(), run))
				windowHoleDraws.push_back({ static_cast<uint32_t>(holes.size()), 0, run.texture, run.mode, run.alphaThreshold });
			for (uint32_t i = run.first; i < run.first + run.count; ++i)
				holes.push_back({ positions[i], uvs[i] });
			windowHoleDraws.back().count += run.count;
		}
	};
	append(runs, windowGlassVertices, windowGlassUVs);
	append(openingRuns, windowOpeningVertices, windowOpeningUVs);

	windowGlass.reset();
	windowGlassVertexCount = 0;
	if (holes.empty())
		return;

	try {
		const uint64_t count = holes.size();
		D3D11_SUBRESOURCE_DATA init{ holes.data(), 0, 0 };
		windowGlass = std::make_unique<Buffer>(StructuredBufferDesc<HoleVertex>(count, false), &init, "InteriorSun::WindowGlass");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = static_cast<UINT>(count);
		windowGlass->CreateSRV(srvDesc);
		windowGlassVertexCount = static_cast<uint32_t>(count);
	} catch (...) {
		logger::error("[Interior Sun] Failed to create the window holes buffer for cell {:08X}", cell->GetFormID());
		windowGlass.reset();
		windowHoleDraws.clear();
	}
}

void InteriorSun::TakeOpeningScan()
{
	if (!openingScan || !openingScan->done)
		return;
	const auto scan = std::exchange(openingScan, nullptr);
	if (scan->cell != windowGlassCell)
		return;

	windowOpeningVertices = std::move(scan->openings);
	windowOpeningUVs = std::move(scan->openingUVs);
	windowOpeningRuns = std::move(scan->openingRuns);
	logger::info("[Interior Sun] Openings in cell {:08X}: {} of {} candidate shapes, {} triangles", scan->cell->GetFormID(),
		std::ranges::count_if(scan->candidates, [](const auto& candidate) { return candidate.opening; }), scan->candidates.size(), windowOpeningVertices.size() / 3);

	UploadWindowHoles(scan->cell);
}

void InteriorSun::RunOpeningScan(OpeningScan& scan)
{
	// Far enough to cross the largest halls; beyond it, a ray has left the cell
	constexpr float Reach = 4096.0f;
	constexpr uint32_t SamplesPerShape = 4;

	struct Box
	{
		float3 min, max;
		uint32_t first, end;
	};
	std::vector<Box> boxes;
	for (size_t shape = 0; shape + 1 < scan.occluderShapes.size(); ++shape) {
		Box box{ float3(FLT_MAX), float3(-FLT_MAX), scan.occluderShapes[shape], scan.occluderShapes[shape + 1] };
		for (uint32_t i = box.first; i < box.end; ++i) {
			box.min = float3::Min(box.min, scan.occluders[i]);
			box.max = float3::Max(box.max, scan.occluders[i]);
		}
		if (box.first < box.end)
			boxes.push_back(box);
	}

	const auto cast = [&](const float3& origin, const float3& direction) {
		float nearest = FLT_MAX;
		for (const auto& box : boxes) {
			if (InteriorSunRays::RayHitsBox(origin, direction, std::min(nearest, Reach), box.min, box.max))
				nearest = std::min(nearest, InteriorSunRays::CastRay(origin, direction, Reach, scan.occluders, box.first, box.end));
		}
		return nearest;
	};

	// Samples on a shape's largest triangles, each looking both ways along its facing: open on one side only means the shell
	struct Measure
	{
		uint32_t openSamples = 0, samples = 0;
		float3 outward = float3(0, 0, 0);
	};
	const auto measure = [&](const std::vector<float3>& tri, uint32_t first, uint32_t count) {
		Measure m;
		std::vector<std::pair<float, uint32_t>> areas;
		for (uint32_t t = 0; t + 2 < count; t += 3) {
			const uint32_t i = first + t;
			areas.emplace_back((tri[i + 1] - tri[i]).Cross(tri[i + 2] - tri[i]).Length(), i);
		}
		std::ranges::sort(areas, std::greater{});
		for (size_t s = 0; s < std::min<size_t>(SamplesPerShape, areas.size()); ++s) {
			const uint32_t i = areas[s].second;
			if (areas[s].first < 1e-3f)
				break;
			float3 normal = (tri[i + 1] - tri[i]).Cross(tri[i + 2] - tri[i]);
			normal.Normalize();
			const float3 centre = (tri[i] + tri[i + 1] + tri[i + 2]) / 3.0f;
			// Start a little off the shape, so a frame flush with it is not hit at zero distance
			const float front = cast(centre + normal * 2.0f, normal);
			const float back = cast(centre - normal * 2.0f, -normal);
			++m.samples;
			if ((front == FLT_MAX) != (back == FLT_MAX)) {
				++m.openSamples;
				m.outward += front == FLT_MAX ? normal : -normal;
			}
		}
		if (m.outward.LengthSquared() > 1e-6f)
			m.outward.Normalize();
		return m;
	};

	for (auto& candidate : scan.candidates) {
		const auto m = measure(scan.candidateTriangles, candidate.firstTriangle, candidate.triangleCount);
		candidate.samples = m.samples;
		candidate.openSamples = m.openSamples;
		candidate.outward = m.outward;
		// Sun never comes up through a floor: rugs and floor pieces have only the void below them
		candidate.opening = candidate.samples && candidate.openSamples * 2 > candidate.samples && candidate.outward.z > -0.7f;
		if (candidate.opening) {
			const auto first = static_cast<uint32_t>(scan.openings.size());
			const auto begin = scan.candidateTriangles.begin() + candidate.firstTriangle;
			const auto uvBegin = scan.candidateUVs.begin() + candidate.firstTriangle;
			scan.openings.insert(scan.openings.end(), begin, begin + candidate.triangleCount);
			scan.openingUVs.insert(scan.openingUVs.end(), uvBegin, uvBegin + candidate.triangleCount);
			scan.openingRuns.push_back({ first, candidate.triangleCount, candidate.diffuse, kSeeThrough, candidate.alphaThreshold });
		}
	}
	// The room's geometry can run to tens of MB, and the result no longer needs it
	scan.occluders = {};
	scan.occluderShapes = {};
}

void InteriorSun::ReleaseWindowGlass()
{
	openingScan.reset();
	lastCandidateTriangles.reset();
	windowOpeningVertices.clear();
	windowOpeningUVs.clear();
	windowOpeningRuns.clear();
	windowGlass.reset();
	windowGlassVertices.clear();
	windowGlassUVs.clear();
	windowGlassRuns.clear();
	windowHoleDraws.clear();
	windowGlassCell = nullptr;
	windowGlassCellFrames = 0;
	windowGlassVertexCount = 0;
}

void InteriorSun::DrawWindowHoles(const RE::NiCamera* camera)
{
	const auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunLight)
		return;

	// Only the sun's cascades: one of their descriptors holds the camera about to be rendered
	const auto& descriptors = sunLight->GetRuntimeData().shadowmapDescriptors;
	const auto descriptorsEnd = descriptors.begin() + std::min(sunLight->shadowMapCount, descriptors.size());
	const auto descriptor = std::find_if(descriptors.begin(), descriptorsEnd, [&](const auto& a_descriptor) { return a_descriptor.camera.get() == camera; });
	if (descriptor == descriptorsEnd)
		return;

	// The engine has just bound and cleared the cascade's slice. Anything else bound means this is not that moment, so leave it alone.
	auto context = globals::d3d::context;
	const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
	const auto target = static_cast<uint32_t>(descriptor->renderTarget);
	const auto slice = descriptor->shadowmapIndex;
	winrt::com_ptr<ID3D11DepthStencilView> dsv;
	context->OMGetRenderTargets(0, nullptr, dsv.put());
	if (target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL || slice >= std::size(depthStencils[target].views) || !dsv || dsv.get() != Util::AsReal(depthStencils[target].views[slice]))
		return;

	D3D11_TEXTURE2D_DESC textureDesc;
	Util::AsReal(depthStencils[target].texture)->GetDesc(&textureDesc);

	// The engine set this camera up right before, so these are the matrices its casters will use
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	const auto eye = shadowState.cameraData.getEye();
	const auto posAdjust = shadowState.posAdjust.getEye();

	WindowHolesCB data{};
	data.ViewProj = eye.viewProjMat;
	data.PosAdjust = { posAdjust.x, posAdjust.y, posAdjust.z };
	data.GlowThreshold = settings.GlowMapThreshold;

	// Same viewport as the engine derives from the camera's port (left, right, top, bottom)
	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = eye.viewPort[0] * textureDesc.Width;
	viewport.TopLeftY = (1.0f - eye.viewPort[2]) * textureDesc.Height;
	viewport.Width = (eye.viewPort[1] - eye.viewPort[0]) * textureDesc.Width;
	viewport.Height = (eye.viewPort[2] - eye.viewPort[3]) * textureDesc.Height;
	viewport.MaxDepth = 1.0f;

	{
		CS_GPU_PASS("InteriorSun::WindowHoles");
		// Everything is shadowed from the sun except through the panes; the casters drawn next still shadow that light
		context->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
		DrawHolesInto(data, viewport);
	}
}

void InteriorSun::DrawHolesInto(WindowHolesCB data, const D3D11_VIEWPORT& viewport)
{
	auto context = globals::d3d::context;
	const auto vs = GetWindowHolesVS();
	if (!vs || !windowGlass)
		return;

	// The engine caches its D3D state, so everything this draw changes is put back
	winrt::com_ptr<ID3D11InputLayout> oldInputLayout;
	D3D11_PRIMITIVE_TOPOLOGY oldTopology;
	winrt::com_ptr<ID3D11VertexShader> oldVS;
	winrt::com_ptr<ID3D11PixelShader> oldPS;
	winrt::com_ptr<ID3D11Buffer> oldCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
	winrt::com_ptr<ID3D11Buffer> oldPSCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldPSSRV;
	winrt::com_ptr<ID3D11SamplerState> oldSampler;
	winrt::com_ptr<ID3D11RasterizerState> oldRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> oldDepthState;
	UINT oldStencilRef;
	D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT oldViewportCount = static_cast<UINT>(std::size(oldViewports));
	context->IAGetInputLayout(oldInputLayout.put());
	context->IAGetPrimitiveTopology(&oldTopology);
	context->VSGetShader(oldVS.put(), nullptr, nullptr);
	context->PSGetShader(oldPS.put(), nullptr, nullptr);
	context->VSGetConstantBuffers(0, 1, oldCB.put());
	context->VSGetShaderResources(0, 1, oldSRV.put());
	context->PSGetConstantBuffers(0, 1, oldPSCB.put());
	context->PSGetShaderResources(0, 1, oldPSSRV.put());
	context->PSGetSamplers(0, 1, oldSampler.put());
	context->RSGetState(oldRasterState.put());
	context->RSGetViewports(&oldViewportCount, oldViewports);
	context->OMGetDepthStencilState(oldDepthState.put(), &oldStencilRef);

	auto cb = windowHolesCB->CB();
	auto srv = windowGlass->srv.get();
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	context->PSSetConstantBuffers(0, 1, &cb);
	auto sampler = windowHolesSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->RSSetState(windowHolesRasterState.get());
	context->RSSetViewports(1, &viewport);
	context->OMSetDepthStencilState(windowHolesDepthState.get(), 0);
	const auto ps = GetWindowHolesPS();
	for (const auto& draw : windowHoleDraws) {
		// With a texture, a pixel shader keeps the solid parts shadowed (frames, lattice bars); without one, depth only
		const bool textured = draw.mode != kWhole && draw.texture && ps;
		if (draw.mode == kSeeThrough && !textured)
			continue;  // an opening that can't be told see-through from solid stays closed
		data.Mode = textured ? draw.mode : kWhole;
		data.AlphaThreshold = draw.alphaThreshold;
		windowHolesCB->Update(data);
		ID3D11ShaderResourceView* texture = textured ? Util::AsReal(draw.texture->rendererTexture->resourceView) : nullptr;
		context->PSSetShader(textured ? ps : nullptr, nullptr, 0);
		context->PSSetShaderResources(0, 1, &texture);
		context->Draw(draw.count, draw.first);
	}

	cb = oldCB.get();
	srv = oldSRV.get();
	context->IASetInputLayout(oldInputLayout.get());
	context->IASetPrimitiveTopology(oldTopology);
	context->VSSetShader(oldVS.get(), nullptr, 0);
	context->PSSetShader(oldPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	cb = oldPSCB.get();
	srv = oldPSSRV.get();
	sampler = oldSampler.get();
	context->PSSetConstantBuffers(0, 1, &cb);
	context->PSSetShaderResources(0, 1, &srv);
	context->PSSetSamplers(0, 1, &sampler);
	context->RSSetState(oldRasterState.get());
	context->RSSetViewports(oldViewportCount, oldViewports);
	context->OMSetDepthStencilState(oldDepthState.get(), oldStencilRef);
}

bool InteriorSun::IsInteriorWithSun(const RE::TESObjectCELL* cell)
{
	return cell && cell->cellFlags.all(RE::TESObjectCELL::Flag::kIsInteriorCell, RE::TESObjectCELL::Flag::kShowSky, RE::TESObjectCELL::Flag::kUseSkyLighting, static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows));
}

RE::TESWorldSpace* InteriorSun::GetWorldSpace::thunk(RE::TES* tes)
{
	if (const auto cell = tes->interiorCell)
		return IsInteriorWithSun(cell) ? enableInteriorSun : disableInteriorSun;
	return func(tes);
}

RE::TESWorldSpace* InteriorSun::enableInteriorSun = [] {
	alignas(RE::TESWorldSpace) static char buffer[sizeof(RE::TESWorldSpace)]{};
	return reinterpret_cast<RE::TESWorldSpace*>(buffer);
}();

RE::TESWorldSpace* InteriorSun::disableInteriorSun = [] {
	alignas(RE::TESWorldSpace) static char buffer[sizeof(RE::TESWorldSpace)] = {};
	const auto noShadows = reinterpret_cast<RE::TESWorldSpace*>(buffer);
	noShadows->flags.set(RE::TESWorldSpace::Flag::kNoSky, RE::TESWorldSpace::Flag::kFixedDimensions);
	return noShadows;
}();

void InteriorSun::DirShadowLightCulling::thunk(RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>& nodes)
{
	auto& singleton = globals::features::interiorSun;
	const auto cell = RE::TES::GetSingleton()->interiorCell;
	auto* passedJobArrays = &jobArrays;

	if (cell && singleton.isInteriorWithSun) {
		const auto* loadedData = cell->GetRuntimeData().loadedData;
		const auto portalGraph = loadedData ? loadedData->portalGraph : nullptr;
		if (portalGraph) {
			singleton.PopulateReplacementJobArrays(cell, portalGraph, dirLight, jobArrays);
			passedJobArrays = &singleton.replacementJobArrays;
		} else {
			singleton.currentCell = nullptr;
			singleton.currentPortalGraph = nullptr;
		}
	} else {
		if (!singleton.arraysCleared)
			singleton.ClearArrays();
		singleton.currentCell = nullptr;
		singleton.currentPortalGraph = nullptr;
	}

	func(dirLight, *passedJobArrays, nodes);
}

void InteriorSun::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	globals::features::interiorSun.UpdateRasterStateCullMode(a_pass, a_technique);
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void InteriorSun::BSShaderAccumulator_StartAccumulating::thunk(RE::BSShaderAccumulator* a_accumulator, RE::NiCamera* a_camera)
{
	auto& singleton = globals::features::interiorSun;
	// Accumulators are also started from culling jobs, which must not touch the D3D context
	// Only real window glass switches a cell to sun through its holes: openings alone (a cave's grate) would darken a cave mouth
	if (singleton.isInteriorWithSun && singleton.settings.WindowLightOnly && singleton.windowGlassVertexCount && !singleton.windowGlassVertices.empty() && std::this_thread::get_id() == singleton.renderThread)
		singleton.DrawWindowHoles(a_camera);
	func(a_accumulator, a_camera);
}

RE::BSEventNotifyControl InteriorSun::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == RE::MainMenu::MENU_NAME) {
		if (a_event->opening)
			globals::features::interiorSun.isInteriorWithSun = false;
	}

	return RE::BSEventNotifyControl::kContinue;
}

void InteriorSun::ClearArrays()
{
	currentCellRoomsAndPortals.clear();

	for (auto& jobArray : replacementJobArrays)
		jobArray.clear();

	arraysCleared = true;
}

void InteriorSun::PopulateReplacementJobArrays(RE::TESObjectCELL* cell, const RE::NiPointer<RE::BSPortalGraph>& portalGraph, const RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays)
{
	if (cell != currentCell || portalGraph != currentPortalGraph) {
		InitialiseOnNewCell(portalGraph);
		currentCell = cell;
		currentPortalGraph = portalGraph;
	}

	const auto jobArraySize = jobArrays.size();

	if (replacementJobArrays.size() != jobArraySize)
		replacementJobArrays.resize(jobArraySize);

	for (auto& jobArray : replacementJobArrays)
		jobArray.clear();

	addedSet.clear();

	// Copy the original job arrays contents into the replacement job arrays
	uint32_t count = 0;
	for (uint32_t i = 0; i < jobArraySize; ++i) {
		for (const auto& object : jobArrays[i]) {
			replacementJobArrays[i].push_back(object);
			addedSet.insert(object.get());
			count++;
		}
	}

	const auto playerPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	auto lightDir = -dirLight->GetShadowDirectionalLightRuntimeData().sunVector;
	lightDir.Unitize();

	// Add extra rooms and portals that are in the direction of the sun
	for (const auto& object : currentCellRoomsAndPortals) {
		if (addedSet.find(object.get()) != addedSet.end() || !IsInSunDirectionAndWithinShadowDistance(object, lightDir, playerPos))
			continue;

		addedSet.insert(object.get());
		replacementJobArrays[count++ % jobArraySize].push_back(object);
	}

	arraysCleared = false;
}

void InteriorSun::InitialiseOnNewCell(const RE::NiPointer<RE::BSPortalGraph>& portalGraph)
{
	currentCellRoomsAndPortals.clear();

	if (const auto portalSharedNode = portalGraph->portalSharedNode) {
		for (const auto& room : portalGraph->rooms)
			currentCellRoomsAndPortals.push_back(room);

		for (auto child : portalGraph->portalSharedNode->GetChildren())
			currentCellRoomsAndPortals.push_back(child);
	}
}

bool InteriorSun::IsInSunDirectionAndWithinShadowDistance(const RE::NiPointer<RE::NiAVObject>& object, const RE::NiPoint3& lightDir, const RE::NiPoint3& playerPos) const
{
	const float radius = object->worldBound.radius;
	const auto diff = object->worldBound.center - playerPos;
	const float distance = diff.Length();
	const float projection = lightDir.Dot(diff);
	return projection >= -radius && (distance - radius) <= *gShadowDistance;
}

void InteriorSun::SetShadowDistance(bool inInterior)
{
	RE::BSShaderManager::UpdateShadowDistanceAndInteriorFlag(inInterior);
}
#undef I18N_KEY_PREFIX

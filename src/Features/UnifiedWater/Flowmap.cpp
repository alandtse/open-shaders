#include "Flowmap.h"
#include "FlowmapLayout.h"

#include "Utils/D3D.h"
#include "Utils/StringUtils.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <array>
#include <charconv>
#include <sstream>

bool Flowmap::IsValid() const
{
	return width > 0 && height > 0 && flowmapTex && flowmapTex->rendererTexture &&
	       flowmapTex->rendererTexture->texture && flowmapTex->rendererTexture->resourceView;
}

bool Flowmap::TryGetFlowmap(RE::NiPointer<RE::NiSourceTexture>& outFlowmapTex) const
{
	if (!IsValid())
		return false;

	outFlowmapTex = this->flowmapTex;
	return true;
}

bool Flowmap::LoadOrGenerateFlowmap(bool useMips)
{
	if (!LoadFlowmap()) {
		logger::info("[Unified Water] [Flowmap] Could not load flowmap - regenerating...");
		return RegenerateAndLoadFlowmap(useMips);
	}

	return true;
}

bool Flowmap::RegenerateAndLoadFlowmap(bool useMips)
{
	std::filesystem::path generatedPath;
	if (!GenerateFlowmap(useMips, generatedPath)) {
		logger::error("[Unified Water] [Flowmap] Failed to generate flowmap");
		return false;
	}

	if (!LoadFlowmap(generatedPath)) {
		logger::error("[Unified Water] [Flowmap] Failed to load generated flowmap '{}'; retaining the previous map", generatedPath.string());
		return false;
	}

	std::vector<std::filesystem::path> cachedPaths;
	if (FindFlowmaps(cachedPaths)) {
		const auto generatedName = Util::ToLowerAscii(generatedPath.filename().string());
		for (const auto& path : cachedPaths) {
			if (Util::ToLowerAscii(path.filename().string()) == generatedName)
				continue;
			std::error_code ec;
			std::filesystem::remove(path, ec);
			if (ec)
				logger::warn("[Unified Water] [Flowmap] Cannot remove obsolete cache '{}': {}", path.string(), ec.message());
		}
	}
	logger::debug("[Unified Water] [Flowmap] Flowmap regenerated and loaded");
	return true;
}

bool Flowmap::FindFlowmaps(std::vector<std::filesystem::path>& paths)
{
	namespace fs = std::filesystem;
	const fs::path dir = Util::PathHelpers::GetDataPath() / "textures" / "water" / "flowmaps";
	std::error_code ec;
	fs::directory_iterator entry(dir, ec);
	const fs::directory_iterator end;
	for (; !ec && entry != end; entry.increment(ec)) {
		const auto name = Util::ToLowerAscii(entry->path().filename().string());
		if (!name.starts_with("tamriel-flowmap.") || !name.ends_with(".dds"))
			continue;
		if (entry->is_regular_file(ec))
			paths.push_back(entry->path());
		if (ec)
			break;
	}
	if (ec) {
		logger::warn("[Unified Water] [Flowmap] Cannot enumerate '{}': {}", dir.string(), ec.message());
		return false;
	}
	return true;
}

bool Flowmap::LoadFlowmap()
{
	std::vector<std::filesystem::path> candidates;
	if (!FindFlowmaps(candidates))
		return false;
	// Timestamps cannot establish which retained cache matches the current data.
	if (candidates.size() != 1) {
		logger::warn("[Unified Water] [Flowmap] Expected one cached flowmap, found {}; regeneration required", candidates.size());
		return false;
	}
	return LoadFlowmap(candidates.front());
}

bool Flowmap::LoadFlowmap(const std::filesystem::path& file)
{
	std::vector<std::string> tokens;
	const auto stem = file.filename().stem().string();
	std::istringstream iss(stem);
	std::string token;
	while (std::getline(iss, token, '.')) {
		tokens.push_back(token);
	}

	if ((tokens.size() != 5 && tokens.size() != 6) || stem.ends_with('.') || Util::ToLowerAscii(tokens[0]) != "tamriel-flowmap" ||
		!Util::IEquals(file.extension().string(), ".dds")) {
		logger::error("[Unified Water] [Flowmap] Invalid file name '{}'", file.string());
		return false;
	}

	if (tokens.size() == 6) {
		uint64_t generation;
		const auto& value = tokens.back();
		const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), generation, 16);
		if (ec != std::errc{} || ptr != value.data() + value.size()) {
			logger::error("[Unified Water] [Flowmap] Invalid generation ID in '{}'", file.string());
			return false;
		}
	}

	std::array<int32_t, 4> dimensions;
	for (size_t i = 0; i < dimensions.size(); ++i) {
		const auto& value = tokens[i + 1];
		const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), dimensions[i]);
		if (ec != std::errc{} || ptr != value.data() + value.size()) {
			logger::error("[Unified Water] [Flowmap] Invalid dimensions in '{}'", file.string());
			return false;
		}
	}
	static_assert(WaterFlowmapLayout::MaxCells == D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / WaterFlowmapLayout::TexelsPerCell);
	if (!WaterFlowmapLayout::ValidCells(dimensions[0], dimensions[1])) {
		logger::error("[Unified Water] [Flowmap] Unsupported dimensions in '{}'", file.string());
		return false;
	}

	auto path = std::format(R"(textures\water\flowmaps\{})", file.filename().string());
	RE::NiPointer<RE::NiTexture> tex;
	RE::BSShaderManager::GetTexture(path.c_str(), true, tex, false);
	if (!tex || tex->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get()) {
		logger::error("[Unified Water] [Flowmap] Failed to load flowmap from {}", path);
		return false;
	}

	const auto sourceTex = static_cast<RE::NiSourceTexture*>(tex.get());
	if (!sourceTex->rendererTexture || !sourceTex->rendererTexture->texture || !sourceTex->rendererTexture->resourceView) {
		logger::error("[Unified Water] [Flowmap] Flowmap texture or view invalid: {}", path);
		return false;
	}

	D3D11_RESOURCE_DIMENSION resourceDimension;
	Util::AsReal(sourceTex->rendererTexture->texture)->GetType(&resourceDimension);
	if (resourceDimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		logger::error("[Unified Water] [Flowmap] Resource is not a 2D texture: {}", path);
		return false;
	}
	D3D11_TEXTURE2D_DESC desc;
	static_cast<ID3D11Texture2D*>(Util::AsReal(sourceTex->rendererTexture->texture))->GetDesc(&desc);
	// Texture quality may strip top mips; normalized UVs still cover the same cells.
	const bool sizeMatches = WaterFlowmapLayout::MatchesTexture(desc.Width, desc.Height, dimensions[0], dimensions[1]);
	if (!sizeMatches || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
		logger::error("[Unified Water] [Flowmap] Texture size/type does not match cell dimensions: {} ({}x{}, cells {}x{})", path, desc.Width, desc.Height, dimensions[0], dimensions[1]);
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
	Util::AsReal(sourceTex->rendererTexture->resourceView)->GetDesc(&viewDesc);
	if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
		logger::error("[Unified Water] [Flowmap] Texture view must expose a 2D map: {}", path);
		return false;
	}

	flowmapTex = RE::NiPointer(sourceTex);
	width = dimensions[0];
	height = dimensions[1];
	offsetX = dimensions[2];
	offsetY = dimensions[3];
	invWidth = 1.0f / static_cast<float>(width);
	invHeight = 1.0f / static_cast<float>(height);

	logger::debug("[Unified Water] [Flowmap] Flowmap loaded from {}", file.string());
	return true;
}

bool Flowmap::GenerateFlowmap(bool useMips, std::filesystem::path& generatedPath)
{
	const auto t0 = std::chrono::steady_clock::now();

	auto dvc = globals::d3d::device;
	auto ctx = globals::d3d::context;
	if (!dvc || !ctx) {
		logger::error("[Unified Water] [Flowmap] D3D device/context not available");
		return false;
	}

	winrt::com_ptr<ID3D11DeviceContext> deferredCtx;
	if (FAILED(dvc->CreateDeferredContext(0, deferredCtx.put()))) {
		logger::error("[Unified Water] [Flowmap] Failed to create deferred context");
		return false;
	}

	Util::MultithreadScope guard(ctx);
	if (!guard) {
		logger::error("[Unified Water] [Flowmap] ID3D11Multithread not available");
		return false;
	}

	const auto tamriel = RE::TESForm::LookupByEditorID<RE::TESWorldSpace>("Tamriel");
	if (!tamriel) {
		logger::error("[Unified Water] [Flowmap] Failed to load Tamriel WorldSpace");
		return false;
	}

	int32_t worldMinX, worldMinY, worldMaxX, worldMaxY;
	Util::WorldToCell(tamriel->minimumCoords, worldMinX, worldMinY);
	Util::WorldToCell(tamriel->maximumCoords, worldMaxX, worldMaxY);
	worldMaxX -= 1;
	worldMaxY -= 1;

	struct FlowCell
	{
		int32_t x;
		int32_t y;
		winrt::com_ptr<ID3D11Texture2D> tex;
	};

	int32_t mapMinX = INT_MAX;
	int32_t mapMinY = INT_MAX;
	int32_t mapMaxX = INT_MIN;
	int32_t mapMaxY = INT_MIN;

	auto cells = std::vector<FlowCell>();
	cells.reserve(1024);

	{
		for (auto y = worldMinY; y < worldMaxY; ++y) {
			for (auto x = worldMinX; x < worldMaxX; ++x) {
				auto path = std::format(R"(Textures\Water\skyrim.esm\flow.{}.{}.dds)", x, y);
				auto stream = RE::BSResourceNiBinaryStream(path);

				if (!stream.good())
					continue;

				const auto size = stream.stream->totalSize;
				std::vector<uint8_t> buffer(size);
				stream.read(buffer.data(), size);

				DirectX::TexMetadata meta{};
				DirectX::ScratchImage src;
				auto hr = DirectX::LoadFromDDSMemory(buffer.data(), size, DirectX::DDS_FLAGS_NONE, &meta, src);
				if (FAILED(hr)) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to load", x, y);
					continue;
				}

				DirectX::ScratchImage conv;
				if (DirectX::IsCompressed(meta.format)) {
					hr = DirectX::Decompress(src.GetImages(), src.GetImageCount(), src.GetMetadata(), DXGI_FORMAT_B8G8R8A8_UNORM, conv);
					if (FAILED(hr)) {
						logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to decompress", x, y);
						continue;
					}
				} else if (meta.format != DXGI_FORMAT_B8G8R8A8_UNORM) {
					hr = DirectX::Convert(src.GetImages(), src.GetImageCount(), src.GetMetadata(), DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, conv);
					if (FAILED(hr)) {
						logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to convert to the correct format", x, y);
						continue;
					}
				} else {
					conv = std::move(src);
				}

				winrt::com_ptr<ID3D11Resource> res;
				hr = DirectX::CreateTexture(dvc, conv.GetImages(), conv.GetImageCount(), conv.GetMetadata(), res.put());
				if (FAILED(hr) || !res) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} creation failed", x, y);
					continue;
				}

				winrt::com_ptr<ID3D11Texture2D> tex;
				hr = res->QueryInterface(IID_PPV_ARGS(tex.put()));
				if (FAILED(hr)) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} is not a Texture2D", x, y);
					continue;
				}

				D3D11_TEXTURE2D_DESC d{};
				tex->GetDesc(&d);
				if (d.Width != WaterFlowmapLayout::TexelsPerCell || d.Height != WaterFlowmapLayout::TexelsPerCell || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.MipLevels < WaterFlowmapLayout::MipCount) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} is invalid", x, y);
					continue;
				}

				mapMinX = std::min(mapMinX, x);
				mapMinY = std::min(mapMinY, y);
				mapMaxX = std::max(mapMaxX, x);
				mapMaxY = std::max(mapMaxY, y);

				cells.emplace_back(FlowCell{ x, y, tex });
			}
		}
	}

	if (cells.empty()) {
		logger::error("[Unified Water] [Flowmap] No source flow textures found");
		return false;
	}

	const auto width = mapMaxX - mapMinX + 1;
	const auto height = mapMaxY - mapMinY + 1;
	const auto offsetX = -mapMinX;
	const auto offsetY = -mapMinY;

	logger::debug("[Unified Water] [Flowmap] Loaded {} flow textures, creating a {}x{} flow map...", cells.size(), width, height);

	static_assert(WaterFlowmapLayout::MaxCells == D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / WaterFlowmapLayout::TexelsPerCell);
	if (!WaterFlowmapLayout::ValidCells(width, height)) {
		logger::error("[Unified Water] [Flowmap] Generated map exceeds supported dimensions");
		return false;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width * WaterFlowmapLayout::TexelsPerCell;
	desc.Height = height * WaterFlowmapLayout::TexelsPerCell;
	desc.MipLevels = useMips ? WaterFlowmapLayout::MipCount : 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> flowmap;
	if (FAILED(dvc->CreateTexture2D(&desc, nullptr, flowmap.put()))) {
		logger::error("[Unified Water] [Flowmap] Failed to create texture");
		return false;
	}
	Util::SetResourceName(flowmap.get(), "UnifiedWater::Flowmap");

	for (const auto& [x, y, flowTex] : cells) {
		D3D11_TEXTURE2D_DESC srcDesc{};
		flowTex->GetDesc(&srcDesc);

		const UINT sx = static_cast<UINT>(x + offsetX);
		const UINT sy = static_cast<UINT>(y + offsetY);
		const UINT dstX0 = sx * WaterFlowmapLayout::TexelsPerCell;

		const UINT maxMipLevel = useMips ? WaterFlowmapLayout::MipCount : 1u;
		for (UINT mipLevel = 0; mipLevel < maxMipLevel; ++mipLevel) {
			const UINT srcSub = D3D11CalcSubresource(mipLevel, 0, srcDesc.MipLevels);
			const UINT dstSub = D3D11CalcSubresource(mipLevel, 0, desc.MipLevels);
			const UINT tileSize = std::max(1u, WaterFlowmapLayout::TexelsPerCell >> mipLevel);
			const UINT flowmapHeight = std::max(1u, desc.Height >> mipLevel);
			const UINT dstX = dstX0 >> mipLevel;
			const UINT dstY = flowmapHeight - (sy + 1) * tileSize;

			deferredCtx->CopySubresourceRegion(flowmap.get(), dstSub, dstX, dstY, 0, flowTex.get(), srcSub, nullptr);
		}
	}

	winrt::com_ptr<ID3D11CommandList> commandList;
	if (deferredCtx && FAILED(deferredCtx->FinishCommandList(FALSE, commandList.put()))) {
		logger::error("[Unified Water] [Flowmap] FinishCommandList failed");
		return false;
	}

	{
		ctx->ExecuteCommandList(commandList.get(), TRUE);

		const auto generation = static_cast<uint64_t>(t0.time_since_epoch().count());
		const auto filename = std::format(L"Tamriel-Flowmap.{}.{}.{}.{}.{:016X}.dds", width, height, offsetX, offsetY, generation);
		const auto path = Util::PathHelpers::GetDataPath() / "textures" / "water" / "flowmaps" / filename;
		const auto hr = Util::SaveTextureToFile(dvc, ctx, path, flowmap.get());

		if (FAILED(hr)) {
			logger::error("[Unified Water] [Flowmap] Failed to save flowmap to {}: hr={:08X}", path.string().c_str(), static_cast<uint32_t>(hr));
			return false;
		}
		generatedPath = path;
	}

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	logger::info("[Unified Water] [Flowmap] Generated in {} ms", ms);

	return true;
}

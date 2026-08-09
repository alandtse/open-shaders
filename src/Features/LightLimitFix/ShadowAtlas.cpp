// ShadowAtlas.cpp
// Tier 2 shadow storage: one SCM-owned depth atlas holding variable-size tiles instead of a kSHADOWMAPS slice per light.

#include <d3d11_1.h>

#include <DirectXTex.h>

#include <filesystem>
#include <fstream>

#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Util.h"
#include "AtlasAllocator.h"
#include "ShadowCasterInternal.h"
#include "ShadowCasterMath.h"

namespace ShadowCasterManager
{
	namespace
	{
		struct SlotTile
		{
			AtlasAllocator::Tile tile;     ///< SAMPLED rect (content lives here)
			AtlasAllocator::Tile pending;  ///< promotion target; rasters land here, swapped in once rendered
			uint32_t escalateFrame = 0;    ///< frame a short-changed slot may retry escalation
			uint32_t renderFrame = 0;      ///< frame stamp of the last raster into the tile
			bool valid = false;            ///< content rendered at least once
			uint64_t staticHash = 0;       ///< static-caster hash baked into the static tile
			bool staticValid = false;      ///< static tile holds baked content
			bool staticEmpty = false;      ///< baked tile captured zero casters
			const void* owner = nullptr;   ///< light whose depths the content holds
			uint32_t orphanSince = 0;      ///< frame the owning light left the slot (0 = occupied)
			ShadowBakeSnapshot bake{};     ///< radius/bias the tile depth was rastered with
			bool bakeValid = false;
			bool bakePending = false;  ///< content landed; renderer must refresh the snapshot
		};

		std::atomic<uint32_t> s_clearsSwallowed{ 0 };
		std::atomic<uint32_t> s_clearsPassed{ 0 };
		std::atomic<uint32_t> s_tileClears{ 0 };
		std::atomic<uint32_t> s_tileReallocs{ 0 };
		std::atomic<uint32_t> s_ownerInvalidations{ 0 };
		std::atomic<uint32_t> s_allocDenied{ 0 };

		constexpr uint32_t kAtlasEscalateRetryFrames = 30;

		struct AtlasState
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;
			winrt::com_ptr<ID3D11DepthStencilView> dsvReadOnly;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11DeviceContext1> context1;
			// Parallel static-cache atlas: same dims/format/tile layout, holds
			// pose-stable ("static") shadow depth that the dynamic pass copies in.
			winrt::com_ptr<ID3D11Texture2D> staticTexture;
			winrt::com_ptr<ID3D11DepthStencilView> staticDsv;
			bool staticReady = false;
			bool staticFailed = false;
			DXGI_FORMAT texFormat = DXGI_FORMAT_UNKNOWN;  ///< chosen typeless depth format
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;  ///< chosen DSV format
			AtlasAllocator allocator;
			std::vector<SlotTile> slots;
			uint32_t dim = 0;
			uint32_t baseTile = 0;       ///< full-class tile size (engine slice res)
			uint32_t cell = 0;           ///< allocator cell size (quarter class)
			uint32_t levels = 0;         ///< buddy levels the allocator was built with
			uint32_t bytesPerPixel = 0;  ///< from the depth format (2 or 4)
			bool ready = false;
			bool failed = false;
		};
		AtlasState s_atlas;

		// Clamp a requested atlas resolution to the legal range and snap it
		// down to a buddy-aligned size for the given cell geometry.
		uint32_t SnapAtlasDim(uint32_t requested, uint32_t baseTile, uint32_t cell, uint32_t& levelsOut)
		{
			const uint32_t clamped = std::clamp(requested, baseTile, kAtlasMaxResolution);
			uint32_t levels = 0;
			while ((cell << (levels + 1)) <= clamped && levels < AtlasAllocator::kMaxLevels)
				levels++;
			levelsOut = levels;
			return cell << levels;
		}

		// Depth formats pair as (typeless resource, DSV format, SRV format).
		bool DepthFormats(DXGI_FORMAT sliceFormat, DXGI_FORMAT& tex, DXGI_FORMAT& dsv, DXGI_FORMAT& srv)
		{
			switch (sliceFormat) {
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				tex = DXGI_FORMAT_R16_TYPELESS;
				dsv = DXGI_FORMAT_D16_UNORM;
				srv = DXGI_FORMAT_R16_UNORM;
				return true;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				tex = DXGI_FORMAT_R32_TYPELESS;
				dsv = DXGI_FORMAT_D32_FLOAT;
				srv = DXGI_FORMAT_R32_FLOAT;
				return true;
			default:
				return false;  // stencil-packed slices are not atlas-tileable
			}
		}

		// The engine full-surface-clears its depth target before each shadow
		// light; on the shared atlas DSV that wipes every other light's tile
		// (only the last-rendered light keeps shadow). Do not remove: tiles
		// are rect-cleared per redraw, and non-atlas views pass through.
		//
		// Detours the prologue (stl::detour_vfunc) rather than overwriting the
		// vtable slot in place: a raw overwrite corrupts RenderDoc's dispatch
		// bookkeeping on this COM object and deadlocks the render thread once attached.
		struct Hook_ClearDepthStencilView
		{
			static void thunk(ID3D11DeviceContext* self, ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil)
			{
				if (view && (view == s_atlas.dsv.get() || view == s_atlas.dsvReadOnly.get() ||
								view == s_atlas.staticDsv.get())) {
					s_clearsSwallowed.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				s_clearsPassed.fetch_add(1, std::memory_order_relaxed);
				func(self, view, flags, depth, stencil);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallClearHook(ID3D11DeviceContext* context)
		{
			static bool installed = false;
			if (installed)
				return;
			installed = true;
			stl::detour_vfunc<53, Hook_ClearDepthStencilView>(context);
		}

		// Functional ClearView probe: rect-clear a tiny depth texture and read
		// it back. Some runtimes accept the call but ignore depth views; only
		// the readback proves the path works.
		bool ProbeClearViewDepth(ID3D11Device* device, ID3D11DeviceContext1* context1)
		{
			constexpr uint32_t kProbeDim = 16;
			winrt::com_ptr<ID3D11Texture2D> tex, staging;
			winrt::com_ptr<ID3D11DepthStencilView> dsv;

			DXGI_FORMAT texFmt, dsvFmt, srvFmt;
			DepthFormats(DXGI_FORMAT_R16_TYPELESS, texFmt, dsvFmt, srvFmt);

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = kProbeDim;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = texFmt;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put())))
				return false;
			Util::SetResourceName(tex.get(), "SCM::AtlasProbe");
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = dsvFmt;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateDepthStencilView(tex.get(), &dsvDesc, dsv.put())))
				return false;
			Util::SetResourceName(dsv.get(), "SCM::AtlasProbe DSV");

			context1->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
			const FLOAT one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			D3D11_RECT rect{ kProbeDim / 2, kProbeDim / 2, kProbeDim, kProbeDim };
			context1->ClearView(dsv.get(), one, &rect, 1);

			desc.BindFlags = 0;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())))
				return false;
			Util::SetResourceName(staging.get(), "SCM::AtlasProbe Staging");
			context1->CopyResource(staging.get(), tex.get());

			// Bounded poll, not a blocking Map: a fence that never signals (seen
			// on VR the first time a shadow tile is needed mid-session) must
			// degrade to a failed probe, not hang the render thread forever.
			D3D11_MAPPED_SUBRESOURCE mapped{};
			HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
			for (int i = 0; i < 1000 && hr == DXGI_ERROR_WAS_STILL_DRAWING; i++) {
				hr = context1->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (FAILED(hr))
				return false;
			auto row = [&](uint32_t y) { return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch); };
			const bool outside = row(2)[2] == 0;
			const bool inside = row(kProbeDim - 4)[kProbeDim - 4] == 0xFFFF;
			context1->Unmap(staging.get(), 0);
			return outside && inside;
		}

		bool EnsureResources()
		{
			if (s_atlas.ready)
				return true;
			if (s_atlas.failed)
				return false;

			auto* device = globals::d3d::device;
			auto* context = globals::d3d::context;
			if (!device || !context)
				return false;

			// Follow the engine's slice format and resolution so atlas tiles
			// keep the exact precision the array slices had.
			const auto info = GetVRAMInfo();
			if (!info.valid || info.shadowWidth == 0) {
				return false;  // kSHADOWMAPS not readable yet; retry next frame
			}

			auto fail = [&](const char* why) {
				s_atlas.failed = true;
				logger::warn("[SCM] Shadow atlas disabled: {}", why);
				return false;
			};

			if (FAILED(context->QueryInterface(s_atlas.context1.put())))
				return fail("D3D11.1 context unavailable (ClearView needed for tile clears)");

			D3D11_TEXTURE2D_DESC sliceDesc{};
			DXGI_FORMAT texFmt, dsvFmt, srvFmt;
			{
				// Format from the live texture when possible; D16 fallback
				// matches the INI-fallback path in GetVRAMInfo.
				DXGI_FORMAT sliceFmt = DXGI_FORMAT_R16_TYPELESS;
				if (TryReadShadowTextureDesc(sliceDesc))
					sliceFmt = sliceDesc.Format;
				if (!DepthFormats(sliceFmt, texFmt, dsvFmt, srvFmt))
					return fail("unsupported kSHADOWMAPS format for tiling");
			}

			if (!ProbeClearViewDepth(device, s_atlas.context1.get()))
				return fail("ClearView depth-rect probe failed on this driver");

			s_atlas.texFormat = texFmt;
			s_atlas.dsvFormat = dsvFmt;
			s_atlas.baseTile = info.shadowWidth;
			s_atlas.cell = std::max(1u, static_cast<uint32_t>(s_atlas.baseTile * kTileScaleFloor));
			uint32_t levels = 0;
			s_atlas.dim = SnapAtlasDim(s_settings.AtlasResolution, s_atlas.baseTile, s_atlas.cell, levels);
			s_atlas.levels = levels;
			s_atlas.bytesPerPixel = (texFmt == DXGI_FORMAT_R32_TYPELESS) ? 4u : 2u;
			s_atlas.allocator.Reset(levels);

			// Raw creation instead of the Buffer.h Texture2D wrapper: the
			// wrapper throws on failure (this path must fail-latch gracefully)
			// and has no read-only DSV slot.
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = s_atlas.dim;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = texFmt;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, s_atlas.texture.put())))
				return fail("atlas texture creation failed");
			Util::SetResourceName(s_atlas.texture.get(), "SCM::ShadowAtlas");

			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = dsvFmt;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateDepthStencilView(s_atlas.texture.get(), &dsvDesc, s_atlas.dsv.put())))
				return fail("atlas DSV creation failed");
			Util::SetResourceName(s_atlas.dsv.get(), "SCM::ShadowAtlas DSV");
			dsvDesc.Flags = D3D11_DSV_READ_ONLY_DEPTH;
			if (FAILED(device->CreateDepthStencilView(s_atlas.texture.get(), &dsvDesc, s_atlas.dsvReadOnly.put())))
				return fail("atlas read-only DSV creation failed");
			Util::SetResourceName(s_atlas.dsvReadOnly.get(), "SCM::ShadowAtlas DSV (read-only)");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = srvFmt;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(s_atlas.texture.get(), &srvDesc, s_atlas.srv.put())))
				return fail("atlas SRV creation failed");
			Util::SetResourceName(s_atlas.srv.get(), "SCM::ShadowAtlas SRV");

			InstallClearHook(context);

			// Whole-atlas clear once at creation so never-rendered regions read
			// as far depth (fully lit) rather than driver garbage. ClearView,
			// not ClearDepthStencilView: the hook above swallows the latter
			// for atlas views.
			const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			D3D11_RECT all{ 0, 0, static_cast<LONG>(s_atlas.dim), static_cast<LONG>(s_atlas.dim) };
			s_atlas.context1->ClearView(s_atlas.dsv.get(), farDepth, &all, 1);

			s_atlas.slots.assign(static_cast<size_t>(std::max(s_lights.Size, 1)), {});
			const uint32_t capacity = s_atlas.allocator.CellsPerAxis() * s_atlas.allocator.CellsPerAxis();
			if (capacity < static_cast<uint32_t>(s_lights.Size))
				logger::warn(
					"[SCM] Shadow atlas holds {} quarter tiles for {} pool slots; "
					"lowest-importance lights will get no tile (raise AtlasResolution "
					"or lower ShadowLightCount)",
					capacity, s_lights.Size);
			s_atlas.ready = true;
			logger::info("[SCM] Shadow atlas ready: {0}x{0}, base tile {1}, cell {2}", s_atlas.dim, s_atlas.baseTile, s_atlas.cell);
			return true;
		}

		// Lazily create the parallel static-cache atlas: a second depth texture
		// of identical dimensions/format so a slot's tile rect maps 1:1 into it.
		// Returns false (and latches off) if creation fails -- the caller then
		// runs the live atlas alone, unsplit.
		bool EnsureStaticResources()
		{
			if (s_atlas.staticReady)
				return true;
			if (s_atlas.staticFailed || !s_atlas.ready)
				return false;
			auto* device = globals::d3d::device;
			if (!device)
				return false;

			auto fail = [&](const char* why) {
				s_atlas.staticFailed = true;
				logger::warn("[SCM] Static shadow cache disabled: {}", why);
				return false;
			};

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = s_atlas.dim;
			desc.MipLevels = desc.ArraySize = 1;
			desc.Format = s_atlas.texFormat;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, s_atlas.staticTexture.put())))
				return fail("static atlas texture creation failed");
			Util::SetResourceName(s_atlas.staticTexture.get(), "SCM::StaticShadowAtlas");

			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = s_atlas.dsvFormat;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			if (FAILED(device->CreateDepthStencilView(s_atlas.staticTexture.get(), &dsvDesc, s_atlas.staticDsv.put())))
				return fail("static atlas DSV creation failed");
			Util::SetResourceName(s_atlas.staticDsv.get(), "SCM::StaticShadowAtlas DSV");

			// Far-depth clear once so never-baked regions read as unshadowed.
			const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
			D3D11_RECT all{ 0, 0, static_cast<LONG>(s_atlas.dim), static_cast<LONG>(s_atlas.dim) };
			s_atlas.context1->ClearView(s_atlas.staticDsv.get(), farDepth, &all, 1);

			s_atlas.staticReady = true;
			logger::info("[SCM] Static shadow cache ready: {0}x{0}", s_atlas.dim);
			return true;
		}

		uint32_t OrderForScale(float scale)
		{
			// Order 0 = the floor class (one allocator cell); each order
			// doubles the tile per axis up to the full class.
			uint32_t order = 0;
			for (float s = kTileScaleFloor; s < scale && s < kTileScaleFull; s *= 2.0f)
				order++;
			return order;
		}
	}

	bool AtlasActive()
	{
		// Cheap flag check only: this runs inside draw-time hooks, where
		// resource creation (and the probe's GPU-sync readback) must never
		// happen mid-pass. UpdateAtlas owns creation at the frame boundary.
		return s_bootAtlasEnabled && s_atlas.ready;
	}

	namespace
	{
		std::atomic<bool> s_dumpRequested{ false };

		// One-shot debug dump: atlas depth DDS + a slot manifest, written on
		// the render thread (the only safe owner of the immediate context).
		// The CaptureTexture readback stalls the GPU; acceptable for an
		// explicitly requested diagnostic.
		void ServiceAtlasDump()
		{
			if (!s_dumpRequested.exchange(false, std::memory_order_acq_rel))
				return;
			if (!s_atlas.ready)
				return;
			auto* device = globals::d3d::device;
			auto* context = globals::d3d::context;
			if (!device || !context)
				return;

			const auto stamp = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
			std::filesystem::path dir = "Data\\SKSE\\Plugins\\CommunityShaders\\Captures";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);

			DirectX::ScratchImage image;
			if (FAILED(DirectX::CaptureTexture(device, context, s_atlas.texture.get(), image))) {
				logger::warn("[SCM] Atlas dump: CaptureTexture failed");
				return;
			}
			const auto ddsPath = dir / std::format("shadow_atlas_frame{}.dds", stamp);
			if (FAILED(DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsPath.c_str()))) {
				logger::warn("[SCM] Atlas dump: SaveToDDSFile failed");
				return;
			}

			// Manifest: per-slot tile rects so the DDS regions map back to
			// pool slots without cross-referencing the live dump.
			std::ofstream manifest(dir / std::format("shadow_atlas_frame{}.json", stamp));
			manifest << "{\n  \"dim\": " << s_atlas.dim
					 << ",\n  \"frame\": " << stamp
					 << ",\n  \"clearsSwallowed\": " << s_clearsSwallowed.load(std::memory_order_relaxed)
					 << ",\n  \"clearsPassed\": " << s_clearsPassed.load(std::memory_order_relaxed)
					 << ",\n  \"tileClears\": " << s_tileClears.load(std::memory_order_relaxed)
					 << ",\n  \"slots\": [\n";
			bool first = true;
			for (size_t i = 0; i < s_atlas.slots.size(); i++) {
				const auto& slot = s_atlas.slots[i];
				if (!slot.tile.valid)
					continue;
				if (!first)
					manifest << ",\n";
				first = false;
				manifest << "    { \"slot\": " << i
						 << ", \"x\": " << slot.tile.x * s_atlas.cell
						 << ", \"y\": " << slot.tile.y * s_atlas.cell
						 << ", \"size\": " << (1u << slot.tile.order) * s_atlas.cell
						 << ", \"lastRenderFrame\": " << slot.renderFrame
						 << ", \"contentValid\": " << (slot.valid ? "true" : "false") << " }";
			}
			manifest << "\n  ]\n}\n";
			logger::info("[SCM] Atlas dumped: {}", ddsPath.string());
		}
	}

	void RequestAtlasDump()
	{
		s_dumpRequested.store(true, std::memory_order_release);
	}

	void UpdateAtlas()
	{
		if (!s_bootAtlasEnabled || s_atlas.failed)
			return;
		ServiceAtlasDump();
		if (!EnsureResources())
			return;
		// Bring up the parallel static cache; a failure latches it off and
		// the live atlas runs unsplit.
		EnsureStaticResources();
		// Track pool reallocation (runtime ShadowLightCount changes): slots
		// beyond the vector would silently render tile-less otherwise.
		const size_t poolSize = static_cast<size_t>(std::max(s_lights.Size, 1));
		if (s_atlas.slots.size() != poolSize) {
			// FreeSlotTile releases the staged promotion too; freeing only the
			// live rect leaked the pending cells on pool shrink.
			for (size_t i = poolSize; i < s_atlas.slots.size(); i++)
				FreeSlotTile(static_cast<int32_t>(i));
			s_atlas.slots.resize(poolSize);
		}

		// Reconcile per frame. Orphans (light left by any removal path) free
		// immediately -- accumulated they starve new lights of tiles. Demoted
		// tiles (class above the rank budget) are NOT freed here: eager (or
		// occupancy-gated -- dense interiors sit above any threshold
		// permanently) down-frees realloc-churned on every budget oscillation.
		// EnsureSlotTile reclaims over-class tiles only when an allocation
		// actually fails.
		for (size_t i = 0; i < s_atlas.slots.size(); i++) {
			auto& slot = s_atlas.slots[i];
			const auto* entry = i < static_cast<size_t>(s_lights.Size) ? &s_lights.Lights[i] : nullptr;
			if (!entry || !entry->Light) {
				if (!slot.tile.valid)
					continue;
				// Orphan grace: a gate-flapped light usually returns within a
				// few frames, and re-entry reclaims this slot with its content
				// intact (array parity). Free only when the owner stays gone;
				// allocation-failure reclaim can still take it earlier.
				const uint32_t now = globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
				if (slot.orphanSince == 0 || now < slot.orphanSince)
					slot.orphanSince = now ? now : 1u;  // (re)anchor; guards a load-time frame reset
				const uint32_t age = now - slot.orphanSince;
				// Owner-reclaim serves the gate flap, which returns within a few
				// frames. Past that, drop owner + content: a light gone this long is
				// likely a real departure, and a lingering raw NiLight* risks a
				// freed-then-reused address rematching a different light onto stale depths.
				if (age > 8u && slot.owner) {
					slot.owner = nullptr;
					slot.valid = false;
					slot.staticValid = false;
					slot.bakeValid = false;
				}
				if (age > 60u)
					FreeSlotTile(static_cast<int32_t>(i));
				continue;
			}
			// The tile's depths (and static bake) belong to the light that
			// rasterized them: a slot handed to a different light must not
			// advertise the previous owner's shadow as valid content. Key on
			// the NiLight, not the BSShadowLight wrapper -- promotion recreates
			// the wrapper for the same engine light, and keying on it blinked
			// shadows off on every promotion churn.
			const void* stableOwner = entry->Light->light.get();
			// A pool reshuffle moves a light between indices without moving its
			// tile; migrate its surviving content (active index or a parked
			// orphan, see ParkOrphanSlotRecord) instead of blanking it. Guarded
			// by slot count so this always terminates.
			for (size_t guard = 0; slot.owner != stableOwner && guard < s_atlas.slots.size(); guard++) {
				int32_t swapWith = -1;
				for (size_t j = 0; j < s_atlas.slots.size() && swapWith < 0; j++) {
					const auto& other = s_atlas.slots[j];
					if (j != i && other.owner == stableOwner && other.tile.valid && other.valid)
						swapWith = static_cast<int32_t>(j);
				}
				if (swapWith < 0)
					break;
				std::swap(slot, s_atlas.slots[static_cast<size_t>(swapWith)]);
			}
			slot.orphanSince = 0;
			if (!slot.tile.valid)
				continue;  // no surviving content anywhere; EnsureSlotTile starts fresh
			if (slot.owner != stableOwner) {
				const bool firstClaim = slot.owner == nullptr;
				slot.owner = stableOwner;
				if (!firstClaim) {
					// No owner-matching content survives to migrate: a slot handed to
					// a different light must not advertise the previous owner's shadow.
					slot.valid = false;
					slot.staticValid = false;
					slot.bakeValid = false;
					// Reschedule immediately: invalidation alone leaves the slot
					// dark for its whole redraw interval; the
					// array path heals by re-rendering, so must this one.
					s_lights.Lights[i].LastDrawnFrame = -1;
					s_ownerInvalidations.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
	}

	ID3D11DepthStencilView* AtlasDSV(bool readOnly)
	{
		return readOnly ? s_atlas.dsvReadOnly.get() : s_atlas.dsv.get();
	}

	ID3D11ShaderResourceView* AtlasSRV()
	{
		return s_atlas.ready ? s_atlas.srv.get() : nullptr;
	}

	uint32_t AtlasDim()
	{
		return s_atlas.dim;
	}

	uint32_t AtlasBaseTile()
	{
		return s_atlas.baseTile;
	}

	float AtlasOccupancy()
	{
		return s_atlas.ready ? s_atlas.allocator.Occupancy() : 0.0f;
	}

	uint64_t AtlasVRAMBytes()
	{
		return s_atlas.ready ? static_cast<uint64_t>(s_atlas.dim) * s_atlas.dim * s_atlas.bytesPerPixel : 0;
	}

	uint32_t AtlasSnapResolution(uint32_t requested)
	{
		if (!s_atlas.ready)
			return 0;
		uint32_t levels = 0;
		return SnapAtlasDim(requested, s_atlas.baseTile, s_atlas.cell, levels);
	}

	uint32_t AtlasCapacityCells()
	{
		return s_atlas.ready ? s_atlas.allocator.CellsPerAxis() * s_atlas.allocator.CellsPerAxis() : 0;
	}

	uint32_t CellsForScale(float scale)
	{
		return 1u << (2 * OrderForScale(scale));
	}

	bool EnsureSlotTile(int32_t poolSlot, float scale)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		auto& slot = s_atlas.slots[poolSlot];
		uint32_t order = OrderForScale(scale);
		const uint32_t requestedOrder = order;  // for the allocDenied check below
		// A larger tile also satisfies a demoted class: the raster fills the
		// whole tile so content and UV mapping stay exact (the advertised
		// filter scale reads slightly small). Down-realloc on every budget
		// oscillation was the dominant tile churn; UpdateAtlas reclaims
		// over-class tiles when the atlas is actually under pressure.
		if (slot.tile.valid && slot.tile.order >= order)
			return true;
		if (slot.pending.valid && slot.pending.order >= order)
			return true;  // promotion already staged; render lands in it
		const uint32_t currentFrame =
			globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		// Backoff before retrying a short-changed slot: constant retries re-evict
		// victims for no gain. Gated on the deadline alone, not slot.pending.valid.
		if (currentFrame < slot.escalateFrame)
			return true;
		// Promotion double-buffer: the current tile is NOT freed up front --
		// its content keeps being sampled until the replacement has rendered
		// (MarkSlotTileRendered swaps and frees). Freeing first blanked the
		// shadow for the window between the realloc and the next upload.
		// On allocation failure, first reclaim tiles hoarding a class above
		// their light's budget (kept lazily to avoid realloc churn), then walk
		// down classes; the quarter class always fits for any sane pool size
		// vs atlas size.
		AtlasAllocator::Tile fresh{};
		bool reclaimed = false;
		for (;;) {
			fresh = s_atlas.allocator.Allocate(order);
			if (fresh.valid)
				break;
			if (!reclaimed) {
				reclaimed = true;
				// ONE victim only, largest overshoot first, and reschedule it
				// immediately. Mass reclaim cascaded: every victim is a light
				// NOT rendering this frame, and un-rescheduled it stayed dark
				// for its whole redraw interval.
				int32_t orphanVictim = -1, idleVictim = -1, activeVictim = -1;
				uint32_t orphanBest = 0, idleOver = 0, activeOver = 0;
				double activeVictimScore = 0.0;
				const auto* requester = poolSlot < static_cast<int32_t>(s_lights.Size) ?
				                            &s_lights.Lights[poolSlot] :
				                            nullptr;
				for (size_t i = 0; i < s_atlas.slots.size(); i++) {
					auto& other = s_atlas.slots[i];
					if (static_cast<int32_t>(i) == poolSlot || !other.tile.valid)
						continue;
					const auto* entry = i < static_cast<size_t>(s_lights.Size) ? &s_lights.Lights[i] : nullptr;
					if (!entry || !entry->Light) {
						const uint32_t cells = 1u << (2 * other.tile.order);
						if (cells > orphanBest) {
							orphanBest = cells;
							orphanVictim = static_cast<int32_t>(i);
						}
						continue;
					}
					// Held lights can't repair a freed tile until they re-enter
					// scoring on un-hold -- excluded from eviction, not just orphans.
					if (IsCameraHeld(entry->Light))
						continue;
					// Already rastered into this frame's command list; evicting now
					// would corrupt GPU-visible content (parity with the node search).
					if (other.renderFrame == currentFrame)
						continue;
					const uint32_t needed = OrderForScale(entry->pendingScale);
					if (other.tile.order <= needed)
						continue;
					const uint32_t over = other.tile.order - needed;
					// Confirmed zero-demand content first: nobody samples its shadow,
					// so its blank-until-redraw window is imperceptible.
					if (DemandSkipCandidate(*entry)) {
						if (over > idleOver) {
							idleOver = over;
							idleVictim = static_cast<int32_t>(i);
						}
					} else if (over > activeOver) {
						activeOver = over;
						activeVictim = static_cast<int32_t>(i);
						activeVictimScore = entry->lastScore;
					}
				}
				// Free the largest ORPHAN first (its light already left, held only
				// for cheap re-entry); never evict an active light while an orphan
				// hoards space -- that starves active lights.
				if (orphanVictim >= 0) {
					FreeSlotTile(orphanVictim);
					continue;
				}
				if (idleVictim >= 0) {
					FreeSlotTile(idleVictim);
					s_lights.Lights[idleVictim].LastDrawnFrame = -1;
					continue;
				}
				// A watched victim falls only to a clearly higher-value requester --
				// same 2x margin the node search below enforces; otherwise the outer
				// loop walks the request down a class instead of blanking a shadow.
				if (activeVictim >= 0 && requester && requester->lastScore > activeVictimScore * 2.0) {
					FreeSlotTile(activeVictim);
					s_lights.Lights[activeVictim].LastDrawnFrame = -1;
					continue;
				}
				// Buddy allocator: freeing one arbitrary tile only unblocks this
				// request by coincidence -- search for the lowest-value ALIGNED node instead.
				if (requester) {
					const uint32_t cellsPerAxis = 1u << s_atlas.levels;
					const uint32_t nodeSize = 1u << order;
					int32_t bestNodeX = -1, bestNodeY = -1;
					double bestNodeCost = 0.0;
					static std::vector<int32_t> candidateVictims, bestVictims;
					for (uint32_t nx = 0; nx < cellsPerAxis; nx += nodeSize) {
						for (uint32_t ny = 0; ny < cellsPerAxis; ny += nodeSize) {
							bool disqualified = false;
							double nodeCost = 0.0;
							candidateVictims.clear();
							for (size_t i = 0; i < s_atlas.slots.size() && !disqualified; i++) {
								if (static_cast<int32_t>(i) == poolSlot)
									continue;
								auto& other = s_atlas.slots[i];
								bool ownerInNode = false;
								for (const AtlasAllocator::Tile* t : { &other.tile, &other.pending }) {
									if (!t->valid)
										continue;
									const uint32_t span = 1u << t->order;
									if (t->order > order) {
										// Only a real disqualification if this candidate
										// node actually falls inside that bigger tile --
										// otherwise the two don't overlap at all and the
										// bigger tile is irrelevant to this node.
										if (nx >= t->x && nx + nodeSize <= t->x + span &&
											ny >= t->y && ny + nodeSize <= t->y + span) {
											disqualified = true;
											break;
										}
										continue;
									}
									if (t->x < nx || t->x + span > nx + nodeSize ||
										t->y < ny || t->y + span > ny + nodeSize)
										continue;  // outside this candidate node
									// Already rastered into this frame's command list;
									// evicting now would corrupt GPU-visible content.
									if (other.renderFrame == currentFrame) {
										disqualified = true;
										break;
									}
									// A held owner can't repair a freed tile until it
									// re-enters scoring on un-hold -- same disqualification
									// as a currently-rastering owner, not just an omission.
									if (const auto* heldEntry = i < static_cast<size_t>(s_lights.Size) ?
									                                &s_lights.Lights[i] :
									                                nullptr;
										heldEntry && heldEntry->Light && IsCameraHeld(heldEntry->Light)) {
										disqualified = true;
										break;
									}
									ownerInNode = true;
								}
								if (disqualified)
									break;
								if (ownerInNode) {
									const auto* entry =
										i < static_cast<size_t>(s_lights.Size) ? &s_lights.Lights[i] : nullptr;
									nodeCost += entry ? entry->lastScore : 0.0;
									candidateVictims.push_back(static_cast<int32_t>(i));
								}
							}
							if (disqualified || candidateVictims.empty())
								continue;
							// Same 2x margin as the single-victim path: only evict when the
							// requester clearly outranks the whole node's combined value.
							if (requester->lastScore > nodeCost * 2.0 &&
								(bestNodeX < 0 || nodeCost < bestNodeCost)) {
								bestNodeX = static_cast<int32_t>(nx);
								bestNodeY = static_cast<int32_t>(ny);
								bestNodeCost = nodeCost;
								bestVictims = candidateVictims;
							}
						}
					}
					if (bestNodeX >= 0) {
						for (int32_t victim : bestVictims) {
							FreeSlotTile(victim);
							if (victim < static_cast<int32_t>(s_lights.Size))
								s_lights.Lights[victim].LastDrawnFrame = -1;
						}
						continue;
					}
				}
			}
			if (order == 0)
				break;
			order--;
		}
		if (!fresh.valid) {
			// Exhausted even at order 0 -- distinct from "settled for smaller"
			// below, but both count as a denial from the caller's perspective.
			s_allocDenied.fetch_add(1, std::memory_order_relaxed);
			return slot.tile.valid;  // exhausted: keep rendering the existing class
		}
		if (slot.tile.valid) {
			if (fresh.order <= slot.tile.order) {
				s_atlas.allocator.Free(fresh);  // walked down below what we hold
				// Walked down to what this slot already held despite requesting more --
				// the silent "returns true having achieved nothing" case.
				if (requestedOrder > slot.tile.order) {
					s_allocDenied.fetch_add(1, std::memory_order_relaxed);
					slot.escalateFrame = currentFrame + kAtlasEscalateRetryFrames;
				}
				return true;
			}
			if (slot.pending.valid)
				s_atlas.allocator.Free(slot.pending);  // superseded stage
			slot.pending = fresh;
			// The staged rect has no bake behind it; the composite must not
			// seed the NEW rect from static content baked at the OLD one.
			slot.staticValid = false;
			slot.escalateFrame = fresh.order < requestedOrder ?
			                         currentFrame + kAtlasEscalateRetryFrames :
			                         0;
			s_tileReallocs.fetch_add(1, std::memory_order_relaxed);
		} else {
			slot.tile = fresh;
			slot.valid = false;  // content pending first render
		}
		return true;
	}

	void MarkSlotTileRendered(int32_t poolSlot, bool a_swapComplete)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return;
		auto& slot = s_atlas.slots[poolSlot];
		if (slot.pending.valid && a_swapComplete) {
			// The staged promotion now holds rendered content: swap it in and
			// release the old rect. Sampling moves over atomically with the
			// same upload that starts advertising the new rect.
			if (slot.tile.valid)
				s_atlas.allocator.Free(slot.tile);
			slot.tile = slot.pending;
			slot.pending = {};
		}
		if (slot.tile.valid) {
			// Refresh the bake snapshot only when this mark actually rewrote the
			// sampled rect (swap or first landing); bake/composite no-op marks
			// must not rebase the snapshot onto depth rastered at an older radius.
			if (a_swapComplete || !slot.valid)
				slot.bakePending = true;
			slot.valid = true;
			slot.renderFrame =
				globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		}
	}

	bool SlotBakeSnapshotPending(int32_t poolSlot)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		return s_atlas.slots[poolSlot].bakePending;
	}

	void StoreSlotBakeSnapshot(int32_t poolSlot, const ShadowBakeSnapshot& snap)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return;
		auto& slot = s_atlas.slots[poolSlot];
		slot.bake = snap;
		slot.bakeValid = true;
		slot.bakePending = false;
	}

	bool LoadSlotBakeSnapshot(int32_t poolSlot, ShadowBakeSnapshot& out)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		const auto& slot = s_atlas.slots[poolSlot];
		if (!slot.bakeValid)
			return false;
		out = slot.bake;
		return true;
	}

	void FreeSlotTile(int32_t poolSlot)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return;
		auto& slot = s_atlas.slots[poolSlot];
		if (slot.tile.valid)
			s_atlas.allocator.Free(slot.tile);
		if (slot.pending.valid)
			s_atlas.allocator.Free(slot.pending);
		slot = {};
	}

	bool ParkOrphanSlotRecord(int32_t poolSlot)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		auto& slot = s_atlas.slots[poolSlot];
		if (!slot.tile.valid || !slot.valid || !slot.owner)
			return false;
		// Any light-free index with no record of its own can host the displaced
		// record; the owner's return finds it by owner scan, not by index.
		for (size_t i = 0; i < s_atlas.slots.size(); i++) {
			if (static_cast<int32_t>(i) == poolSlot)
				continue;
			auto& other = s_atlas.slots[i];
			if (other.tile.valid || other.pending.valid)
				continue;
			if (i < static_cast<size_t>(s_lights.Size) && s_lights.Lights[i].Light)
				continue;
			std::swap(slot, other);
			return true;
		}
		return false;
	}

	void FreeAllTiles()
	{
		if (!s_atlas.ready)
			return;
		for (auto& slot : s_atlas.slots)
			slot = {};
		s_atlas.allocator.Reset(s_atlas.levels);
	}

	void DumpSlotTileRegion(int32_t poolSlot, uint32_t a_stamp)
	{
		AtlasTileTexels t{};
		if (!GetSlotTileTexels(poolSlot, t) || t.size == 0)
			return;
		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context)
			return;
		D3D11_TEXTURE2D_DESC desc{};
		s_atlas.texture->GetDesc(&desc);
		desc.Width = t.size;
		desc.Height = t.size;
		desc.MipLevels = desc.ArraySize = 1;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Texture2D> staging;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put())))
			return;
		Util::SetResourceName(staging.get(), "SCM::RecTileDump Staging");
		D3D11_BOX box{ t.x, t.y, 0, t.x + t.size, t.y + t.size, 1 };
		context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, s_atlas.texture.get(), 0, &box);
		DirectX::ScratchImage image;
		if (FAILED(DirectX::CaptureTexture(device, context, staging.get(), image)))
			return;
		std::filesystem::path dir = "Data\\SKSE\\Plugins\\CommunityShaders\\Captures";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const auto path = dir / std::format("scm_rec_slot{}_f{}.dds", poolSlot, a_stamp);
		DirectX::SaveToDDSFile(image.GetImages(), image.GetImageCount(), image.GetMetadata(),
			DirectX::DDS_FLAGS_NONE, path.c_str());
	}

	int32_t FindFreeSlotByOwner(const void* a_owner)
	{
		if (!s_atlas.ready || !a_owner)
			return -1;
		for (size_t i = 0; i < s_atlas.slots.size(); i++) {
			const auto& slot = s_atlas.slots[i];
			if (slot.owner == a_owner && slot.tile.valid && slot.orphanSince != 0)
				return static_cast<int32_t>(i);
		}
		return -1;
	}

	bool GetSlotTileTexels(int32_t poolSlot, AtlasTileTexels& out)
	{
		// RENDER-side rect: rasters land in the staged promotion tile when one
		// exists. The SAMPLE side (GetSlotAtlasRectUV) keeps reading the
		// current tile until MarkSlotTileRendered swaps.
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		const auto& slot = s_atlas.slots[poolSlot];
		const auto& t = slot.pending.valid ? slot.pending : slot.tile;
		if (!t.valid)
			return false;
		out.x = t.x * s_atlas.cell;
		out.y = t.y * s_atlas.cell;
		out.size = (1u << t.order) * s_atlas.cell;
		out.lastRenderFrame = slot.renderFrame;
		out.contentValid = slot.pending.valid ? false : slot.valid;
		return true;
	}

	AtlasClearStats GetAtlasClearStats()
	{
		return {
			s_clearsSwallowed.load(std::memory_order_relaxed),
			s_clearsPassed.load(std::memory_order_relaxed),
			s_tileClears.load(std::memory_order_relaxed),
			s_tileReallocs.load(std::memory_order_relaxed),
			s_ownerInvalidations.load(std::memory_order_relaxed),
			s_allocDenied.load(std::memory_order_relaxed)
		};
	}

	bool GetSlotAtlasRectUV(int32_t poolSlot, AtlasRectUV& out)
	{
		// SAMPLE-side rect: always the swapped-in tile, never the staged
		// promotion (its content is mid-render).
		if (!s_atlas.ready || s_atlas.dim == 0 || poolSlot < 0 ||
			static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		const auto& slot = s_atlas.slots[poolSlot];
		if (!slot.tile.valid || !slot.valid)
			return false;
		const float dim = static_cast<float>(s_atlas.dim);
		const float size = static_cast<float>((1u << slot.tile.order) * s_atlas.cell);
		out.scaleX = out.scaleY = size / dim;
		out.biasX = static_cast<float>(slot.tile.x * s_atlas.cell) / dim;
		out.biasY = static_cast<float>(slot.tile.y * s_atlas.cell) / dim;
		// Class scale derived from the ADVERTISED tile itself, so the shader's
		// texel-size-scaled depth bias can never drift from the sampled rect
		// (a stale per-light renderedScale gave small tiles full-class bias:
		// 16-64x too little, reading as a self-shadow acne halo around the
		// light's whole footprint).
		out.classScale = s_atlas.baseTile ? size / static_cast<float>(s_atlas.baseTile) : 1.0f;
		return true;
	}

	void ClearSlotTile(int32_t poolSlot)
	{
		AtlasTileTexels t{};
		if (!GetSlotTileTexels(poolSlot, t))
			return;
		const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		D3D11_RECT rect{ static_cast<LONG>(t.x), static_cast<LONG>(t.y),
			static_cast<LONG>(t.x + t.size), static_cast<LONG>(t.y + t.size) };
		s_atlas.context1->ClearView(s_atlas.dsv.get(), farDepth, &rect, 1);
		s_tileClears.fetch_add(1, std::memory_order_relaxed);
	}

	bool StaticAtlasReady()
	{
		return s_atlas.staticReady;
	}

	ID3D11DepthStencilView* StaticAtlasDSV(bool)
	{
		// One writable DSV; the read-only variant is unused for the bake pass.
		return s_atlas.staticDsv.get();
	}

	void ClearStaticSlotTile(int32_t poolSlot)
	{
		AtlasTileTexels t{};
		if (!s_atlas.staticReady || !GetSlotTileTexels(poolSlot, t))
			return;
		const FLOAT farDepth[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		D3D11_RECT rect{ static_cast<LONG>(t.x), static_cast<LONG>(t.y),
			static_cast<LONG>(t.x + t.size), static_cast<LONG>(t.y + t.size) };
		s_atlas.context1->ClearView(s_atlas.staticDsv.get(), farDepth, &rect, 1);
	}

	void CopyStaticTileToLive(int32_t poolSlot)
	{
		AtlasTileTexels t{};
		if (!s_atlas.staticReady || !GetSlotTileTexels(poolSlot, t))
			return;
		CS_GPU_PASS("SCM::Render::StaticCopy");
		auto* context = globals::d3d::context;
		// Unbind first: the static texture is the copy source and may still be
		// the OM depth target from the bake pass; a resource cannot be both.
		context->OMSetRenderTargets(0, nullptr, nullptr);
		D3D11_BOX box{ t.x, t.y, 0, t.x + t.size, t.y + t.size, 1 };
		context->CopySubresourceRegion(s_atlas.texture.get(), 0, t.x, t.y, 0,
			s_atlas.staticTexture.get(), 0, &box);
	}

	bool GetSlotStaticState(int32_t poolSlot, uint64_t& hashOut, bool& validOut, bool* emptyOut)
	{
		if (!s_atlas.ready || poolSlot < 0 || static_cast<size_t>(poolSlot) >= s_atlas.slots.size())
			return false;
		const auto& slot = s_atlas.slots[poolSlot];
		if (!slot.tile.valid)
			return false;
		hashOut = slot.staticHash;
		validOut = slot.staticValid;
		if (emptyOut)
			*emptyOut = slot.staticEmpty;
		return true;
	}

	void MarkSlotStaticRendered(int32_t poolSlot, uint64_t staticHash, bool a_sawCasters)
	{
		if (s_atlas.ready && poolSlot >= 0 && static_cast<size_t>(poolSlot) < s_atlas.slots.size() &&
			s_atlas.slots[poolSlot].tile.valid) {
			s_atlas.slots[poolSlot].staticValid = true;
			s_atlas.slots[poolSlot].staticHash = staticHash;
			s_atlas.slots[poolSlot].staticEmpty = !a_sawCasters;
		}
	}

	void InvalidateAllStaticBakes()
	{
		// Cell-grid-shift response: drop only the static cache, not the live
		// tile or slot ownership -- self-staggers each light's next rebake
		// through the existing per-frame budget instead of a sync rebake storm.
		if (!s_atlas.ready)
			return;
		for (auto& slot : s_atlas.slots) {
			if (!slot.tile.valid)
				continue;
			slot.staticValid = false;
			slot.staticHash = 0;
			slot.staticEmpty = false;
		}
	}
}

// ShadowSlotAllocator.cpp
// kSHADOWMAPS slot bookkeeping: pool allocation, allocated-slice verification, VRAM telemetry.

#include "../../Deferred.h"
#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../../Utils/D3D.h"
#include "../../Utils/Game.h"
#include "../../Utils/UI.h"
#include "../Upscaling.h"
#include "../VR.h"
#include "I18n/I18n.h"
#include "ShadowCasterInternal.h"

namespace ShadowCasterManager
{
	// =========================================================================
	// LightContainer methods
	// =========================================================================

	// Engine writes focus shadows to kSHADOWMAPS slots
	// [kFocusShadowBaseSlotIndex .. +s_focusShadowSlots) (DAT_141867188 = 4
	// in vanilla, max 4 actors). Two predicates separate "could be claimed"
	// from "currently claimed":
	//   IsFocusShadowReservableSlot(i) -- in the full [4..8) range that
	//     focus might use. FindFreeIndex treats these as last-resort so an
	//     actor appearance rarely needs to evict anything.
	//   IsFocusShadowSlot(i) -- currently held by an active focus actor;
	//     never allocated, and any point light here gets ejected at
	//     scheduling time.

	static inline bool IsFocusShadowReservableSlot(int32_t i)
	{
		return i >= kFocusShadowBaseSlotIndex && i < kFocusShadowBaseSlotIndex + kFocusShadowMaxSlots;
	}

	static inline bool IsFocusShadowSlot(int32_t i)
	{
		return i >= kFocusShadowBaseSlotIndex && i < kFocusShadowBaseSlotIndex + s_focusShadowSlots;
	}

	int32_t LightContainer::FindFreeIndex(bool shadowSlot, int32_t shadowCount, int32_t convertCount) const
	{
		// Pool layout when Sun=true:  [0]=sun, [1..shadowCount]=point lights, [shadowCount+1..]=converted
		//                  Sun=false: [0..shadowCount-1]=point lights,        [shadowCount..]=converted
		//
		// Slot 0 is reserved for the sun pointer when present (sunOff=1) so
		// FindLight can locate the sun. The sun renders to kSHADOWMAPS_ESRAM
		// (target 2), not kSHADOWMAPS (target 4), so slot 0 in our pool maps
		// to a kSHADOWMAPS slice the sun never writes -- safe to leave as a
		// bookkeeping placeholder.
		//
		// Slots 4..7 are the engine's focus shadow range. Two-pass allocation:
		// preferred slots first (avoiding 4..7 entirely so an actor appearance
		// rarely needs to evict), then 4..7 as fallback for slots not
		// currently claimed by an active focus actor.
		const int32_t sunOff = Sun ? 1 : 0;
		const int32_t shadowEnd = sunOff + shadowCount;
		auto scanShadow = [&](auto reservablePolicy) -> int32_t {
			for (int i = sunOff; i < shadowEnd; i++) {
				if (reservablePolicy(i))
					continue;
				if (IsFocusShadowSlot(i))
					continue;  // actively held by focus right now
				if (!Lights[i].Light)
					return i;
			}
			return -1;
		};
		if (shadowSlot) {
			// First pass: skip the entire focus-reservable range so a focus
			// actor appearance ideally finds those slots already empty.
			if (int32_t i = scanShadow([](int32_t k) { return IsFocusShadowReservableSlot(k); }); i >= 0)
				return i;
			// Fallback: fill the unclaimed focus-reservable slots from the
			// top down. Engine packs FocusShadowActors densely from slot
			// kFocusShadowBaseSlotIndex upward (player first, then tracked
			// NPCs in priority order), so slot 7 is the LAST to be claimed
			// as focus count grows. Placing a point light there has the
			// lowest probability of being evicted later.
			for (int32_t i = kFocusShadowBaseSlotIndex + kFocusShadowMaxSlots - 1; i >= kFocusShadowBaseSlotIndex; --i) {
				if (i >= shadowEnd)
					continue;
				if (IsFocusShadowSlot(i))
					continue;
				if (!Lights[i].Light)
					return i;
			}
			return -1;
		}
		// Converted lights live past the shadow range and never collide with
		// focus slots (focus base is 4, converted base is >= sunOff + shadowCount > 4).
		const int32_t convBase = shadowEnd;
		for (int i = convBase; i < convBase + convertCount; i++) {
			if (!Lights[i].Light)
				return i;
		}
		return -1;
	}

	int32_t LightContainer::FindLight(RE::BSShadowLight* light, int32_t shadowCount) const
	{
		// Search the full allocation range. Hook_OverwriteShadowMapIndex calls
		// in for the sun too, so the sun pointer (in slot 0 when Sun=true) must
		// be findable. The fallback to idx=0 on -1 silently corrupts slot 0,
		// so the search range must match FindFreeIndex's allocation range.
		const int32_t sunOff = Sun ? 1 : 0;
		const int32_t maxIdx = sunOff + shadowCount;
		for (int i = 0; i < maxIdx; i++)
			if (Lights[i].Light == light)
				return i;
		return -1;
	}

	std::uint32_t MaxShadowAccumIterationBound()
	{
		// Each entry advances idx by its shadowMapCount. Worst-case per
		// light is the directional sun's cascade count (iNumSplits:Display,
		// INI-capped at 3 by ShadowmapRasterizerFix). 4 is a defensive
		// upper bound. With ShadowLightCount user-capped at 127 plus one
		// sun bookkeeping slot, the walked index never exceeds
		// (1 + 127) * 4 = 512; add a margin so a transient mismatch
		// between live settings and an already-populated engine array
		// doesn't tripwire iteration.
		constexpr std::uint32_t kCascadesPerLight = 4;
		constexpr std::uint32_t kMargin = 16;
		const std::uint32_t lights = static_cast<std::uint32_t>(std::max(1, s_settings.ShadowLightCount));
		return (lights + 1) * kCascadesPerLight + kMargin;
	}

	VRAMVerdict EvaluateVRAMVerdict(std::uint64_t shadowBytes, std::uint64_t freeBytes, std::uint64_t budgetBytes)
	{
		constexpr std::uint64_t kTightFree = 512ull * 1024 * 1024;
		constexpr std::uint64_t kOverFree = 128ull * 1024 * 1024;
		VRAMVerdict v;
		v.tight = freeBytes < kTightFree || shadowBytes * 4 > budgetBytes;
		v.over = freeBytes < kOverFree || shadowBytes * 2 > budgetBytes;
		v.colour = v.over  ? ImVec4(0.95f, 0.35f, 0.35f, 1) :
		           v.tight ? ImVec4(0.95f, 0.85f, 0.25f, 1) :
		                     ImVec4(0.55f, 0.85f, 0.55f, 1);
		return v;
	}

	// Reads kSHADOWMAPS's underlying Texture2D desc, bypassing the SRV's
	// ViewDimension. Skyrim creates the SRV with a non-array view dimension
	// even though the resource itself is a Texture2DArray, so reading
	// `desc.Texture2DArray.ArraySize` from the SRV desc returns 0; only the
	// texture's own ArraySize is reliable. Returns false on any failure
	// stage; out param is left untouched.
	bool TryReadShadowTextureDesc(D3D11_TEXTURE2D_DESC& out)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		auto* srv = renderer->GetDepthStencilData()
		                .depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kSHADOWMAPS]
		                .depthSRV;
		if (!srv)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		if (!Util::GetTexture2DDesc(Util::AsReal<ID3D11View>(srv), desc))
			return false;
		if (desc.ArraySize == 0)
			return false;
		out = desc;
		return true;
	}

	// Lazily verifies that the engine's actual kSHADOWMAPS slice count
	// matches what SCM patched in. Self-healing: bails until the texture
	// is readable, then early-returns. Cross-checks against the requested
	// count and clamps the scheduler on mismatch so out-of-bounds slice
	// indexing can't occur after a VRAM-exhaustion fallback.
	void RefreshInstalledSlotCount()
	{
		if (s_installedSlotCount > 0)
			return;

		D3D11_TEXTURE2D_DESC desc{};
		if (!TryReadShadowTextureDesc(desc))
			return;

		uint32_t actual = desc.ArraySize;
		s_installedSlotCount = actual;
		if (s_slotCountLogged)
			return;
		s_slotCountLogged = true;
		// Atlas boot mode expects the vanilla 8-slice array; the pool is
		// intentionally larger, so skip the mismatch clamp.
		if (s_bootAtlasEnabled)
			return;
		if (s_requestedSlotCount && actual != s_requestedSlotCount) {
			logger::warn(
				"[SCM] Requested {} kSHADOWMAPS slots, GPU allocated {} -- "
				"clamping scheduler to the actual count.",
				s_requestedSlotCount, actual);
			s_installedShadowLightCount = std::min(s_installedShadowLightCount, static_cast<int32_t>(actual));
		} else {
			logger::info("[SCM] kSHADOWMAPS array verified: {} slots allocated", actual);
		}
	}

	uint32_t GetInstalledSlotCount()
	{
		// Atlas boot mode: shadow records are keyed by POOL slot while the
		// engine array stays at the vanilla slice count; span the pool.
		if (s_bootAtlasEnabled)
			return static_cast<uint32_t>(std::max(s_installedShadowLightCount + 1, 1));
		// Lazy-refresh; cheap once verified. Fall back to the requested
		// count when verification can't complete -- a non-zero slot count
		// is needed for the cluster pipeline to engage shadow handling.
		// Out-of-bounds slice indexes are hardware-clamped in D3D11, so a
		// transient over-estimate yields stale shadow data rather than a
		// crash.
		RefreshInstalledSlotCount();
		return s_installedSlotCount > 0 ? s_installedSlotCount : s_requestedSlotCount;
	}

	bool IsSessionResetPending()
	{
		return s_pendingSessionReset.load(std::memory_order_acquire);
	}

	// Resolution actually used to allocate kSHADOWMAPS this session. Captured
	// lazily from the real D3D11 texture geometry the first time it becomes
	// readable -- NOT from the RE::Setting at Install() time. The engine's
	// SkyrimPrefs.ini load happens after our PostPostLoad hook, so a snapshot
	// at Install() catches the hardcoded default (e.g. 2048) before the
	// user's INI value (e.g. 4096) is applied. Reading from the texture is
	// the source of truth either way -- it reflects what was actually
	// allocated, regardless of where the setting ended up.
	std::int32_t s_initialShadowMapResolution = 0;

	// kSHADOWMAPS footprint = w*h*bytesPerPixel*ArraySize. Per-slice cost
	// is 64 MB at 4K D32_FLOAT; arrays grow linearly with ShadowLightCount.
	// Returned info.valid is false only when both the DXGI budget query
	// and the texture/INI fallback fail (rare).
	VRAMInfo GetVRAMInfo()
	{
		VRAMInfo info{};

		// DXGI budget. Prefer Menu's cached adapter; fall back to the
		// device-derived path before Menu::Init() has run.
		winrt::com_ptr<IDXGIAdapter3> adapter3;
		if (auto* menu = Menu::GetSingleton())
			adapter3 = menu->GetDXGIAdapter3();
		if (!adapter3 && globals::d3d::device) {
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			if (SUCCEEDED(globals::d3d::device->QueryInterface(dxgiDevice.put()))) {
				winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
				if (SUCCEEDED(dxgiDevice->GetAdapter(dxgiAdapter.put())))
					dxgiAdapter->QueryInterface(adapter3.put());
			}
		}
		if (adapter3) {
			DXGI_QUERY_VIDEO_MEMORY_INFO vmem{};
			HRESULT hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmem);
			if (SUCCEEDED(hr) && vmem.Budget > 0) {
				info.currentUsageBytes = vmem.CurrentUsage;
				info.budgetBytes = vmem.Budget;
			}
		}

		// kSHADOWMAPS geometry from the underlying texture (when readable).
		D3D11_TEXTURE2D_DESC desc{};
		if (TryReadShadowTextureDesc(desc)) {
			info.shadowWidth = desc.Width;
			info.shadowHeight = desc.Height;
			info.shadowSlices = desc.ArraySize;
			// Latch the texture's allocated resolution as the canonical
			// "what this session is using" value -- this is what the UI's
			// restart-required indicator compares against. Once latched it
			// doesn't move for the session (kSHADOWMAPS is allocated once).
			if (s_initialShadowMapResolution == 0)
				s_initialShadowMapResolution = static_cast<std::int32_t>(desc.Width);
			// Default to 4 B/pixel (R32_TYPELESS / D32_FLOAT, the format
			// Skyrim ships with) and override for stencil-packed variants.
			std::uint32_t bytesPerPixel = 4;
			switch (desc.Format) {
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
			case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
				bytesPerPixel = 8;
				break;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				bytesPerPixel = 2;
				break;
			default:
				break;  // 4 B fallback covers R24G8 and R32 families
			}
			info.bytesPerSlice = info.shadowWidth * info.shadowHeight * bytesPerPixel;
			info.shadowArrayBytes = static_cast<std::uint64_t>(info.bytesPerSlice) * info.shadowSlices;
		}

		// INI-based fallback when the texture isn't readable yet (e.g.
		// main menu, before BSShaderRenderTargets_Create). Resolution
		// from SkyrimPrefs.ini, slot count from settings; assume the
		// stock D32_FLOAT format (4 B/pixel).
		if (info.bytesPerSlice == 0) {
			std::uint32_t res = 4096;  // SkyrimPrefs.ini default
			if (auto* prefColl = RE::INIPrefSettingCollection::GetSingleton()) {
				if (auto* setting = prefColl->GetSetting("iShadowMapResolution:Display")) {
					int v = setting->GetInteger();
					if (v > 0)
						res = static_cast<std::uint32_t>(v);
				}
			}
			info.shadowWidth = res;
			info.shadowHeight = res;
			info.bytesPerSlice = info.shadowWidth * info.shadowHeight * 4;
			info.shadowSlices = static_cast<std::uint32_t>(s_settings.ShadowLightCount);
			info.shadowArrayBytes = static_cast<std::uint64_t>(info.bytesPerSlice) * info.shadowSlices;
		}

		// Budget and per-slice are independent so a partial answer still
		// renders (budget alone shows VRAM headroom, per-slice alone shows
		// projection from the INI fallback).
		info.valid = info.budgetBytes > 0 || info.bytesPerSlice > 0;

		// One-shot log on first valid observation. Any caller trips it.
		static bool s_loggedFirstValid = false;
		if (info.valid && !s_loggedFirstValid) {
			s_loggedFirstValid = true;
			const std::uint64_t freeBytes = info.budgetBytes > info.currentUsageBytes ? info.budgetBytes - info.currentUsageBytes : 0;
			const float arrayMB = static_cast<float>(info.shadowArrayBytes) / (1024.f * 1024.f);
			const float perSliceMB = static_cast<float>(info.bytesPerSlice) / (1024.f * 1024.f);
			const float budgetMB = static_cast<float>(info.budgetBytes) / (1024.f * 1024.f);
			const float usageMB = static_cast<float>(info.currentUsageBytes) / (1024.f * 1024.f);
			logger::info(
				"[SCM] kSHADOWMAPS {}x{} x {} slices, {:.2f} MB/slice -> {:.1f} MB "
				"(VRAM {:.1f}/{:.1f} MB used, ShadowLightCount={})",
				info.shadowWidth, info.shadowHeight, info.shadowSlices,
				perSliceMB, arrayMB, usageMB, budgetMB, s_settings.ShadowLightCount);
			if (info.shadowArrayBytes > freeBytes) {
				logger::warn(
					"[SCM] Shadow texture array ({:.1f} MB) exceeds remaining VRAM budget "
					"({:.1f} MB). Lower Shadow Light Count or iShadowMapResolution if you "
					"see stutter or driver hitches.",
					arrayMB, static_cast<float>(freeBytes) / (1024.f * 1024.f));
			}
		}

		return info;
	}

	std::uint64_t ProjectShadowArrayBytes(std::uint32_t sliceCount)
	{
		auto info = GetVRAMInfo();
		if (!info.valid)
			return 0;
		return static_cast<std::uint64_t>(info.bytesPerSlice) * sliceCount;
	}

}

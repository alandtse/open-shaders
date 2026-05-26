#pragma once

#include <cstdint>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// Forward-declared so the header doesn't drag in <d3d11.h>. The plugin's PCH
// brings the real types into scope at definition sites.
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

// Mirrors the global `using json = nlohmann::json;` from the plugin PCH so
// the header builds standalone (e.g. in unit-test targets that don't
// precompile PCH). Identical aliases in the same scope are well-defined.
using json = nlohmann::json;

namespace Util::Subrect
{
	struct UVRegion
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 1.0f;
		float h = 1.0f;
	};

	struct PixelRegion
	{
		uint32_t x = 0;
		uint32_t y = 0;
		uint32_t w = 1;
		uint32_t h = 1;
	};

	struct StereoPixelRegions
	{
		PixelRegion leftEye;
		PixelRegion rightEye;
	};

	struct Preset
	{
		std::string name;
		UVRegion uv;  // Left-eye UV when stereo is enabled; sole UV otherwise.
		// Right-eye UV. `std::nullopt` means "no explicit right eye — auto-mirror
		// left around x=0.5 when stereo is enabled". A default-constructed
		// `UVRegion{}` (full frame) would otherwise be ambiguous: it could mean
		// "the user wants full frame" or "the caller didn't supply one", and
		// the silent-full-frame case bites SeedDefaultPresets callers that only
		// fill `.name` and `.uv`.
		std::optional<UVRegion> rightUV{};
	};

	// "User picks a sub-rectangle of an image" controller. Crop UV is in [0,1]
	// of the source the caller passes to GetPixelRegion(). Hosts that want
	// preset-based eye selection seed Left/Right/Full Frame via SeedDefaultPresets.
	//
	// Stereo: hosts that consume a side-by-side stereo texture call
	// SetStereoEnabled(true) to track a separate right-eye UV. Right-eye UV
	// auto-mirrors left around x=0.5 unless explicitly edited; this matches
	// HMD nose-side overlap symmetry.
	class Controller
	{
	public:
		void LoadSettings(const json& a_json);
		void SaveSettings(json& a_json) const;

		// Replaces the built-in "Full Frame" placeholder used when JSON has no
		// CropPresets entry yet. Empty-case only - user edits/deletions persist.
		void SeedDefaultPresets(std::vector<Preset> defaults);

		// Toggles right-eye UV tracking. Off by default (mono).
		// When enabled, edits to the primary UV auto-mirror to the right-eye
		// UV (around x=0.5), and SaveSettings emits the extra right-eye keys.
		void SetStereoEnabled(bool enabled);
		bool IsStereoEnabled() const { return stereoEnabled; }

		// uvStartX/uvVisibleWidth window the preview onto a sub-region of the
		// texture; crop UV stays in [0,1] of that window. imageRenderCallback,
		// when non-null, is queued via ImDrawList::AddCallback around the
		// preview Image draw (paired with ImDrawCallback_ResetRenderState) so
		// hosts can override blend state for the image specifically. Pass
		// OpaquePreviewBlendCallback when the preview texture is an RT with
		// non-1 alpha (kMAIN, etc.) to suppress menu-background bleed-through.
		void DrawEditor(ID3D11ShaderResourceView* previewSrv, ID3D11Texture2D* previewTexture,
			float uvVisibleWidth = 1.0f, float uvStartX = 0.0f,
			ImDrawCallback imageRenderCallback = nullptr);

		// Resolves the crop UV against an arbitrary pixel size.
		PixelRegion GetPixelRegion(uint32_t width, uint32_t height) const;

		// In stereo mode, resolves both eyes' UVs against an SBS texture by
		// dividing width by 2. In mono mode, both eyes resolve from currentUV.
		//
		// Coordinate space: both leftEye.x and rightEye.x are in PER-EYE
		// space (i.e. x in [0, fullWidth/2)) — the right eye is NOT
		// pre-offset by eyeWidth. Callers that draw into the full SBS
		// texture must add `fullWidth / 2` to rightEye.x themselves.
		StereoPixelRegions GetStereoPixelRegions(uint32_t fullWidth, uint32_t fullHeight) const;

		const UVRegion& GetUV() const { return currentUV; }
		const UVRegion& GetRightEyeUV() const { return stereoEnabled ? currentRightUV : currentUV; }

	private:
		std::vector<Preset> presets;
		std::vector<Preset> seededDefaults;
		int selectedPresetIndex = 0;
		char newPresetName[64] = "";

		UVRegion currentUV{};
		UVRegion currentRightUV{};
		bool stereoEnabled = false;
		// True once LoadSettings sees an explicit CropRight* key. Suppresses
		// the auto-mirror in SetStereoEnabled(true) so a deliberate JSON
		// right-eye crop survives a mono→stereo transition that happens
		// after the load.
		bool rightUVLoadedFromJson = false;

		bool isDraggingCrop = false;
		float dragStartUV[2] = { 0.0f, 0.0f };

		void EnsureDefaultPreset();
		void ClampCurrentUV();
		void ApplyPreset(int index);
		void SyncRightUV();
	};

	// Opaque-RGB blend state callback for Controller::DrawEditor. Pass when the
	// preview SRV is a render target with non-1 alpha (kMAIN, kTOTAL, etc.).
	// ImGui's default SRC_ALPHA blend would let the menu background bleed
	// through where the source alpha is < 1, making the preview look like a
	// transparency mask. This callback switches to opaque RGB-only writes
	// around the Image draw; DrawEditor queues ImDrawCallback_ResetRenderState
	// immediately after to restore default state.
	//
	// Two non-obvious regression risks if reimplemented:
	//   1. BlendEnable must stay FALSE — SRC_ALPHA causes the bleed-through.
	//   2. WriteMask must exclude alpha (RGB only). In VR, Skyrim's menu UI
	//      shader recomposites the menu plate over the SBS framebuffer with
	//      alpha blending; writing texture alpha into the menu plate RT
	//      produces a cutout visible only through the HMD. RGB-only writes
	//      leave the plate's pre-cleared alpha=1 in place.
	void OpaquePreviewBlendCallback(const ImDrawList*, const ImDrawCmd*);
}  // namespace Util::Subrect

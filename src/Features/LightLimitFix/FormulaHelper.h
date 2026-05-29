#pragma once

#include <cstdint>
#include <string>

// exprtk-backed expression evaluator for ShadowCasterManager's score /
// redraw-interval / redraw-budget formulas. Extracted to its own translation
// unit so the (very large) exprtk header compiles in exactly one place and the
// formula logic can be unit-tested without the game/RE runtime. The header
// keeps exprtk out via a void* pimpl, so it stays cheap to include.
namespace ShadowCasterManager
{
	// -------------------------------------------------------------------------
	// Formula parameter indices (bound to symbol-table variables by name).
	// -------------------------------------------------------------------------
	enum FormulaParams
	{
		kFormulaParam_LightIndex,
		kFormulaParam_LightIntensity,
		kFormulaParam_LightDistance,
		kFormulaParam_LightRadius,
		kFormulaParam_LightX,
		kFormulaParam_LightY,
		kFormulaParam_LightZ,
		kFormulaParam_LightR,
		kFormulaParam_LightG,
		kFormulaParam_LightB,
		kFormulaParam_LightAmbientR,
		kFormulaParam_LightAmbientG,
		kFormulaParam_LightAmbientB,
		kFormulaParam_LightChosenLastFrame,
		kFormulaParam_LightFramesSinceRender,  ///< frames since this light's slot was last rendered; large sentinel if never rendered or no slot
		kFormulaParam_LightNeverFades,
		kFormulaParam_LightPortalStrict,
		kFormulaParam_LightNS,
		kFormulaParam_LightConverted,
		kFormulaParam_LightDisplacement,    ///< distance moved since last shadow map render (game units)
		kFormulaParam_PlayerLightDistance,  ///< distance from the player character to the light (game units)
		kFormulaParam_LightImportance,      ///< contribution importance: lum(diffuse*fade) * max(att_cam, att_plr); set in interval loop only
		kFormulaParam_LightIsSpot,          ///< 1 if light is a spot (BSShadowFrustumLight), 0 otherwise
		kFormulaParam_LightSpotVisible,     ///< 1 if a spot's cone is plausibly visible to the camera (cone-aimed-at-frustum). Always 1 for non-spots so omni-only formulas aren't affected.

		kFormulaParam_CameraX,
		kFormulaParam_CameraY,
		kFormulaParam_CameraZ,
		kFormulaParam_IsInterior,
		kFormulaParam_TimeOfDay,

		kFormulaParam_FrameTime,     ///< EMA-smoothed frame time (ms)
		kFormulaParam_FrameTarget,   ///< 90th-percentile frame time (ms) — target budget ceiling
		kFormulaParam_StableFrames,  ///< consecutive frames the EMA has been below FrameTarget

		kFormulaParam_Max
	};

	struct FormulaVarInfo
	{
		const char* name;
		const char* description;
		int32_t index;
	};

	// Single authoritative list of formula variables. Drives both symbol-table
	// registration (FormulaHelper) and the formula-editor help text (UI).
	inline constexpr FormulaVarInfo kFormulaVars[] = {
		{ "lightindex", "sequential index of this candidate light", kFormulaParam_LightIndex },
		{ "lightintensity", "NiLight fade/intensity", kFormulaParam_LightIntensity },
		{ "lightdistance", "camera-to-light distance (game units; 1 unit ~= 1.428 cm)", kFormulaParam_LightDistance },
		{ "lightradius", "light radius/range (game units; 1 unit ~= 1.428 cm)", kFormulaParam_LightRadius },
		{ "lightx", "light world X", kFormulaParam_LightX },
		{ "lighty", "light world Y", kFormulaParam_LightY },
		{ "lightz", "light world Z", kFormulaParam_LightZ },
		{ "lightr", "diffuse red", kFormulaParam_LightR },
		{ "lightg", "diffuse green", kFormulaParam_LightG },
		{ "lightb", "diffuse blue", kFormulaParam_LightB },
		{ "lightambientr", "ambient red", kFormulaParam_LightAmbientR },
		{ "lightambientg", "ambient green", kFormulaParam_LightAmbientG },
		{ "lightambientb", "ambient blue", kFormulaParam_LightAmbientB },
		{ "lightchosenlastframe", "1 if this light held a slot last frame", kFormulaParam_LightChosenLastFrame },
		{ "lightframessincerender", "frames since this light's slot was last actually rendered into the shadow atlas; 1e6 sentinel when never rendered or unassigned", kFormulaParam_LightFramesSinceRender },
		{ "lightneverfades", "1 if lodFade disabled (permanent light)", kFormulaParam_LightNeverFades },
		{ "lightportalstrict", "1 if portal-strict (always 1 for shadow casters)", kFormulaParam_LightPortalStrict },
		{ "lightns", "1 if promoted from normal light (PromoteNormalToShadow)", kFormulaParam_LightNS },
		{ "lightconverted", "1 if light is in the converted (non-shadow) slot range", kFormulaParam_LightConverted },
		{ "lightdisplacement", "distance this light moved since its last shadow map render (game units; 0 when not yet tracked or in score formula)", kFormulaParam_LightDisplacement },
		{ "playerlightdistance", "distance from the player character to the light (game units; falls back to lightdistance when player unavailable)", kFormulaParam_PlayerLightDistance },
		{ "lightimportance", "contribution score: lum(diffuse*fade) * max(att_cam,att_plr) where att=(1-(dist/radius)^2)^2; 0 in score formula", kFormulaParam_LightImportance },
		{ "lightisspot", "1 if this is a spot/frustum shadow light (BSShadowFrustumLight); 0 for omni / hemi / sun", kFormulaParam_LightIsSpot },
		{ "lightspotvisible", "1 if the spot's cone plausibly reaches the camera frustum, 0 otherwise. Always 1 for non-spot lights so existing omni-only formulas are unaffected", kFormulaParam_LightSpotVisible },
		{ "camerax", "camera world X", kFormulaParam_CameraX },
		{ "cameray", "camera world Y", kFormulaParam_CameraY },
		{ "cameraz", "camera world Z", kFormulaParam_CameraZ },
		{ "isinterior", "1 in interior cells, 0 outdoors", kFormulaParam_IsInterior },
		{ "timeofday", "in-game hour (0.0-24.0)", kFormulaParam_TimeOfDay },
		{ "frametime", "EMA-smoothed frame time (ms)", kFormulaParam_FrameTime },
		{ "frametarget", "90th-percentile recent frame time (ms) -- headroom ceiling", kFormulaParam_FrameTarget },
		{ "stableframes", "consecutive frames EMA has been below frametarget", kFormulaParam_StableFrames },
	};

	// -------------------------------------------------------------------------
	// Expression-based formula evaluator (wraps exprtk).
	// -------------------------------------------------------------------------
	struct FormulaHelper
	{
		FormulaHelper();
		~FormulaHelper();

		FormulaHelper(const FormulaHelper&) = delete;
		FormulaHelper& operator=(const FormulaHelper&) = delete;
		FormulaHelper(FormulaHelper&&) = delete;
		FormulaHelper& operator=(FormulaHelper&&) = delete;

		bool Parse(const std::string& input);
		double Calculate();

		/// Re-parse with a new expression, replacing any previously compiled formula.
		/// Returns true on success. On failure the old formula remains active.
		bool Reparse(const std::string& input);

		/// Compile `input` into a temporary expression and return true if it succeeds.
		/// On failure, `errorOut` receives the first parser error message.
		/// Does NOT affect the active formula.
		static bool Validate(const std::string& input, std::string& errorOut);

		static void SetParam(int32_t index, double value);
		static double GetParam(int32_t index);

	private:
		void* _ptr;
	};
}

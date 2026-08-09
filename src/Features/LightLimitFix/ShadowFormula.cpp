// ShadowFormula.cpp
// exprtk-backed scoring formulas: FormulaHelper, the symbol table, per-frame/per-light param setup.
// Only translation unit including exprtk.hpp (heavy header; keep it out of the other SCM modules).

#include "../../Globals.h"
#include "ShadowCasterInternal.h"

#include <exprtk.hpp>

namespace ShadowCasterManager
{
	struct FormulaWrapper
	{
		exprtk::expression<double> expression;
		exprtk::parser<double> parser;
	};

	static double s_formulaParams[kFormulaParam_Max];
	static exprtk::symbol_table<double> s_symbolTable;
	static bool s_formulaInited = false;

	// EMA position anchor ComputeLightGeometry scores from; see ResetScoreAnchor.
	static std::unordered_map<const RE::NiLight*, RE::NiPoint3> s_scoreAnchor;

	void ResetScoreAnchor(const RE::NiLight* ni)
	{
		if (ni)
			s_scoreAnchor.erase(ni);
	}

	static void InitFormulaSystem()
	{
		if (s_formulaInited)
			return;
		s_formulaInited = true;

		memset(s_formulaParams, 0, sizeof(double) * kFormulaParam_Max);

		for (const auto& v : kFormulaVars)
			s_symbolTable.add_variable(v.name, s_formulaParams[v.index]);
	}

	FormulaHelper::FormulaHelper() :
		_ptr(nullptr) { InitFormulaSystem(); }

	FormulaHelper::~FormulaHelper()
	{
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
	}

	bool FormulaHelper::Parse(const std::string& input)
	{
		if (_ptr)
			return false;
		auto* w = new FormulaWrapper();
		w->expression.register_symbol_table(s_symbolTable);
		// Defer the _ptr assignment until compile succeeds. Otherwise a
		// failed compile leaves the helper in a "parsed" state (Calculate
		// would evaluate an uncompiled expression and the early-return
		// guard above would block subsequent Parse retries).
		if (!w->parser.compile(input, w->expression)) {
			delete w;
			return false;
		}
		_ptr = w;
		return true;
	}

	double FormulaHelper::Calculate()
	{
		auto* w = static_cast<FormulaWrapper*>(_ptr);
		return w ? w->expression.value() : 0.0;
	}

	bool FormulaHelper::Reparse(const std::string& input)
	{
		std::string err;
		if (!Validate(input, err))
			return false;
		if (_ptr)
			delete static_cast<FormulaWrapper*>(_ptr);
		_ptr = nullptr;
		return Parse(input);
	}

	bool FormulaHelper::Validate(const std::string& input, std::string& errorOut)
	{
		InitFormulaSystem();
		FormulaWrapper tmp;
		tmp.expression.register_symbol_table(s_symbolTable);
		if (tmp.parser.compile(input, tmp.expression))
			return true;
		if (tmp.parser.error_count() > 0)
			errorOut = tmp.parser.get_error(0).diagnostic;
		else
			errorOut = "Unknown parse error";
		return false;
	}

	void FormulaHelper::SetParam(int32_t index, double value) { s_formulaParams[index] = value; }
	double FormulaHelper::GetParam(int32_t index) { return s_formulaParams[index]; }

	// =========================================================================
	// Formula helpers
	//
	// SetupSceneFormula: called once per frame, sets camera/scene params.
	// SetupLightFormula: called per candidate light, sets all light params.
	// CalculateLightScore: evaluates s_formulaScore if available.
	// =========================================================================

	LightGeometry ComputeLightGeometry(const RE::BSShadowLight* light, const RE::NiCamera* camera, float lightRadius)
	{
		LightGeometry g{};
		if (!light)
			return g;
		const RE::NiLight* ni = light->light.get();
		if (!ni)
			return g;
		const auto& rtd = const_cast<RE::NiLight*>(ni)->GetLightRuntimeData();
		// Score on an EMA'd position anchor, not the live pose: flame flicker
		// translates the light several units every frame, jittering every
		// distance and area term below -- churning rank, redraw priority, and
		// the membership gates. Fade is NOT smoothed: a light fading out must
		// drop its score promptly, or it holds a shadow slot after it should
		// have left and its stale shadow flickers in. Rendering keeps the live
		// pose (shadows still dance).
		PruneIfOversized(s_scoreAnchor, 1024);
		const auto live = ni->world.translate;
		auto [anchorIt, anchorNew] = s_scoreAnchor.try_emplace(ni, live);
		if (!anchorNew) {
			anchorIt->second.x += 0.15f * (live.x - anchorIt->second.x);
			anchorIt->second.y += 0.15f * (live.y - anchorIt->second.y);
			anchorIt->second.z += 0.15f * (live.z - anchorIt->second.z);
		}
		const auto lp = anchorIt->second;

		// Perceptual luminance (Rec.709) x live fade. Valid even at zero
		// radius, where every geometric term below collapses to 0.
		g.lum = (0.2126f * rtd.diffuse.red + 0.7152f * rtd.diffuse.green + 0.0722f * rtd.diffuse.blue) * rtd.fade;
		if (lightRadius <= 0.0f)
			return g;

		// Corrects the omnidirectional-sphere assumption for spot/frustum lights
		// via coneFraction. Magnitude-only, not direction-gated: forward-axis
		// sign is unverified, and a wrong gate could invert rankings.
		const RE::BSShadowFrustumLight* frustumLight = skyrim_cast<const RE::BSShadowFrustumLight*>(light);
		float coneFraction = 1.0f;
		if (frustumLight) {
			// semiWidth/semiHeight are runtime-versioned (BSShadowFrustumLight::
			// RUNTIME_DATA); direct member access reads garbage (adjacent
			// BSTArray's heap pointer) in the multi-runtime layout.
			const auto& frustumRtd = frustumLight->GetShadowFrustumLightRuntimeData();
			if (frustumRtd.semiWidth > 0.0f && frustumRtd.semiHeight > 0.0f) {
				constexpr float kPi = 3.14159265358979323846f;
				const float w = frustumRtd.semiWidth, h = frustumRtd.semiHeight;
				const float sinX = w / std::sqrt(1.0f + w * w);
				const float sinY = h / std::sqrt(1.0f + h * h);
				coneFraction = std::clamp(std::asin(std::clamp(sinX * sinY, -1.0f, 1.0f)) / kPi, 0.0f, 1.0f);
			}
		}

		// Projected solid-angle proxy: angularRadius ~ radius/viewZ, coverage
		// ~ angularRadius^2 (Olsson & Assarsson 2012; CryEngine shadow LOD).
		// Screen constants drop out of the ranking. Camera intersecting the
		// sphere clamps effectiveZ to avoid blow-up; fully behind = 0.
		if (camera) {
			auto* cam = const_cast<RE::NiCamera*>(camera);
			const auto cp = camera->world.translate;
			const RE::NiPoint3 fwd = camera->world.rotate.GetVectorY();
			const float rx = lp.x - cp.x, ry = lp.y - cp.y, rz = lp.z - cp.z;
			const float viewZ = fwd.x * rx + fwd.y * ry + fwd.z * rz;
			if (viewZ > -lightRadius) {
				const float effectiveZ = std::max(viewZ, lightRadius * 0.5f);
				const float angularRadius = lightRadius / effectiveZ;
				g.coverage = angularRadius * angularRadius;
			}

			// Screen area [0,1]: fraction of the viewport the light's SPHERE projects
			// onto, clamped to the frustum -- unlike coverage/forwardness (light
			// CENTER), a light behind the camera whose sphere still reaches into
			// view keeps a large area. Industry pattern for shadow importance.
			const float dist = std::sqrt(rx * rx + ry * ry + rz * rz);
			if (dist < lightRadius + cam->GetNearPlane()) {
				g.screenArea = 1.0f;  // camera within the sphere: it fills the view
			} else {
				const float inv = 1.0f / dist;
				float coord[4] = { lp.x - rx * lightRadius * inv, lp.y - ry * lightRadius * inv,
					lp.z - rz * lightRadius * inv, lightRadius };
				float r1[2], r2[2];
				GameFrustumOverlap(cam, coord, r1, r2, 0.00001f);
				const float x0 = std::clamp(std::min(r1[0], r2[0]), -1.0f, 1.0f);
				const float x1 = std::clamp(std::max(r1[0], r2[0]), -1.0f, 1.0f);
				const float y0 = std::clamp(std::min(r1[1], r2[1]), -1.0f, 1.0f);
				const float y1 = std::clamp(std::max(r1[1], r2[1]), -1.0f, 1.0f);
				g.screenArea = std::clamp((x1 - x0) * (y1 - y0) * 0.25f, 0.0f, 1.0f);
			}
		}

		// coverage is a solid-angle proxy, so coneFraction applies directly;
		// screenArea is a 2D frustum-projected footprint and stays unscaled.
		g.coverage *= coneFraction;

		// Skyrim's quadratic falloff (1-(d/r)^2)^2 at camera and player: the
		// out-of-view floor and the carried-light signal, also scaled by
		// coneFraction since falloff alone cannot distinguish beam-elsewhere
		// from actually-lit within the sphere.
		auto computeAtt = [&](const RE::NiPoint3& pos) -> float {
			const float dist2 = pos.GetSquaredDistance(lp);
			const float r2 = lightRadius * lightRadius;
			if (dist2 >= r2)
				return 0.0f;
			const float a = 1.0f - dist2 / r2;
			return a * a;
		};
		auto* plr = RE::PlayerCharacter::GetSingleton();
		const float rawAttCam = camera ? computeAtt(camera->world.translate) : 0.0f;
		const float rawAttPlr = plr ? computeAtt(plr->GetPosition()) : rawAttCam;
		g.attCam = rawAttCam * coneFraction;
		g.attPlr = rawAttPlr * coneFraction;
		// Third person: a light enclosing the PLAYER dominates the view around
		// the player character even though the camera sits outside its sphere
		// (the carried-torch case) -- same enclosure rule as camera-inside.
		// First person degenerates to the camera test.
		if (g.screenArea < 1.0f && plr) {
			const auto pp = plr->GetPosition();
			const float px = pp.x - lp.x, py = pp.y - lp.y, pz = pp.z - lp.z;
			if (px * px + py * py + pz * pz < lightRadius * lightRadius)
				g.screenArea = 1.0f;
		}
		// sizeProxy is linear: coverage's sqrt already linearizes coneFraction, so
		// the raw (pre-coneFraction) attenuation takes the same sqrt rather than
		// the full area fraction g.attCam/g.attPlr carry.
		g.sizeProxy = std::max(sqrtf(g.coverage), std::max(rawAttCam, rawAttPlr) * sqrtf(coneFraction));
		return g;
	}

	// SEH in its own function (no C++ unwinding objects) so MSVC accepts
	// __try. NiAVObject::parent is not refcounted; during scene teardown a
	// live light's chain can dangle, so a miss must be a false, not a CTD.
	static bool WalkToPlayerRoot(const RE::NiAVObject* node, const RE::NiAVObject* root0, const RE::NiAVObject* root1) noexcept
	{
		__try {
			for (int depth = 0; node && depth < 64; node = node->parent, ++depth)
				if (node == root0 || node == root1)
					return true;
		} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		}
		return false;
	}

	bool IsPlayerAttachedLight(const RE::NiLight* ni)
	{
		if (!ni)
			return false;
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (!plr)
			return false;
		// Both 3D roots: a held torch parents under the active person's
		// skeleton (the first-person one while in first person).
		return WalkToPlayerRoot(ni, plr->Get3D(false), plr->Get3D(true));
	}

	void SetupSceneFormula(const RE::NiCamera* camera)
	{
		if (camera) {
			FormulaHelper::SetParam(kFormulaParam_CameraX, camera->world.translate.x);
			FormulaHelper::SetParam(kFormulaParam_CameraY, camera->world.translate.y);
			FormulaHelper::SetParam(kFormulaParam_CameraZ, camera->world.translate.z);
		} else {
			FormulaHelper::SetParam(kFormulaParam_CameraX, 0.0);
			FormulaHelper::SetParam(kFormulaParam_CameraY, 0.0);
			FormulaHelper::SetParam(kFormulaParam_CameraZ, 0.0);
		}

		FormulaHelper::SetParam(kFormulaParam_IsInterior, 0);
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (plr) {
			auto* cell = plr->parentCell;
			if (cell && cell->IsInteriorCell())
				FormulaHelper::SetParam(kFormulaParam_IsInterior, 1);
		}

		// Time of day from GameHour global
		auto* cal = RE::Calendar::GetSingleton();
		if (cal)
			FormulaHelper::SetParam(kFormulaParam_TimeOfDay, cal->GetHour());
	}

	void SetupLightFormula(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index)
	{
		FormulaHelper::SetParam(kFormulaParam_LightConverted, 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightIndex, index);
		FormulaHelper::SetParam(kFormulaParam_LightDisplacement, 0.0);    // overridden per-entry in redraw interval loop
		FormulaHelper::SetParam(kFormulaParam_LightDynamicCasters, 0.0);  // overridden per-entry in redraw interval loop
		FormulaHelper::SetParam(kFormulaParam_PlayerLightDistance, 0.0);  // overridden below after light position is known

		// Temporal stickiness signals. Both derived from the slot pool in one
		// pass: chosenLastFrame is the boolean kept for backward-compat with
		// user formulas; framesSinceRender is a continuous age that decays to
		// zero stickiness once the slot has been stale long enough to no
		// longer represent a true rank-drift case. Sentinel 1e6 covers the
		// "no slot" and "never rendered" branches so the default formula's
		// max(0, 1 - age/window) decay term cleanly collapses to 0.
		double chosenLastFrame = 0.0;
		double framesSinceRender = 1e6;
		{
			const int32_t now = *globals::game::frameCounter;
			for (int i = s_lights.PointLightFirst(); i < s_lights.PointLightEnd(s_settings.ShadowLightCount); i++) {
				const auto& e = s_lights.Lights[i];
				if (e.Light != light)
					continue;
				chosenLastFrame = 1.0;
				if (e.LastDrawnFrame >= 0)
					framesSinceRender = static_cast<double>(now - e.LastDrawnFrame);
				break;
			}
		}
		FormulaHelper::SetParam(kFormulaParam_LightChosenLastFrame, chosenLastFrame);
		FormulaHelper::SetParam(kFormulaParam_LightFramesSinceRender, framesSinceRender);

		FormulaHelper::SetParam(kFormulaParam_LightNeverFades, light->lodFade ? 0.0 : 1.0);
		FormulaHelper::SetParam(kFormulaParam_LightPortalStrict, light->portalStrict ? 1.0 : 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightNS, 0.0);

		// Spot detection + cone-aware visibility prior (option 1 from spot
		// preservation analysis). Non-spots get spotvisible=1 so existing
		// omni-tuned formulas are unaffected. For spots, we read last
		// frame's UpdateCamera verdict (frustumCull / lodDimmer) -- the
		// score runs BEFORE this frame's validation pass updates those,
		// but cameras move continuously so last-frame's cone-vs-frustum
		// is a strong predictor of this-frame's. Trading a one-frame lag
		// for not double-calling UpdateCamera is a worthwhile cost since
		// the score is a preference, not a gate.
		const bool isSpot = (skyrim_cast<const RE::BSShadowFrustumLight*>(light) != nullptr);
		double spotVisible = 1.0;  // default for non-spots: always "visible"
		if (isSpot) {
			// frustumCull == 0 means "in frustum"; engine sets 0xff when
			// cone-vs-frustum rejects. lodDimmer > 0 means the LOD fader
			// hasn't zeroed the light. Both must hold for a spot to count
			// as plausibly visible.
			// Note: the engine field is misspelled "frustrumCull" in the SDK
			// (matches Bethesda's original symbol). 0 = visible, 0xff = culled.
			const bool inFrustum = (light->frustrumCull == 0);
			const bool lodLit = (light->lodDimmer > 0.0f);
			spotVisible = (inFrustum && lodLit) ? 1.0 : 0.0;
		}
		FormulaHelper::SetParam(kFormulaParam_LightIsSpot, isSpot ? 1.0 : 0.0);
		FormulaHelper::SetParam(kFormulaParam_LightSpotVisible, spotVisible);
		FormulaHelper::SetParam(kFormulaParam_LightPlayerAttached, IsPlayerAttachedLight(light->light.get()) ? 1.0 : 0.0);

		float x, y, z;

		auto* nilight = light->light.get();
		if (nilight) {
			FormulaHelper::SetParam(kFormulaParam_LightIntensity, nilight->GetLightRuntimeData().fade);
			FormulaHelper::SetParam(kFormulaParam_LightRadius, nilight->GetLightRuntimeData().radius.x);
			FormulaHelper::SetParam(kFormulaParam_LightR, nilight->GetLightRuntimeData().diffuse.red);
			FormulaHelper::SetParam(kFormulaParam_LightG, nilight->GetLightRuntimeData().diffuse.green);
			FormulaHelper::SetParam(kFormulaParam_LightB, nilight->GetLightRuntimeData().diffuse.blue);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientR, nilight->GetLightRuntimeData().ambient.red);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientG, nilight->GetLightRuntimeData().ambient.green);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientB, nilight->GetLightRuntimeData().ambient.blue);
			x = nilight->world.translate.x;
			y = nilight->world.translate.y;
			z = nilight->world.translate.z;

			if (s_settings.PromoteNormalToShadow)
				FormulaHelper::SetParam(kFormulaParam_LightNS, IsPromotedLight(nilight) ? 1.0 : 0.0);

			const auto geom = ComputeLightGeometry(light, camera, nilight->GetLightRuntimeData().radius.x);
			FormulaHelper::SetParam(kFormulaParam_LightCoverage, geom.coverage);
			FormulaHelper::SetParam(kFormulaParam_LightScreenArea, geom.screenArea);
			FormulaHelper::SetParam(kFormulaParam_LightLum, geom.lum);
			FormulaHelper::SetParam(kFormulaParam_LightAttCam, geom.attCam);
			FormulaHelper::SetParam(kFormulaParam_LightAttPlayer, geom.attPlr);
		} else {
			FormulaHelper::SetParam(kFormulaParam_LightIntensity, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightRadius, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightR, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightG, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightB, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientR, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientG, 1.0);
			FormulaHelper::SetParam(kFormulaParam_LightAmbientB, 1.0);
			x = light->worldTranslate.x;
			y = light->worldTranslate.y;
			z = light->worldTranslate.z;
			FormulaHelper::SetParam(kFormulaParam_LightCoverage, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightScreenArea, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightLum, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightAttCam, 0.0);
			FormulaHelper::SetParam(kFormulaParam_LightAttPlayer, 0.0);
		}

		FormulaHelper::SetParam(kFormulaParam_LightX, x);
		FormulaHelper::SetParam(kFormulaParam_LightY, y);
		FormulaHelper::SetParam(kFormulaParam_LightZ, z);

		float camx = camera ? camera->world.translate.x : (float)FormulaHelper::GetParam(kFormulaParam_CameraX);
		float camy = camera ? camera->world.translate.y : (float)FormulaHelper::GetParam(kFormulaParam_CameraY);
		float camz = camera ? camera->world.translate.z : (float)FormulaHelper::GetParam(kFormulaParam_CameraZ);

		float dx = x - camx, dy = y - camy, dz = z - camz;
		FormulaHelper::SetParam(kFormulaParam_LightDistance, sqrtf(dx * dx + dy * dy + dz * dz));

		// Player-to-light distance: ensures third-person shadow maps redraw when the
		// player character is inside a light's radius even if the camera is outside.
		double playerLightDist = FormulaHelper::GetParam(kFormulaParam_LightDistance);
		auto* plr = RE::PlayerCharacter::GetSingleton();
		if (plr) {
			auto pp = plr->GetPosition();
			float pdx = x - pp.x, pdy = y - pp.y, pdz = z - pp.z;
			playerLightDist = static_cast<double>(sqrtf(pdx * pdx + pdy * pdy + pdz * pdz));
		}
		FormulaHelper::SetParam(kFormulaParam_PlayerLightDistance, playerLightDist);
	}

	double CalculateLightScore(const RE::BSShadowLight* light, const RE::NiCamera* camera, int32_t index, float* outImpact)
	{
		SetupLightFormula(light, camera, index);

		if (outImpact) {
			// Screen impact = the larger of "influence sphere fills the view"
			// and "lights the camera or player". Read back from the params
			// SetupLightFormula just set, so ComputeLightGeometry (and its EMA)
			// is not advanced a second time.
			const float sa = static_cast<float>(FormulaHelper::GetParam(kFormulaParam_LightScreenArea));
			const float ac = static_cast<float>(FormulaHelper::GetParam(kFormulaParam_LightAttCam));
			const float ap = static_cast<float>(FormulaHelper::GetParam(kFormulaParam_LightAttPlayer));
			*outImpact = std::max(sa, std::max(ac, ap));
		}

		if (s_formulaScore) {
			// Scores feed std::sort keys (selection, atlas budget) where a
			// NaN/inf violates strict weak ordering: UB, and in practice an
			// out-of-bounds introsort crash. Engine light data can be garbage
			// mid-load and user formulas can divide by zero; sanitize here.
			const double v = s_formulaScore->Calculate();
			return std::isfinite(v) ? v : 0.0;
		}

		return 0.0;
	}
}

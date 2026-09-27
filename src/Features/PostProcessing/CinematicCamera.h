#pragma once

namespace CinematicCamera
{
	constexpr float kMotionBlurReferenceScale = 300.0f;
	constexpr float kReferenceEV100 = 8.555816155f;

	enum class FilmbackPreset : int
	{
		FullFrame = 0,  // 36 x 24 mm
		Super35 = 1,    // 24.89 x 18.66 mm
		APSC = 2,       // 23.6 x 15.7 mm
		Custom = 3,
	};

	enum class GateFit : int
	{
		Horizontal = 0,  // sensor width maps to the horizontal FOV
		Vertical = 1,    // sensor height maps to the vertical FOV
	};

	enum class FocusMode : int
	{
		Manual = 0,
		ScreenPoint = 1,
		Target = 2,
	};

	enum class ExposureMode : int
	{
		AutoISO = 0,
		Manual = 1,
	};

	struct FilmbackSettings
	{
		int Preset = (int)FilmbackPreset::FullFrame;
		float SensorWidthMM = 36.0f;
		float SensorHeightMM = 24.0f;
		int GateFit = (int)GateFit::Horizontal;
	};

	struct LensSettings
	{
		float FocalLengthMM = 50.0f;
		float FNumber = 2.8f;
		int ApertureBladeCount = 6;
		float ApertureBladeRotationDeg = 0.0f;
		float ApertureRoundness = 0.5f;
	};

	struct FocusSettings
	{
		int Mode = (int)FocusMode::ScreenPoint;
		float ManualDistanceM = 10.0f;
		float2 ScreenPointUV = float2(0.5f, 0.5f);
		float TransitionSpeed = 0.5f;
	};

	struct ExposureSettings
	{
		int Mode = (int)ExposureMode::AutoISO;
		float ISO = 100.0f;
		float MinISO = 25.0f;
		float MaxISO = 12800.0f;
		float FrameRate = 24.0f;
		float ShutterAngleDeg = 180.0f;
		float ExposureCompensationEV = 0.0f;
	};

	struct Settings
	{
		bool Enabled = false;
		FilmbackSettings Filmback;
		LensSettings Lens;
		FocusSettings Focus;
		ExposureSettings Exposure;
	};

	struct PhysicalCameraState
	{
		bool Valid = false;

		float SensorWidthMM = 36.0f;
		float SensorHeightMM = 24.0f;
		float EffectiveSensorWidthMM = 36.0f;

		float FocalLengthMM = 50.0f;
		float FNumber = 2.8f;
		int ApertureBladeCount = 6;
		float ApertureBladeRotationDeg = 0.0f;  // degrees
		float ApertureRoundness = 0.5f;
		float EntrancePupilMM = 17.857f;

		FocusMode Mode = FocusMode::ScreenPoint;
		float ManualDistanceM = 10.0f;
		float2 ScreenPointUV = float2(0.5f, 0.5f);
		float TransitionSpeed = 0.5f;

		ExposureMode Exposure = ExposureMode::AutoISO;
		float ISO = 100.0f;
		float MinISO = 25.0f;
		float MaxISO = 12800.0f;
		float ExposureCompensationEV = 0.0f;
		float ShutterAngleDeg = 180.0f;
		float ShutterTimeS = 1.0f / 48.0f;
		float EV100 = kReferenceEV100;
		float ExposureDeltaEV = 0.0f;

		float HorizontalFOVDeg = 39.6f;
		float VerticalFOVDeg = 27.0f;
	};

	struct FocusResolver
	{
		/** @brief Resolves dialogue, TDM and console targets in priority order. */
		RE::TESObjectREFR* FindTarget(bool a_allowConsoleSelection, uint& a_currentRef);

		struct TargetFocusResult
		{
			bool hasTarget = false;  // a target (or held history) is available
			bool projected = false;  // target visible on screen -> autofocus at coord
			float2 focusCoord = { 0.5f, 0.5f };
			float distanceM = 10.0f;
		};

		/** @brief Resolves target focus, retaining the last valid input when lost. */
		TargetFocusResult ResolveTarget(bool a_allowConsoleSelection, uint& a_currentRef);

	private:
		bool hasValidHistory = false;
		bool historyProjected = false;
		float2 historyCoord = { 0.5f, 0.5f };
		float historyDistanceM = 10.0f;
	};

	struct Controller
	{
		Settings settings;

		FocusResolver focusResolver;

		PhysicalCameraState activeState{};
		bool stateValid = false;

		enum class FovState
		{
			Inactive,
			Applied,
			ExternallyModified,
			Suspended,
			Unavailable,
			Invalid
		};
		FovState fovState = FovState::Inactive;

		/** @brief Loads and validates physical camera settings. */
		void LoadSettings(json& o_json);
		/** @brief Saves validated physical camera settings. */
		void SaveSettings(json& o_json);
		/** @brief Restores disabled camera defaults. */
		void RestoreDefaultSettings();

		/** @brief Publishes effective camera state and conditionally owns flat FOV. */
		void Update(bool runnable, float viewportAspect);

		/** @brief Returns active physical parameters or null while suspended. */
		[[nodiscard]] const PhysicalCameraState* GetState() const { return stateValid ? &activeState : nullptr; }

		/** @brief Returns the localized projection ownership status. */
		[[nodiscard]] const char* GetFovStateText() const;

		/** @brief Draws camera parameters and effective optical readouts. */
		void DrawSettings();

	private:
		void DrawExposureSettings();
		[[nodiscard]] PhysicalCameraState BuildState(float viewportAspect) const;
		void ApplyFOV(float viewportAspect);
		void ValidateSettings();

		float restoreFOV = 0.0f;
		float lastAppliedFOV = 0.0f;
		float lastAppliedAspect = 0.0f;
		bool fovWritePending = true;
	};
}
